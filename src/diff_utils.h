#pragma once

#include <godot_cpp/classes/editor_vcs_interface.hpp>
#include <godot_cpp/variant/typed_array.hpp>

namespace diversion {

// Parses git-style unified diff text (as emitted by `dv diff`) into the
// diff-file dictionaries Godot's VCS dock expects. The interface pointer is
// needed because the dictionary layouts come from EditorVCSInterface's
// create_diff_* helper methods.
godot::TypedArray<godot::Dictionary> parse_unified_diff(godot::EditorVCSInterface *iface, const godot::String &diff_text);

// Same parse, but returns the flat list of hunk dictionaries (the layout
// _get_line_diff expects for the script editor gutter).
godot::TypedArray<godot::Dictionary> parse_unified_diff_hunks(godot::EditorVCSInterface *iface, const godot::String &diff_text);

} // namespace diversion
