#pragma once

#include "subprocess.h"

#include <godot_cpp/templates/vector.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/string.hpp>

namespace diversion {

// Thin wrapper around the `dv` command-line client. All commands run with the
// project directory as cwd, since dv locates the workspace from cwd.
class DvCli {
public:
	// Locates the dv executable: default install dir first, then PATH.
	static godot::String find_dv();

	// Runs `dv args...` inside `workspace_dir`. exit_code -1 = dv not runnable.
	static SubprocessResult run(const godot::String &workspace_dir, const godot::PackedStringArray &args);
};

// Convenience builder so call sites can write dv_args({"status", "--nowait"}).
godot::PackedStringArray dv_args(std::initializer_list<godot::String> parts);

enum class DvChange {
	NEW,
	MODIFIED,
	RENAMED,
	DELETED,
	UNMERGED,
};

struct DvStatusEntry {
	// Display path shown in the dock; for renames this is "old -> new".
	godot::String path;
	// Path to feed back to dv (the new name for renames).
	godot::String dv_path;
	DvChange change = DvChange::MODIFIED;
};

struct DvStatusInfo {
	bool valid = false;
	godot::String repo_name;
	godot::String repo_id; // dv.repo.<uuid>
	godot::String branch_name;
	godot::String branch_id; // dv.branch.<n>
	godot::String workspace_id; // dv.ws.<uuid>
	godot::String head_commit_id; // dv.commit.<n>
	godot::Vector<DvStatusEntry> entries;
};

// Parses the human-readable output of `dv status --no-limit`.
DvStatusInfo parse_dv_status(const godot::String &output);

} // namespace diversion
