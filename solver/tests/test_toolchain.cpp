// M0 toolchain tests: not physics yet. They prove each vendored library
// compiles, links and behaves under our exact build flags (MSVC, C++17,
// no exceptions, EIGEN_MPL2_ONLY) before we build anything on top of them.

#include <doctest/doctest.h>

#include <Eigen/Sparse>
#include <Eigen/SparseCholesky>

#include <CDT.h>

#include "skinsim/info.hpp"

TEST_CASE("solver library links and reports its version") {
    const std::string info = skinsim::build_info();
    CHECK(info.find("skinsim solver 0.0.1") == 0);
    CHECK(info.find("Eigen") != std::string::npos);
}

TEST_CASE("Eigen sparse Cholesky solves a small SPD system") {
    // The Newton solver (M2) will repeatedly solve K * dx = -r, where K is a
    // sparse, symmetric stiffness matrix. SimplicialLDLT is the built-in Eigen
    // solver for exactly that, so check it works here.
    //
    // K is the 1D "spring chain" matrix [2 -1 0; -1 2 -1; 0 -1 2].
    Eigen::SparseMatrix<double> K(3, 3);
    K.insert(0, 0) = 2;  K.insert(0, 1) = -1;
    K.insert(1, 0) = -1; K.insert(1, 1) = 2;  K.insert(1, 2) = -1;
    K.insert(2, 1) = -1; K.insert(2, 2) = 2;
    K.makeCompressed();

    const Eigen::Vector3d x_true(1.0, 2.0, 3.0);
    const Eigen::Vector3d b = K * x_true;

    Eigen::SimplicialLDLT<Eigen::SparseMatrix<double>> ldlt(K);
    REQUIRE(ldlt.info() == Eigen::Success);
    const Eigen::Vector3d x = ldlt.solve(b);
    CHECK((x - x_true).norm() < 1e-12);
}

TEST_CASE("CDT triangulates a unit square into two triangles") {
    // Used from M4 on for re-meshing after incisions.
    CDT::Triangulation<double> cdt;
    std::vector<CDT::V2d<double>> pts = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
    cdt.insertVertices(pts);
    cdt.eraseSuperTriangle();  // drop the helper triangle CDT starts from
    CHECK(cdt.vertices.size() == 4);
    CHECK(cdt.triangles.size() == 2);
}
