#include "softlock_plugin.h"

#include "dv_cli.h"

#include <godot_cpp/classes/accept_dialog.hpp>
#include <godot_cpp/classes/button.hpp>
#include <godot_cpp/classes/dir_access.hpp>
#include <godot_cpp/classes/editor_interface.hpp>
#include <godot_cpp/classes/editor_settings.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/h_box_container.hpp>
#include <godot_cpp/classes/label.hpp>
#include <godot_cpp/classes/line_edit.hpp>
#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/classes/tree.hpp>
#include <godot_cpp/classes/tree_item.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <chrono>

using namespace godot;
using namespace diversion;

static const char *TOKEN_SETTING = "diversion/api_token";
static const std::chrono::seconds POLL_INTERVAL(30);

void DiversionSoftLockPlugin::_bind_methods() {
	ClassDB::bind_method(D_METHOD("on_token_button"), &DiversionSoftLockPlugin::on_token_button);
	ClassDB::bind_method(D_METHOD("on_token_confirmed"), &DiversionSoftLockPlugin::on_token_confirmed);
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

	// Register the token setting if absent.
	Ref<EditorSettings> es = EditorInterface::get_singleton()->get_editor_settings();
	if (es.is_valid() && !es->has_setting(TOKEN_SETTING)) {
		es->set_setting(TOKEN_SETTING, "");
	}

	// --- Build the bottom panel UI ---
	panel = memnew(VBoxContainer);
	panel->set_custom_minimum_size(Vector2(0, 150));

	HBoxContainer *header = memnew(HBoxContainer);
	status_label = memnew(Label);
	status_label->set_text("Diversion locks: starting...");
	status_label->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	header->add_child(status_label);
	token_button = memnew(Button);
	token_button->set_text("Set API Token");
	token_button->connect("pressed", callable_mp(this, &DiversionSoftLockPlugin::on_token_button));
	header->add_child(token_button);
	panel->add_child(header);

	tree = memnew(Tree);
	tree->set_v_size_flags(Control::SIZE_EXPAND_FILL);
	tree->set_columns(3);
	tree->set_column_titles_visible(true);
	tree->set_column_title(0, "File");
	tree->set_column_title(1, "Edited by");
	tree->set_column_title(2, "Branch");
	panel->add_child(tree);

	token_dialog = memnew(AcceptDialog);
	token_dialog->set_title("Diversion API Token");
	token_field = memnew(LineEdit);
	token_field->set_placeholder("Paste your Diversion API token");
	token_field->set_secret(true);
	token_field->set_custom_minimum_size(Vector2(400, 0));
	token_dialog->add_child(token_field);
	token_dialog->connect("confirmed", callable_mp(this, &DiversionSoftLockPlugin::on_token_confirmed));
	panel->add_child(token_dialog);

	warn_dialog = memnew(AcceptDialog);
	warn_dialog->set_title("Diversion: File in use");
	panel->add_child(warn_dialog);

	add_control_to_bottom_panel(panel, "Diversion Locks");

	connect("scene_changed", callable_mp(this, &DiversionSoftLockPlugin::on_scene_changed));

	// Start the background poller.
	{
		std::lock_guard<std::mutex> lock(mutex);
		exit_requested = false;
		pending_token = read_token_setting();
		token_dirty = true;
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

String DiversionSoftLockPlugin::read_token_setting() {
	Ref<EditorSettings> es = EditorInterface::get_singleton()->get_editor_settings();
	String token;
	if (es.is_valid() && es->has_setting(TOKEN_SETTING)) {
		token = es->get_setting(TOKEN_SETTING);
	}
	// DEV fallback so the feature can be exercised before a token is entered
	// in Editor Settings. Harmless (reads the user's own dv token) but slated
	// for removal before release.
	if (token.is_empty()) {
		String tokfile = OS::get_singleton()->get_environment("USERPROFILE").path_join(".diversion").path_join("api_token.txt");
		if (FileAccess::file_exists(tokfile)) {
			token = FileAccess::get_file_as_string(tokfile).strip_edges();
		}
	}
	return token;
}

void DiversionSoftLockPlugin::_process(double delta) {
	poll_accum += delta;
	if (poll_accum < 1.0) {
		return;
	}
	poll_accum = 0.0;

	// Pick up a token change made in Editor Settings.
	String current = read_token_setting();
	{
		std::lock_guard<std::mutex> lock(mutex);
		if (current != pending_token) {
			pending_token = current;
			token_dirty = true;
			cv.notify_all();
		}
	}
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
		TreeItem *item = tree->create_item(root);
		item->set_text(0, from_dv_to_project(e.path));
		item->set_text(1, e.author);
		item->set_text(2, e.branch_name);
	}
}

void DiversionSoftLockPlugin::on_token_button() {
	token_field->set_text(read_token_setting());
	token_dialog->popup_centered();
}

void DiversionSoftLockPlugin::on_token_confirmed() {
	String token = token_field->get_text().strip_edges();
	Ref<EditorSettings> es = EditorInterface::get_singleton()->get_editor_settings();
	if (es.is_valid()) {
		es->set_setting(TOKEN_SETTING, token);
	}
	std::lock_guard<std::mutex> lock(mutex);
	pending_token = token;
	token_dirty = true;
	cv.notify_all();
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
		bool apply_tok = false;
		String tok;
		{
			std::unique_lock<std::mutex> lock(mutex);
			if (exit_requested) {
				return;
			}
			if (token_dirty) {
				tok = pending_token;
				token_dirty = false;
				apply_tok = true;
			}
		}
		if (apply_tok) {
			api.set_refresh_token(tok);
			repo_id = String();
			workspace_id = String();
		}

		poll_once(api, project_path, repo_id, workspace_id);

		std::unique_lock<std::mutex> lock(mutex);
		cv.wait_for(lock, POLL_INTERVAL, [this] { return exit_requested || token_dirty; });
		if (exit_requested) {
			return;
		}
	}
}

void DiversionSoftLockPlugin::poll_once(DiversionApi &api, String &project_path, String &repo_id, String &workspace_id) {
	if (!api.has_token()) {
		std::lock_guard<std::mutex> lock(mutex);
		status_message = "Diversion locks: no API token set. Click 'Set API Token' (from the Diversion web app: Avatar -> Integrations).";
		edits.clear();
		have_data = true;
		return;
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

	Vector<OtherEdit> result;
	String err;
	bool auth_invalid = false;
	bool ok = api.fetch_other_statuses(repo_id, workspace_id, result, err, auth_invalid);

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
