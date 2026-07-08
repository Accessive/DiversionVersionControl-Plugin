#pragma once

#include "dv_cli.h"
#include "status_poller.h"

#include <godot_cpp/classes/editor_vcs_interface.hpp>
#include <godot_cpp/templates/hash_map.hpp>
#include <godot_cpp/templates/hash_set.hpp>
#include <godot_cpp/variant/typed_array.hpp>

namespace godot {

// Diversion (diversion.dev) backend for Godot's built-in version control UI.
//
// Model mapping notes: Diversion has auto-syncing workspaces and no staging
// area, push, or remotes. Staging is emulated with an in-plugin set that
// selects which files go into `dv commit`; _push is a no-op (the sync agent
// uploads continuously) and _pull maps to `dv update`.
class DiversionVCSPlugin : public EditorVCSInterface {
	GDCLASS(DiversionVCSPlugin, EditorVCSInterface)

	String project_path;

	// Prefix of the project dir relative to the Diversion workspace root
	// (empty when they coincide). dv reports workspace-relative paths; the
	// dock wants project-relative ones.
	String rel_prefix;

	// Diversion has no staging area; the dock's stage/unstage selections are
	// tracked here and turned into the explicit file list for `dv commit`.
	HashSet<String> staged_files;
	// Consecutive status queries in which a staged file was absent; used to
	// avoid unstaging on a single transient mid-sync snapshot.
	HashMap<String, int> staged_miss;

	// Cached identifiers from the last `dv status` parse.
	diversion::DvStatusInfo last_status;

	// commit id -> parent commit id, learned from the last `dv log` parse so
	// the dock's commit view can be answered with `dv diff --base parent`.
	HashMap<String, String> commit_parents;

	diversion::StatusPoller poller;
	bool reported_status_failure = false;

	// Blocking status refresh (used right after mutating operations).
	bool refresh_status();

	// dv commands return before `dv status` reflects them (the sync agent
	// settles asynchronously), but the dock re-queries the file list
	// immediately after commit/discard. Polls briefly until none of the given
	// workspace-relative paths appear in the changelist anymore, so that
	// re-query sees settled state instead of needing a manual refresh.
	void wait_until_paths_settle(const PackedStringArray &dv_paths);

	// Converts a dock/editor path (project-relative or res://) into the
	// workspace-relative path dv expects.
	String to_dv_path(const String &display_path) const;

	// Converts a workspace-relative dv path into a project-relative one;
	// returns empty for paths outside the project directory.
	String from_dv_path(const String &dv_path) const;

protected:
	static void _bind_methods();

public:
	bool _initialize(const String &p_project_path) override;
	bool _shut_down() override;
	String _get_vcs_name() override;
	void _set_credentials(const String &p_username, const String &p_password, const String &p_ssh_public_key_path, const String &p_ssh_private_key_path, const String &p_ssh_passphrase) override;

	TypedArray<Dictionary> _get_modified_files_data() override;
	void _stage_file(const String &p_file_path) override;
	void _unstage_file(const String &p_file_path) override;
	void _discard_file(const String &p_file_path) override;
	void _commit(const String &p_msg) override;

	TypedArray<Dictionary> _get_diff(const String &p_identifier, int32_t p_area) override;
	TypedArray<Dictionary> _get_line_diff(const String &p_file_path, const String &p_text) override;
	TypedArray<Dictionary> _get_previous_commits(int32_t p_max_commits) override;

	TypedArray<String> _get_branch_list() override;
	String _get_current_branch_name() override;
	void _create_branch(const String &p_branch_name) override;
	void _remove_branch(const String &p_branch_name) override;
	bool _checkout_branch(const String &p_branch_name) override;

	TypedArray<String> _get_remotes() override;
	void _create_remote(const String &p_remote_name, const String &p_remote_url) override;
	void _remove_remote(const String &p_remote_name) override;
	void _pull(const String &p_remote) override;
	void _push(const String &p_remote, bool p_force) override;
	void _fetch(const String &p_remote) override;
};

} // namespace godot
