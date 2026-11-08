// Basic identification of the solver library.
//
// This header is part of the solver's *public interface*: everything under
// solver/include/skinsim/ may be used by the Godot extension, the tests, and
// (later) a Python wrapper. It must never include Godot headers.
#pragma once

#include <string>

namespace skinsim {

// Semantic version of the solver library.
inline constexpr int kVersionMajor = 0;
inline constexpr int kVersionMinor = 0;
inline constexpr int kVersionPatch = 1;

// Human-readable string such as "skinsim solver 0.0.1 (Eigen 5.0.1)".
// Declared here, defined in solver/src/info.cpp: in C++ the header says *what*
// exists (the declaration) and a .cpp file says *how* (the definition). Other
// code only needs the header to call it; the linker connects the two later.
std::string build_info();

}  // namespace skinsim
