#!/usr/bin/env python
"""
Top-level build script for skin-sim.

SCons build scripts are plain Python. SCons reads this file, builds a dependency
graph of "targets" (files to produce) and their "sources", then rebuilds only
what is out of date, like `make`, but in Python.

What gets built:
  1. godot-cpp      -> static library with the C++ bindings to Godot's API
  2. solver         -> static library (thirdparty-free of Godot; Eigen + CDT only)
  3. solver tests   -> standalone .exe using doctest (runs headless)
  4. extension      -> the GDExtension DLL, written into game/bin/ for Godot to load

Common commands (run from the repo root):
  scons                 build everything (debug template, the default)
  scons run_tests       build and run the solver unit tests
  scons -j8             build using 8 parallel compiler processes
  scons -c              clean (delete build outputs)
"""

import os

# --- 1. godot-cpp ------------------------------------------------------------
# Calling godot-cpp's own SConstruct does two things: it builds libgodot-cpp,
# and it *returns* an Environment already configured for the platform/compiler
# (MSVC flags, C++17, include paths, link to libgodot-cpp). We pass
# api_version="4.7": godot-cpp 10.x supports several Godot versions and needs to
# know which extension_api JSON (the description of Godot's classes) to bind to.
env = SConscript("thirdparty/godot-cpp/SConstruct", {"api_version": "4.7"})

EIGEN_DIR = "#thirdparty/eigen"  # '#' means "relative to the top-level SConstruct"
CDT_DIR = "#thirdparty/CDT/CDT/include"
DOCTEST_DIR = "#thirdparty/doctest"

# --- 2. solver (no Godot allowed) --------------------------------------------
# Clone keeps the compiler flags (so the static runtime, C++17, and the
# no-exceptions setting all match the extension and link cleanly) but we
# *replace* the include path and libraries. Any accidental
# `#include <godot_cpp/...>` inside /solver then fails to compile, which enforces
# the "zero Godot dependency" rule mechanically.
solver_env = env.Clone()
solver_env.Replace(CPPPATH=["#solver/include", EIGEN_DIR, CDT_DIR], LIBS=[], LIBPATH=[])
# EIGEN_MPL2_ONLY: Eigen refuses to compile any of its (few) LGPL-licensed parts.
solver_env.Append(CPPDEFINES=["EIGEN_MPL2_ONLY"])

solver_lib = solver_env.StaticLibrary(
    "#build/skinsim_solver" + env["suffix"] + env["LIBSUFFIX"],
    source=Glob("solver/src/*.cpp"),
)

# --- 3. solver tests (headless) ----------------------------------------------
tests_env = solver_env.Clone()
tests_env.Append(CPPPATH=[DOCTEST_DIR])
# We compile without C++ exceptions (to match Godot). doctest normally uses
# exceptions to abort a failing REQUIRE; this define keeps all assertion
# macros available in a no-exceptions build.
tests_env.Append(CPPDEFINES=["DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS"])
tests_env.Append(LIBS=[solver_lib])

tests_exe = tests_env.Program(
    "#build/skinsim_tests" + env["suffix"] + env["PROGSUFFIX"],
    source=Glob("solver/tests/*.cpp"),
)
# `scons run_tests` builds the test exe, then executes it. A non-zero exit code
# (a failed test) makes SCons report the build as failed.
# We run it from a Python function with subprocess rather than a shell-command
# action, because godot-cpp installs a custom process spawner on Windows that
# hides the program's console output.
def _run_tests(target, source, env):
    import subprocess

    return subprocess.call([source[0].abspath])


run_tests = tests_env.Command("run_tests_stamp", tests_exe, _run_tests)
tests_env.AlwaysBuild(run_tests)
Alias("run_tests", run_tests)
Alias("tests", tests_exe)

# --- 4. GDExtension ----------------------------------------------------------
ext_env = env.Clone()
ext_env.Append(CPPPATH=["#extension/src", "#solver/include"])
ext_env.Append(CPPDEFINES=["EIGEN_MPL2_ONLY"])
# Prepend so the solver comes before libgodot-cpp on the link line.
ext_env.Prepend(LIBS=[solver_lib])

ext_sources = Glob("extension/src/*.cpp")

# Output name, e.g. game/bin/libskinsim.windows.template_debug.x86_64.dll.
# env["suffix"] encodes platform/target/arch so debug and release builds for
# several platforms can sit side by side; skinsim.gdextension maps each to a file.
if env["platform"] == "macos":
    library = ext_env.SharedLibrary(
        "#game/bin/libskinsim{0}.framework/libskinsim{0}".format(env["suffix"]),
        source=ext_sources,
    )
elif env["platform"] == "ios":
    # iOS does not allow loading arbitrary shared libraries: static lib instead.
    library = ext_env.StaticLibrary(
        "#game/bin/libskinsim{}.a".format(env["suffix"]), source=ext_sources
    )
else:
    library = ext_env.SharedLibrary(
        "#game/bin/libskinsim{}{}".format(env["suffix"], env["SHLIBSUFFIX"]),
        source=ext_sources,
    )

Default(library, tests_exe)
