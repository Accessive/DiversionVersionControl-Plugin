#include "softlock_plugin.h"

#include "dv_cli.h"

#include <godot_cpp/classes/accept_dialog.hpp>
#include <godot_cpp/classes/control.hpp>
#include <godot_cpp/classes/dir_access.hpp>
#include <godot_cpp/classes/editor_file_system.hpp>
#include <godot_cpp/classes/editor_interface.hpp>
#include <godot_cpp/classes/label.hpp>
#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/classes/tree.hpp>
#include <godot_cpp/classes/tree_item.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <chrono>

using namespace godot;
using namespace diversion;

static const std::chrono::seconds POLL_INTERVAL(30);

void DiversionSoftLockPlugin::_bind_methods() {
	ClassDB::bind_method(D_METHOD("on_scene_changed", "scene_root"), &DiversionSoftLockPlugin::on_scene_changed);
}

String DiversionSoftLockPlugin::_get_plugin_name() const {
	return "Diversion Locks";
}

void DiversionSoftLockPlugin::_enter_tree() {
	project_root = ProjectSettings::get_singleton()->globalize_path("res://").trim_suffix("/");

	// Find the Diversion workspace root so dv paths can map to res://.
	rel_prefix = String();
	String walk = project_root;
	while (!walk.is_empty()) {
		if (DirAccess::dir_exists_absolute(walk.path_join(".diversion"))) {
			if (walk != project_root) {
				rel_prefix = project_root.trim_prefix(walk).trim_prefix("/");
			}
			break;
		}
		String parent = walk.get_base_dir();
		if (parent == walk) {
			break;
		}
		walk = parent;
	}

	// --- Build the bottom panel UI ---
	panel = memnew(VBoxContainer);
	panel->set_custom_minimum_size(Vector2(0, 150));

	status_label = memnew(Label);
	status_label->set_text("Diversion locks: starting...");
	panel->add_child(status_label);

	tree = memnew(Tree);
	tree->set_v_size_flags(Control::SIZE_EXPAND_FILL);
	tree->set_columns(3);
	tree->set_column_titles_visible(true);
	tree->set_column_title(0, "File");
	tree->set_column_title(1, "Edited by");
	tree->set_column_title(2, "Branch");
	panel->add_child(tree);

	warn_dialog = memnew(AcceptDialog);
	warn_dialog->set_title("Diversion: File in use");
	panel->add_child(warn_dialog);

	add_control_to_bottom_panel(panel, "Diversion Locks");

	connect("scene_changed", callable_mp(this, &DiversionSoftLockPlugin::on_scene_changed));

	{
		std::lock_guard<std::mutex> lock(mutex);
		exit_requested = false;
	}
	worker = std::thread(&DiversionSoftLockPlugin::thread_loop, this);
}

void DiversionSoftLockPlugin::_exit_tree() {
	{
		std::lock_guard<std::mutex> lock(mutex);
		exit_requested = true;
	}
	cv.notify_all();
	if (worker.joinable()) {
		worker.join();
	}
	if (panel) {
		remove_control_from_bottom_panel(panel);
		panel->queue_free();
		panel = nullptr;
	}
}

void DiversionSoftLockPlugin::_process(double delta) {
	// Godot's Version Control dock only re-reads the file list on the editor's
	// filesystem_changed signal (there is no timer). Changes the Diversion sync
	// agent writes -- e.g. a teammate's pulled edits -- don't always trigger a
	// rescan, leaving the Commit dock stale until a manual refresh. A periodic
	// light source scan makes the editor notice them and refreshes the dock
	// (and imports synced assets) on its own.
	scan_accum += delta;
	if (scan_accum >= 5.0) {
		scan_accum = 0.0;
		EditorFileSystem *efs = EditorInterface::get_singleton()->get_resource_filesystem();
		if (efs && !efs->is_scanning()) {
			efs->scan_sources();
		}
	}

	poll_accum += delta;
	if (poll_accum < 1.0) {
		return;
	}
	poll_accum = 0.0;
	refresh_ui();
}

void DiversionSoftLockPlugin::refresh_ui() {
	Vector<OtherEdit> snapshot;
	String message;
	bool ready;
	{
		std::lock_guard<std::mutex> lock(mutex);
		snapshot = edits;
		message = status_message;
		ready = have_data;
	}

	status_label->set_text(message);
	if (!ready) {
		return;
	}

	tree->clear();
	TreeItem *root = tree->create_item();
	for (int i = 0; i < snapshot.size(); i++) {
		const OtherEdit &e = snapshot[i];
		String display = from_dv_to_project(e.path);
		if (display.is_empty()) {
			continue; // outside this project
		}
		TreeItem *item = tree->create_item(root);
		item->set_text(0, display);
		item->set_text(1, e.author);
		item->set_text(2, e.branch_name);
	}
}

void DiversionSoftLockPlugin::on_scene_changed(Node *scene_root) {
	if (!scene_root) {
		return;
	}
	String scene_path = scene_root->get_scene_file_path();
	if (scene_path.is_empty() || scene_path == warned_for_scene) {
		return;
	}

	Vector<OtherEdit> snapshot;
	{
		std::lock_guard<std::mutex> lock(mutex);
		snapshot = edits;
	}
	for (int i = 0; i < snapshot.size(); i++) {
		if (from_dv_to_project(snapshot[i].path) == scene_path) {
			warned_for_scene = scene_path;
			warn_dialog->set_text(String("This scene is being edited by ") + snapshot[i].author +
					" on branch '" + snapshot[i].branch_name + "'.\n\nYou can still edit it, but you may conflict when committing. Coordinate before making large changes.");
			warn_dialog->popup_centered();
			return;
		}
	}
}

String DiversionSoftLockPlugin::from_dv_to_project(const String &dv_path) const {
	String p = dv_path;
	if (!rel_prefix.is_empty()) {
		String prefix = rel_prefix + String("/");
		if (!p.begins_with(prefix)) {
			return String(); // outside this project
		}
		p = p.trim_prefix(prefix);
	}
	return String("res://") + p;
}

void DiversionSoftLockPlugin::thread_loop() {
	DiversionApi api;
	String project_path = project_root;
	String repo_id, workspace_id;

	while (true) {
		{
			std::unique_lock<std::mutex> lock(mutex);
			if (exit_requested) {
				return;
			}
		}

		poll_once(api, project_path, repo_id, workspace_id);

		std::unique_lock<std::mutex> lock(mutex);
		cv.wait_for(lock, POLL_INTERVAL, [this] { return exit_requested; });
		if (exit_requested) {
			return;
		}
	}
}

void DiversionSoftLockPlugin::poll_once(DiversionApi &api, String &project_path, String &repo_id, String &workspace_id) {
	if (!api.has_token()) {
		String cred_err;
		if (!api.load_local_credentials(cred_err)) {
			std::lock_guard<std::mutex> lock(mutex);
			status_message = String("Diversion locks: ") + cred_err;
			edits.clear();
			have_data = true;
			return;
		}
	}

	// Resolve repo/workspace ids once via dv status.
	if (repo_id.is_empty() || workspace_id.is_empty()) {
		SubprocessResult st = DvCli::run(project_path, dv_args({ "status", "--nowait", "--no-limit" }));
		if (st.exit_code == 0) {
			DvStatusInfo info = parse_dv_status(st.output);
			repo_id = info.repo_id;
			workspace_id = info.workspace_id;
		}
		if (repo_id.is_empty() || workspace_id.is_empty()) {
			std::lock_guard<std::mutex> lock(mutex);
			status_message = "Diversion locks: could not determine repo/workspace (is this a Diversion workspace?).";
			have_data = true;
			return;
		}
	}

	Vector<OtherEdit> raw;
	String err;
	bool auth_invalid = false;
	bool ok = api.fetch_other_statuses(repo_id, workspace_id, raw, err, auth_invalid);

	// Drop noise the team never edits collaboratively: the plugin's own files
	// (its binary lives in the repo and legitimately differs across branches)
	// and Godot's transient "~"-prefixed hot-reload library copies. Without
	// this the panel is dominated by the plugin reporting itself.
	Vector<OtherEdit> result;
	for (int i = 0; i < raw.size(); i++) {
		const String &p = raw[i].path;
		if (p.get_file().begins_with("~")) {
			continue;
		}
		if (p.contains("addons/diversion/")) {
			continue;
		}
		result.push_back(raw[i]);
	}

	std::lock_guard<std::mutex> lock(mutex);
	if (!ok) {
		status_message = String("Diversion locks: ") + err;
		if (auth_invalid) {
			edits.clear();
		}
		have_data = true;
		return;
	}
	edits = result;
	have_data = true;
	if (result.is_empty()) {
		status_message = "Diversion locks: no files are being edited by others.";
	} else {
		status_message = String("Diversion locks: ") + itos(result.size()) + " file(s) being edited by others.";
	}
}
