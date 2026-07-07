# godot-diversion-plugin

A [Diversion](https://www.diversion.dev) version control plugin for the Godot editor,
implemented as a GDExtension that plugs into Godot's built-in Version Control UI
(`EditorVCSInterface`), the same way the official Git plugin does.

## Features

- Modified/new/renamed/deleted files in the Version Control dock, kept fresh by a
  background poller (the editor UI never blocks on Diversion calls)
- Commit selected files from the dock (staging is emulated: Diversion has no
  staging area, your selection becomes the `dv commit` file list)
- Branch list, create, delete, and checkout (checkout carries uncommitted changes)
- Commit history with per-commit diffs, file diffs, and script-editor change gutter
- Discard file changes (`dv reset --clean`), Pull = `dv update`
- Friendly errors when dv is missing, logged out, or the project isn't a workspace

## How Diversion concepts map to Godot's VCS UI

| Godot VCS UI | Diversion |
|---|---|
| Stage / unstage | In-plugin selection of what `dv commit` includes |
| Commit | `dv commit <files> -m <msg>` (publishes to your branch) |
| Push | No-op — the sync agent uploads your work continuously |
| Pull | `dv update` (brings branch head into your workspace) |
| Fetch | Refreshes cached status |
| Remotes | Managed by Diversion; shown as a single `diversion-cloud` entry |
| Conflicts | Files show as unmerged; resolve in the Diversion app (`dv view`) |

## Requirements

- Godot 4.5+ (editor)
- [Diversion CLI](https://docs.diversion.dev) installed and logged in (`dv login`),
  with the project folder inside a Diversion workspace (`dv init` / `dv clone`)

## Installation

1. Copy `addons/diversion/` into your project (from a release zip or the Asset
   Library).
2. Restart the editor once so the extension registers.
3. **Project > Version Control > Set Up Version Control**, pick **Diversion**.

## Building from source

```
git clone --recurse-submodules <this repo>
cd godot-diversion-plugin
scons platform=windows target=editor   # or platform=linux / platform=macos
```

The library is emitted into `demo/addons/diversion/bin/`. CI builds all three
platforms on every push (see `.github/workflows/build.yml`).

## Known limitations

- The script-editor change gutter updates on save (unsaved buffer edits are not
  diffed yet).
- Commit amend is not supported (Diversion has no amend).
- Merge-conflict resolution happens in the Diversion desktop/web app, not inside
  Godot.

## License

MIT — see [LICENSE](LICENSE).
