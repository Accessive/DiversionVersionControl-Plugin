#pragma once

#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/string.hpp>

namespace diversion {

struct SubprocessResult {
	// -1 means the process could not be started (exe missing, bad cwd, ...).
	int exit_code = -1;
	// stdout and stderr, merged in arrival order.
	godot::String output;
};

// Runs `exe args...` with the child's working directory set to `cwd` and
// blocks until it exits. Godot's OS::execute cannot set a child cwd, which
// dv requires for workspace detection, hence this native implementation.
SubprocessResult subprocess_run(const godot::String &exe, const godot::PackedStringArray &args, const godot::String &cwd);

} // namespace diversion
