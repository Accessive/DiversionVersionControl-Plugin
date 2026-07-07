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

// Sample `dv status --no-limit` output:
//
//   In repo TheBankGame dv.repo.7af22abf-5829-4f0f-b942-a90509f3c73a
//   On branch player dv.branch.10
//   Cloud workspace is over commit dv.commit.2
//   Working in workspace test-game @ Jaydins-PC (dv.ws.ec8acafe-...)
//   Total modified paths: 10 (7 files)
//   New:
//   	 addons/diversion/plugin.gdextension
//   Modified:
//   	 project.godot
//   	 scenes/main.tscn -> scenes/debug_env.tscn
DvStatusInfo parse_dv_status(const String &output) {
	DvStatusInfo info;

	DvChange section = DvChange::MODIFIED;
	bool in_section = false;

	PackedStringArray lines = output.split("\n");
	for (int i = 0; i < lines.size(); i++) {
		const String &line = lines[i];
		String stripped = line.strip_edges();
		if (stripped.is_empty()) {
			continue;
		}

		bool indented = line.begins_with("\t") || line.begins_with(" ");

		if (!indented) {
			in_section = false;
			if (stripped.begins_with("In repo ")) {
				String rest = stripped.trim_prefix("In repo ");
				int split = rest.rfind(" ");
				if (split > 0) {
					info.repo_name = rest.substr(0, split);
					info.repo_id = rest.substr(split + 1);
				}
				info.valid = true;
			} else if (stripped.begins_with("On branch ")) {
				String rest = stripped.trim_prefix("On branch ");
				int split = rest.rfind(" ");
				if (split > 0) {
					info.branch_name = rest.substr(0, split);
					info.branch_id = rest.substr(split + 1);
				}
			} else if (stripped.contains("dv.commit.")) {
				int start = stripped.find("dv.commit.");
				String rest = stripped.substr(start);
				info.head_commit_id = rest.get_slice(" ", 0);
			} else if (stripped.begins_with("Working in workspace")) {
				int start = stripped.find("(dv.ws.");
				int end = stripped.find(")", start);
				if (start >= 0 && end > start) {
					info.workspace_id = stripped.substr(start + 1, end - start - 1);
				}
			} else if (stripped.ends_with(":")) {
				String name = stripped.trim_suffix(":").to_lower();
				in_section = true;
				if (name == "new") {
					section = DvChange::NEW;
				} else if (name == "modified") {
					section = DvChange::MODIFIED;
				} else if (name == "deleted") {
					section = DvChange::DELETED;
				} else if (name.contains("conflict") || name.contains("unmerged")) {
					section = DvChange::UNMERGED;
				} else {
					in_section = false; // Unknown section: ignore its entries.
				}
			}
			continue;
		}

		if (!in_section) {
			continue;
		}

		DvStatusEntry entry;
		entry.path = stripped;
		entry.dv_path = stripped;
		entry.change = section;
		int arrow = stripped.find(" -> ");
		if (arrow >= 0) {
			entry.change = DvChange::RENAMED;
			entry.dv_path = stripped.substr(arrow + 4).strip_edges();
		}
		info.entries.push_back(entry);
	}

	return info;
}

} // namespace diversion
