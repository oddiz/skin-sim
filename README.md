# skin-sim

Local flap simulator for plastic surgery: a skin membrane you can cut, drag and suture.
See `CLAUDE.md` for goals, architecture and milestones.

## Layout

| Path | What |
|---|---|
| `solver/` | Standalone C++ mechanics library (no Godot). Tests in `solver/tests/`. |
| `extension/` | Thin GDExtension layer exposing solver classes to Godot. |
| `game/` | Godot 4.7 project. The built DLL lands in `game/bin/`. |
| `thirdparty/` | Pinned git submodules: godot-cpp, Eigen, CDT, doctest. |

## Prerequisites (Windows)

- Git, Python 3, SCons (`python -m pip install scons`)
- Visual Studio 2022 Build Tools with "Desktop development with C++" (MSVC)
- Godot 4.7 standard build

## Build

```
git clone --recurse-submodules <repo-url>     # or: git submodule update --init
scons                # build godot-cpp, solver, tests, extension (debug)
scons run_tests      # build + run solver unit tests
scons target=template_release   # optimised build
```

The first build compiles godot-cpp and takes a few minutes; later builds are incremental.

## Run

Open `game/project.godot` in Godot 4.7, or headless:

```
Godot_v4.7.2-stable_win64_console.exe --headless --path game --quit-after 1
```

Note: scripts declare global types with `class_name`. Godot registers these when the editor
scans the project. If you run the game from the command line on a fresh checkout (no
`game/.godot/` folder yet), run an import first:

```
Godot_v4.7.2-stable_win64_console.exe --headless --path game --import
```

## Controls

| Input | Action |
|---|---|
| Drag (Grab tool) | Grab and pull the skin with forceps; several fingers on a touch screen |
| Drag (Cut tool), or Shift + drag | Draw an incision; it is made when you release |
| C | Switch between Grab and Cut |
| Z | Undo the last incision |
| W / R | Wireframe on/off / reset the deformation (incisions stay) |

Incisions are stored in rest (tissue) coordinates, so you can cut skin that is
being pulled. They stay one element away from the pinned outer border, and a
stroke starting within ~2 mm of an existing cut snaps onto it, so flap limbs join cleanly.

## Tests

`scons run_tests` runs the unit tests. An opt-in stress test throws ~1600
adversarial strokes at the cutter and mesher (it takes ~40 s):

```
build\skinsim_tests.windows.template_debug.x86_64.exe --no-skip -tc="*adversarial*"
```

## Presets

Built-in tissue presets live in `game/presets/*.json` (format documented in
`game/presets/README.md`). Presets saved from the app go to `user://presets/`.
