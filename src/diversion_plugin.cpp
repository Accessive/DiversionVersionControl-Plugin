#include "diversion_plugin.h"

#include "dv_cli.h"

#include <godot_cpp/variant/utility_functions.hpp>

using namespace godot;
using namespace diversion;

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

TypedArray<Dictionary> DiversionVCSPlugin::_get_modified_files_data() {
	// Phase 2: parse `dv status` / REST workspace status into status files.
	return TypedArray<Dictionary>();
}

void DiversionVCSPlugin::_stage_file(const String &p_file_path) {
	// Phase 2: add to the in-plugin staged set (Diversion has no staging).
}

void DiversionVCSPlugin::_unstage_file(const String &p_file_path) {
	// Phase 2: remove from the in-plugin staged set.
}

void DiversionVCSPlugin::_discard_file(const String &p_file_path) {
	// Phase 3: dv reset / dv restore.
	UtilityFunctions::push_warning("[Diversion] Discard is not implemented yet.");
}

void DiversionVCSPlugin::_commit(const String &p_msg) {
	// Phase 2: dv commit <staged files...> -m msg.
	UtilityFunctions::push_warning("[Diversion] Commit is not implemented yet.");
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
	// Phase 3: dv branch-name.
	return String();
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
