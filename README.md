# godot-diversion-plugin

A [Diversion](https://www.diversion.dev) version control plugin for the Godot editor,
implemented as a GDExtension that plugs into Godot's built-in Version Control UI
(`EditorVCSInterface`), the same way the official Git plugin does.

**Status: early development (phase 1 — scaffold).**

## How it works

- Actions (commit, branch, checkout, update) shell out to the `dv` CLI so the local
  Diversion sync agent stays authoritative.
- Diversion has auto-syncing workspaces and no staging area, push, or remotes.
  The plugin emulates staging (selecting files for `dv commit`), maps *Pull* to
  `dv update`, and makes *Push* a no-op since the sync agent uploads continuously.

## Requirements

- Godot 4.5+ (editor)
- [Diversion CLI](https://docs.diversion.dev) installed and logged in (`dv login`),
  with the project folder inside a Diversion workspace (`dv init` / `dv clone`)

## Building from source

```
git clone --recurse-submodules <this repo>
cd godot-diversion-plugin
scons platform=windows target=editor   # or platform=linux / platform=macos
```

The library is emitted into `demo/addons/diversion/bin/`. Copy `demo/addons/diversion`
into your project's `addons/` folder, then enable it via
**Project > Version Control > Set Up Version Control** and pick **Diversion**.

## License

MIT — see [LICENSE](LICENSE).
