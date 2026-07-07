#include "diff_utils.h"

using namespace godot;

namespace diversion {

namespace {

struct DiffParseState {
	EditorVCSInterface *iface = nullptr;

	TypedArray<Dictionary> files;
	TypedArray<Dictionary> all_hunks; // Flat hunk list for _get_line_diff.

	Dictionary file;
	bool has_file = false;
	String old_name;
	String new_name;

	TypedArray<Dictionary> file_hunks;
	Dictionary hunk;
	bool has_hunk = false;
	TypedArray<Dictionary> hunk_lines;

	int old_line = 0;
	int new_line = 0;

	void flush_hunk() {
		if (!has_hunk) {
			return;
		}
		Dictionary complete = iface->add_line_diffs_into_diff_hunk(hunk, hunk_lines);
		file_hunks.push_back(complete);
		all_hunks.push_back(complete);
		hunk_lines = TypedArray<Dictionary>();
		has_hunk = false;
	}

	void flush_file() {
		flush_hunk();
		if (!has_file) {
			return;
		}
		files.push_back(iface->add_diff_hunks_into_diff_file(file, file_hunks));
		file_hunks = TypedArray<Dictionary>();
		has_file = false;
	}
};

// Strips the "a/" or "b/" prefix git-style names carry; "/dev/null" becomes
// an empty name (new or deleted file).
String clean_diff_name(const String &raw) {
	if (raw == "/dev/null") {
		return String();
	}
	if (raw.begins_with("a/") || raw.begins_with("b/")) {
		return raw.substr(2);
	}
	return raw;
}

// "-31,6" or "+31" -> start/count (count defaults to 1).
void parse_range(const String &token, int &r_start, int &r_count) {
	String body = token.substr(1); // Drop the sign.
	int comma = body.find(",");
	if (comma < 0) {
		r_start = body.to_int();
		r_count = 1;
	} else {
		r_start = body.substr(0, comma).to_int();
		r_count = body.substr(comma + 1).to_int();
	}
}

void parse_into(DiffParseState &state, const String &diff_text) {
	PackedStringArray lines = diff_text.split("\n");
	for (int i = 0; i < lines.size(); i++) {
		String line = lines[i];
		if (line.ends_with("\r")) {
			line = line.substr(0, line.length() - 1);
		}

		if (line.begins_with("diff --git")) {
			state.flush_file();
			continue;
		}
		if (line.begins_with("--- ")) {
			state.old_name = clean_diff_name(line.substr(4).strip_edges());
			continue;
		}
		if (line.begins_with("+++ ")) {
			state.new_name = clean_diff_name(line.substr(4).strip_edges());
			state.file = state.iface->create_diff_file(state.new_name, state.old_name);
			state.has_file = true;
			continue;
		}
		if (line.begins_with("@@")) {
			state.flush_hunk();
			// "@@ -31,6 +31,11 @@ optional context"
			String old_token = line.get_slice(" ", 1);
			String new_token = line.get_slice(" ", 2);
			int old_start = 0, old_count = 0, new_start = 0, new_count = 0;
			parse_range(old_token, old_start, old_count);
			parse_range(new_token, new_start, new_count);
			state.hunk = state.iface->create_diff_hunk(old_start, new_start, old_count, new_count);
			state.has_hunk = true;
			state.old_line = old_start;
			state.new_line = new_start;
			continue;
		}
		if (!state.has_hunk) {
			continue;
		}
		if (line.begins_with("\\")) {
			continue; // "\ No newline at end of file"
		}
		if (line.begins_with("+")) {
			state.hunk_lines.push_back(state.iface->create_diff_line(state.new_line++, -1, line.substr(1), "+"));
		} else if (line.begins_with("-")) {
			state.hunk_lines.push_back(state.iface->create_diff_line(-1, state.old_line++, line.substr(1), "-"));
		} else {
			// Context line (a leading space, or an empty line inside a hunk).
			String content = line.begins_with(" ") ? line.substr(1) : line;
			state.hunk_lines.push_back(state.iface->create_diff_line(state.new_line++, state.old_line++, content, " "));
		}
	}
	state.flush_file();
}

} // namespace

TypedArray<Dictionary> parse_unified_diff(EditorVCSInterface *iface, const String &diff_text) {
	DiffParseState state;
	state.iface = iface;
	parse_into(state, diff_text);
	return state.files;
}

TypedArray<Dictionary> parse_unified_diff_hunks(EditorVCSInterface *iface, const String &diff_text) {
	DiffParseState state;
	state.iface = iface;
	parse_into(state, diff_text);
	return state.all_hunks;
}

} // namespace diversion
