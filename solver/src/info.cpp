#include "skinsim/info.hpp"

// Including Eigen here (rather than only in tests) proves the solver library
// itself compiles against Eigen with EIGEN_MPL2_ONLY defined.
#include <Eigen/Core>

namespace skinsim {

std::string build_info() {
    return "skinsim solver " + std::to_string(kVersionMajor) + "." +
           std::to_string(kVersionMinor) + "." + std::to_string(kVersionPatch) +
           // Use EIGEN_VERSION_STRING: Eigen 5 keeps the legacy
           // EIGEN_WORLD/MAJOR/MINOR_VERSION macros frozen at 3.5.x so old
           // `#if` checks keep working, so they don't show the real version.
           " (Eigen " + EIGEN_VERSION_STRING + ")";
}

}  // namespace skinsim
