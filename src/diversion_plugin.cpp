#include "diversion_plugin.h"

#include "diff_utils.h"
#include "dv_cli.h"

#include <godot_cpp/classes/dir_access.hpp>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

using namespace godot;
using namespace diversion;

// The dock passes back the display path; renames are shown as "old -> new"
// but dv expects the new name.
static String dv_path_of(const String &display_path) {
	int arrow = display_path.find(" -> ");
	if (arrow >= 0) {
		return display_path.substr(arrow + 4).strip_edges();
	}
	return display_path;
}

void DiversionVCSPlugin::_bind_methods() {
}

bool DiversionVCSPlugin::_initialize(const String &p_project_path) {
	project_path = p_project_path;

	SubprocessResult version = DvCli::run(project_path, dv_args({ "version" }));
	if (version.exit_code != 0) {
		popup_error("Diversion CLI (dv) was not found or is not working.\n\nInstall it from https://www.diversion.dev, then run 'dv login' in a terminal and restart the editor.");
		return false;
	}

	SubprocessResult status = DvCli::run(project_path, dv_args({ "status", "--nowait", "--sync-only" }));
	if (status.exit_code != 0) {
		popup_error(String("This project is not inside a Diversion workspace, or you are not logged in.\n\nRun 'dv login' and then 'dv init' or 'dv clone' for this project folder.\n\ndv said:\n") + status.output.strip_edges());
		return false;
	}

	UtilityFunctions::print("[Diversion] Plugin initialized (dv ", version.output.strip_edges(), ") for ", project_path);
	if (refresh_status()) {
		UtilityFunctions::print("[Diversion] Repo '", last_status.repo_name, "', branch '", last_status.branch_name, "', ", last_status.entries.size(), " modified path(s).");
	}
	return true;
}

bool DiversionVCSPlugin::_shut_down() {
	return true;
}

String DiversionVCSPlugin::_get_vcs_name() {
	return "Diversion";
}

void DiversionVCSPlugin::_set_credentials(const String &p_username, const String &p_password, const String &p_ssh_public_key_path, const String &p_ssh_private_key_path, const String &p_ssh_passphrase) {
	// Authentication is handled by 'dv login' (the sync agent stores the
	// session). Nothing to store here for now; an API-token override may be
	// accepted via the password field in a later phase.
}

bool DiversionVCSPlugin::refresh_status() {
	SubprocessResult res = DvCli::run(project_path, dv_args({ "status", "--no-limit" }));
	if (res.exit_code != 0) {
		UtilityFunctions::push_warning("[Diversion] dv status failed: ", res.output.strip_edges());
		return false;
	}
	last_status = parse_dv_status(res.output);
	return last_status.valid;
}

TypedArray<Dictionary> DiversionVCSPlugin::_get_modified_files_data() {
	TypedArray<Dictionary> result;
	if (!refresh_status()) {
		return result;
	}

	HashSet<String> still_modified;
	for (const DvStatusEntry &entry : last_status.entries) {
		// dv lists directories as their own entries; the dock only deals in
		// files, so skip paths that exist locally as directories.
		if (DirAccess::dir_exists_absolute(project_path.path_join(entry.dv_path))) {
			continue;
		}

		ChangeType change = CHANGE_TYPE_MODIFIED;
		switch (entry.change) {
			case DvChange::NEW:
				change = CHANGE_TYPE_NEW;
				break;
			case DvChange::MODIFIED:
				change = CHANGE_TYPE_MODIFIED;
				break;
			case DvChange::RENAMED:
				change = CHANGE_TYPE_RENAMED;
				break;
			case DvChange::DELETED:
				change = CHANGE_TYPE_DELETED;
				break;
			case DvChange::UNMERGED:
				change = CHANGE_TYPE_UNMERGED;
				break;
		}

		still_modified.insert(entry.path);
		TreeArea area = staged_files.has(entry.path) ? TREE_AREA_STAGED : TREE_AREA_UNSTAGED;
		result.push_back(create_status_file(entry.path, change, area));
	}

	// Drop staged entries that are no longer modified (committed or reverted
	// behind our back).
	Vector<String> stale;
	for (const String &path : staged_files) {
		if (!still_modified.has(path)) {
			stale.push_back(path);
		}
	}
	for (const String &path : stale) {
		staged_files.erase(path);
	}

	return result;
}

void DiversionVCSPlugin::_stage_file(const String &p_file_path) {
	staged_files.insert(p_file_path);
}

void DiversionVCSPlugin::_unstage_file(const String &p_file_path) {
	staged_files.erase(p_file_path);
}

void DiversionVCSPlugin::_discard_file(const String &p_file_path) {
	// --clean also deletes files that were newly added, matching the dock's
	// expectation that discarding a new file removes it.
	SubprocessResult res = DvCli::run(project_path, dv_args({ "reset", dv_path_of(p_file_path), "-f", "--clean" }));
	if (res.exit_code != 0) {
		popup_error(String("Could not discard '") + p_file_path + "'.\n\ndv said:\n" + res.output.strip_edges());
	}
}

void DiversionVCSPlugin::_commit(const String &p_msg) {
	if (staged_files.is_empty()) {
		popup_error("No files are staged. Stage the files you want to commit first.");
		return;
	}

	PackedStringArray args;
	args.push_back("commit");
	for (const String &path : staged_files) {
		args.push_back(dv_path_of(path));
	}
	args.push_back("-m");
	args.push_back(p_msg);

	SubprocessResult res = DvCli::run(project_path, args);
	if (res.exit_code != 0) {
		popup_error(String("Commit failed.\n\ndv said:\n") + res.output.strip_edges());
		return;
	}

	UtilityFunctions::print("[Diversion] Committed ", staged_files.size(), " file(s): ", res.output.strip_edges());
	staged_files.clear();
}

TypedArray<Dictionary> DiversionVCSPlugin::_get_diff(const String &p_identifier, int32_t p_area) {
	SubprocessResult res;
	if (p_area == TREE_AREA_COMMIT) {
		// The dock always lists history (which fills commit_parents) before a
		// commit can be selected, but refresh once in case that assumption
		// ever breaks.
		if (!commit_parents.has(p_identifier)) {
			_get_previous_commits(100);
		}
		if (!commit_parents.has(p_identifier) || commit_parents[p_identifier].is_empty()) {
			return TypedArray<Dictionary>(); // Root commit or unknown id.
		}
		res = DvCli::run(project_path, dv_args({ "diff", "--base", commit_parents[p_identifier], "--compare", p_identifier, "--color", "never" }));
	} else {
		// Staged and unstaged both mean "workspace vs base" in Diversion.
		res = DvCli::run(project_path, dv_args({ "diff", dv_path_of(p_identifier), "--color", "never" }));
	}

	if (res.exit_code != 0) {
		UtilityFunctions::push_warning("[Diversion] dv diff failed: ", res.output.strip_edges());
		return TypedArray<Dictionary>();
	}
	return parse_unified_diff(this, res.output);
}

TypedArray<Dictionary> DiversionVCSPlugin::_get_line_diff(const String &p_file_path, const String &p_text) {
	// Diffs the file as saved on disk against the workspace base. Unsaved
	// buffer edits (p_text) are not considered yet; the gutter catches up on
	// save. A local text diff against the committed base can lift this later.
	SubprocessResult res = DvCli::run(project_path, dv_args({ "diff", p_file_path, "--color", "never" }));
	if (res.exit_code != 0) {
		return TypedArray<Dictionary>();
	}
	return parse_unified_diff_hunks(this, res.output);
}

TypedArray<Dictionary> DiversionVCSPlugin::_get_previous_commits(int32_t p_max_commits) {
	TypedArray<Dictionary> result;

	SubprocessResult res = DvCli::run(project_path, dv_args({ "log", "-n", String::num_int64(p_max_commits), "--date", "iso" }));
	if (res.exit_code != 0) {
		UtilityFunctions::push_warning("[Diversion] dv log failed: ", res.output.strip_edges());
		return result;
	}

	// Blocks look like:
	//   commit dv.commit.3 (dv.branch.10)
	//   Author: Jaydin <jaydin.gulley@outlook.com>
	//   Date:   2026-07-07T04:44:14Z
	//
	//   \tMessage line(s)
	String id, author, date_iso, msg, prev_id;
	auto flush = [&]() {
		if (id.is_empty()) {
			return;
		}
		int64_t unix_time = Time::get_singleton()->get_unix_time_from_datetime_string(date_iso.trim_suffix("Z"));
		result.push_back(create_commit(msg.strip_edges(), author, id, unix_time, 0));
		// dv log is newest-first, so the entry after `prev_id` is its parent.
		if (!prev_id.is_empty()) {
			commit_parents[prev_id] = id;
		}
		commit_parents[id] = String(); // Until a following entry proves otherwise.
		prev_id = id;
		id = author = date_iso = msg = String();
	};

	PackedStringArray lines = res.output.split("\n");
	for (int i = 0; i < lines.size(); i++) {
		String line = lines[i];
		String stripped = line.strip_edges();
		if (stripped.begins_with("commit ")) {
			flush();
			id = stripped.trim_prefix("commit ").get_slice(" ", 0);
		} else if (stripped.begins_with("Author: ")) {
			author = stripped.trim_prefix("Author: ");
		} else if (stripped.begins_with("Date: ")) {
			date_iso = stripped.trim_prefix("Date: ").strip_edges();
		} else if ((line.begins_with("\t") || line.begins_with("    ")) && !stripped.is_empty()) {
			if (!msg.is_empty()) {
				msg += "\n";
			}
			msg += stripped;
		}
	}
	flush();

	return result;
}

TypedArray<String> DiversionVCSPlugin::_get_branch_list() {
	TypedArray<String> result;

	SubprocessResult res = DvCli::run(project_path, dv_args({ "branch" }));
	if (res.exit_code != 0) {
		UtilityFunctions::push_warning("[Diversion] dv branch failed: ", res.output.strip_edges());
		return result;
	}

	// Lines look like: "branch main (dv.branch.1)".
	PackedStringArray lines = res.output.split("\n");
	for (int i = 0; i < lines.size(); i++) {
		String stripped = lines[i].strip_edges();
		if (!stripped.begins_with("branch ")) {
			continue;
		}
		String rest = stripped.trim_prefix("branch ");
		int paren = rest.rfind(" (");
		if (paren > 0) {
			rest = rest.substr(0, paren);
		}
		result.push_back(rest);
	}
	return result;
}

String DiversionVCSPlugin::_get_current_branch_name() {
	if (!last_status.branch_name.is_empty()) {
		return last_status.branch_name;
	}
	SubprocessResult res = DvCli::run(project_path, dv_args({ "branch-name" }));
	if (res.exit_code != 0) {
		return String();
	}
	return res.output.strip_edges();
}

void DiversionVCSPlugin::_create_branch(const String &p_branch_name) {
	// --no-checkout: the dock treats create and checkout as separate actions.
	SubprocessResult res = DvCli::run(project_path, dv_args({ "branch", "-c", p_branch_name, "--no-checkout" }));
	if (res.exit_code != 0) {
		popup_error(String("Could not create branch '") + p_branch_name + "'.\n\ndv said:\n" + res.output.strip_edges());
	}
}

void DiversionVCSPlugin::_remove_branch(const String &p_branch_name) {
	// -f: dv prompts for confirmation otherwise, and there is no stdin.
	SubprocessResult res = DvCli::run(project_path, dv_args({ "branch", "-d", p_branch_name, "-f" }));
	if (res.exit_code != 0) {
		popup_error(String("Could not delete branch '") + p_branch_name + "'.\n\ndv said:\n" + res.output.strip_edges());
	}
}

bool DiversionVCSPlugin::_checkout_branch(const String &p_branch_name) {
	// --take-changes carries uncommitted work to the target branch, matching
	// how the git plugin's checkout behaves.
	SubprocessResult res = DvCli::run(project_path, dv_args({ "checkout", p_branch_name, "--take-changes" }));
	if (res.exit_code != 0) {
		popup_error(String("Could not check out branch '") + p_branch_name + "'.\n\ndv said:\n" + res.output.strip_edges());
		return false;
	}
	last_status.branch_name = p_branch_name;
	UtilityFunctions::print("[Diversion] Checked out '", p_branch_name, "': ", res.output.strip_edges());
	return true;
}

TypedArray<String> DiversionVCSPlugin::_get_remotes() {
	// Diversion manages transport itself; expose one pseudo-remote so the
	// editor's remote dropdown has a valid entry.
	TypedArray<String> remotes;
	remotes.push_back("diversion-cloud");
	return remotes;
}

void DiversionVCSPlugin::_create_remote(const String &p_remote_name, const String &p_remote_url) {
	popup_error("Diversion manages its cloud remote automatically; custom remotes are not supported.");
}

void DiversionVCSPlugin::_remove_remote(const String &p_remote_name) {
	popup_error("Diversion manages its cloud remote automatically; custom remotes are not supported.");
}

void DiversionVCSPlugin::_pull(const String &p_remote) {
	SubprocessResult res = DvCli::run(project_path, dv_args({ "update" }));
	if (res.exit_code != 0) {
		popup_error(String("Update failed.\n\ndv said:\n") + res.output.strip_edges());
		return;
	}
	UtilityFunctions::print("[Diversion] Updated workspace: ", res.output.strip_edges());
}

void DiversionVCSPlugin::_push(const String &p_remote, bool p_force) {
	// Diversion's sync agent uploads changes continuously; committing is the
	// action that publishes to the branch. Nothing to do.
	UtilityFunctions::print("[Diversion] Push is not needed: the sync agent uploads changes automatically.");
}

void DiversionVCSPlugin::_fetch(const String &p_remote) {
	// There is no fetch in Diversion; refresh cached state so the dock's
	// numbers are current.
	refresh_status();
}
