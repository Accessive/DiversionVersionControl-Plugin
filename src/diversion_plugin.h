#pragma once

#include "dv_cli.h"

#include <godot_cpp/classes/editor_vcs_interface.hpp>
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

	// Diversion has no staging area; the dock's stage/unstage selections are
	// tracked here and turned into the explicit file list for `dv commit`.
	HashSet<String> staged_files;

	// Cached identifiers from the last `dv status` parse.
	diversion::DvStatusInfo last_status;

	// Runs `dv status --no-limit`, refreshes `last_status`, and returns
	// whether the command succeeded.
	bool refresh_status();

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
