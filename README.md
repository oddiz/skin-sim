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
