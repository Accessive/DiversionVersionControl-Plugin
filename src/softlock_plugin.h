#pragma once

#include "diversion_api.h"

#include <godot_cpp/classes/editor_plugin.hpp>
#include <godot_cpp/classes/v_box_container.hpp>

#include <condition_variable>
#include <mutex>
#include <thread>

namespace godot {

class Tree;
class Label;
class Button;
class AcceptDialog;
class LineEdit;

// Companion editor plugin (separate from the VCS integration) that surfaces
// Diversion's free-tier soft locks: it polls the other_statuses API for files
// being edited in other workspaces/branches and warns before you collide.
//
// Lives in its own bottom panel and pops a dialog when you open a scene that
// someone else is already editing. Dormant (with a hint) until an API token is
// set in Editor Settings under "diversion/api_token".
class DiversionSoftLockPlugin : public EditorPlugin {
	GDCLASS(DiversionSoftLockPlugin, EditorPlugin)

	// --- UI (main thread) ---
	VBoxContainer *panel = nullptr;
	Label *status_label = nullptr;
	Tree *tree = nullptr;
	Button *token_button = nullptr;
	AcceptDialog *token_dialog = nullptr;
	LineEdit *token_field = nullptr;
	AcceptDialog *warn_dialog = nullptr;

	double poll_accum = 0.0;
	String warned_for_scene; // avoid repeating the warning for the same scene

	// --- Shared state (guarded by mutex) ---
	std::thread worker;
	std::mutex mutex;
	std::condition_variable cv;
	bool exit_requested = false;
	bool refresh_requested = false;
	String pending_token; // main thread -> worker
	bool token_dirty = false;

	Vector<diversion::OtherEdit> edits; // worker -> main
	String status_message;
	bool have_data = false;

	void thread_loop();
	void poll_once(diversion::DiversionApi &api, String &project_path, String &repo_id, String &workspace_id);

	String read_token_setting();
	void apply_token(const String &token);
	void refresh_ui();
	void on_token_button();
	void on_token_confirmed();
	void on_scene_changed(Node *scene_root);

	// workspace-relative dv path -> res:// for matching scene paths
	String from_dv_to_project(const String &dv_path) const;

	String project_root; // absolute path of the Godot project
	String rel_prefix; // project dir relative to workspace root

protected:
	static void _bind_methods();

public:
	void _enter_tree() override;
	void _exit_tree() override;
	void _process(double delta) override;
	String _get_plugin_name() const override;
};

} // namespace godot
