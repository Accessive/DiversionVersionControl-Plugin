#include "diversion_plugin.h"

#include "dv_cli.h"

#include <godot_cpp/classes/dir_access.hpp>
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
	// Phase 3: dv reset / dv restore.
	UtilityFunctions::push_warning("[Diversion] Discard is not implemented yet.");
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
	// Phase 3: dv diff -> unified diff -> diff dictionaries.
	return TypedArray<Dictionary>();
}

TypedArray<Dictionary> DiversionVCSPlugin::_get_line_diff(const String &p_file_path, const String &p_text) {
	// Phase 3: local diff of the editor buffer against the committed base.
	return TypedArray<Dictionary>();
}

TypedArray<Dictionary> DiversionVCSPlugin::_get_previous_commits(int32_t p_max_commits) {
	// Phase 3: dv log / REST commits endpoint.
	return TypedArray<Dictionary>();
}

TypedArray<String> DiversionVCSPlugin::_get_branch_list() {
	// Phase 3: dv branch list / REST list-branches.
	return TypedArray<String>();
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
	UtilityFunctions::push_warning("[Diversion] Branch creation is not implemented yet.");
}

void DiversionVCSPlugin::_remove_branch(const String &p_branch_name) {
	UtilityFunctions::push_warning("[Diversion] Branch deletion is not implemented yet.");
}

bool DiversionVCSPlugin::_checkout_branch(const String &p_branch_name) {
	UtilityFunctions::push_warning("[Diversion] Branch checkout is not implemented yet.");
	return false;
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
	// Phase 3: dv update (sync workspace to branch head).
	UtilityFunctions::push_warning("[Diversion] Update is not implemented yet.");
}

void DiversionVCSPlugin::_push(const String &p_remote, bool p_force) {
	// Diversion's sync agent uploads changes continuously; committing is the
	// action that publishes to the branch. Nothing to do.
	UtilityFunctions::print("[Diversion] Push is not needed: the sync agent uploads changes automatically.");
}

void DiversionVCSPlugin::_fetch(const String &p_remote) {
	// Phase 3/4: refresh cached status; report if workspace is behind head.
}
