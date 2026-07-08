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
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/classes/tree.hpp>
#include <godot_cpp/classes/tree_item.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <chrono>

using namespace godot;
using namespace diversion;

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
	tree->set_columns(5);
	tree->set_column_titles_visible(true);
	tree->set_column_title(0, "File");
	tree->set_column_title(1, "Edited by");
	tree->set_column_title(2, "Branch");
	tree->set_column_title(3, "Status");
	tree->set_column_title(4, "Last touched");
	tree->set_column_expand(3, false);
	tree->set_column_expand(4, false);
	tree->set_column_custom_minimum_width(3, 90);
	tree->set_column_custom_minimum_width(4, 110);
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
	bool is_syncing;
	int done, total;
	String direction;
	{
		std::lock_guard<std::mutex> lock(mutex);
		snapshot = edits;
		message = status_message;
		ready = have_data;
		is_syncing = syncing;
		done = sync_done;
		total = sync_total;
		direction = sync_direction;
	}

	// Live sync progress (commit/pull/push transfer) takes priority in the
	// status line, since it's the feedback the user is waiting on.
	if (is_syncing) {
		String p = direction + " to Diversion";
		if (total > 0) {
			p += ": " + itos(done) + " / " + itos(total) + " files";
		} else {
			p += "...";
		}
		status_label->set_text(p);
	} else {
		status_label->set_text(message);
	}
	if (!ready) {
		return;
	}

	tree->clear();
	TreeItem *root = tree->create_item();
	Color warn(1.0, 0.75, 0.3);
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
		item->set_text(3, change_label(e));
		item->set_text(4, relative_time(e.mtime));
		// Actively-edited files (uncommitted in someone's live workspace) are
		// the real collision risk; make them stand out.
		if (!e.workspace_id.is_empty()) {
			item->set_custom_color(1, warn);
			item->set_custom_color(3, warn);
		}
	}
}

String DiversionSoftLockPlugin::change_label(const OtherEdit &e) const {
	// An empty workspace id means the change was already committed on another
	// branch rather than being edited live right now.
	if (e.workspace_id.is_empty()) {
		return "committed";
	}
	switch (e.status) {
		case 2:
			return "adding";
		case 4:
			return "deleting";
		default:
			return "editing";
	}
}

String DiversionSoftLockPlugin::relative_time(int64_t mtime) const {
	if (mtime <= 0) {
		return "-";
	}
	int64_t now = (int64_t)Time::get_singleton()->get_unix_time_from_system();
	int64_t d = now - mtime;
	if (d < 0) {
		d = 0;
	}
	if (d < 60) {
		return "just now";
	}
	if (d < 3600) {
		return itos(d / 60) + "m ago";
	}
	if (d < 86400) {
		return itos(d / 3600) + "h ago";
	}
	return itos(d / 86400) + "d ago";
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

	// Fast tick for live sync progress; the cloud lock fetch runs less often.
	const std::chrono::seconds TICK(2);
	const int LOCK_EVERY = 15; // ~30s
	int tick = 0;

	while (true) {
		{
			std::unique_lock<std::mutex> lock(mutex);
			if (exit_requested) {
				return;
			}
		}

		poll_sync(project_path, repo_id, workspace_id);
		if (tick == 0) {
			poll_once(api, project_path, repo_id, workspace_id);
		}
		tick = (tick + 1) % LOCK_EVERY;

		std::unique_lock<std::mutex> lock(mutex);
		cv.wait_for(lock, TICK, [this] { return exit_requested; });
		if (exit_requested) {
			return;
		}
	}
}

void DiversionSoftLockPlugin::poll_sync(const String &project_path, String &repo_id, String &workspace_id) {
	SubprocessResult st = DvCli::run(project_path, dv_args({ "status", "--nowait", "--sync-only" }));
	if (st.exit_code != 0) {
		return;
	}
	DvStatusInfo info = parse_dv_status(st.output);
	if (repo_id.is_empty()) {
		repo_id = info.repo_id;
	}
	if (workspace_id.is_empty()) {
		workspace_id = info.workspace_id;
	}
	std::lock_guard<std::mutex> lock(mutex);
	syncing = info.syncing;
	sync_done = info.sync_done;
	sync_total = info.sync_total;
	sync_direction = info.sync_direction.is_empty() ? String("Syncing") : info.sync_direction;
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
