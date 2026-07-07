#pragma once

#include "subprocess.h"

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

} // namespace diversion
