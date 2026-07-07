#include "dv_cli.h"

#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/os.hpp>

using namespace godot;

namespace diversion {

String DvCli::find_dv() {
#ifdef _WIN32
	String home = OS::get_singleton()->get_environment("USERPROFILE");
	String candidate = home.path_join(".diversion").path_join("bin").path_join("dv.exe");
	if (FileAccess::file_exists(candidate)) {
		return candidate;
	}
	return "dv.exe"; // Fall back to PATH lookup.
#else
	String home = OS::get_singleton()->get_environment("HOME");
	String candidate = home.path_join(".diversion").path_join("bin").path_join("dv");
	if (FileAccess::file_exists(candidate)) {
		return candidate;
	}
	return "dv";
#endif
}

SubprocessResult DvCli::run(const String &workspace_dir, const PackedStringArray &args) {
	return subprocess_run(find_dv(), args, workspace_dir);
}

PackedStringArray dv_args(std::initializer_list<String> parts) {
	PackedStringArray args;
	for (const String &part : parts) {
		args.push_back(part);
	}
	return args;
}

} // namespace diversion
