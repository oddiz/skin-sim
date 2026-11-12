#include "skinsim/contact.hpp"

#include <algorithm>
#include <cmath>

namespace skinsim::contact {

namespace {

// 2D cross product a x b (the z component of the 3D one): positive when b
// points to the left of a.
double cross(const Eigen::Vector2d& a, const Eigen::Vector2d& b) { return a.x() * b.y() - a.y() * b.x(); }

}  // namespace

// Derivation (worth following once, since every contact force comes from it).
//
// With e = v - u and w = p - u, the gap is g = -c / L, where
//   c = e x w = e^T J w,   J = [[0, 1], [-1, 0]],   L = |e|.
// e and w are linear in the unknowns q = (p, u, v):
//   e = E q,  E = [0, -I, I]        w = W q,  W = [I, -I, 0]   (2 x 6 each).
// So, with s = 1/L:
//   grad c = E^T J w + W^T J^T e          hess c = E^T J W + W^T J^T E
//   grad s = -E^T e / L^3                 hess s = E^T (3 e e^T / L^5 - I / L^3) E
// and g = -c s gives, by the product rule,
//   grad g = -(s grad c + c grad s)
//   hess g = -(s hess c + grad c grad s^T + grad s grad c^T + c hess s).
double signed_gap(const Eigen::Vector2d& p, const Eigen::Vector2d& u, const Eigen::Vector2d& v, Vec6* grad,
                  Mat6* hess) {
    const Eigen::Vector2d e = v - u, w = p - u;
    const double L = e.norm();
    const double c = cross(e, w);
    const double s = 1.0 / L;
    const double g = -c * s;
    if (!grad && !hess) return g;

    Eigen::Matrix<double, 2, 6> E = Eigen::Matrix<double, 2, 6>::Zero(), W = Eigen::Matrix<double, 2, 6>::Zero();
    E.block<2, 2>(0, 2) = -Eigen::Matrix2d::Identity();
    E.block<2, 2>(0, 4) = Eigen::Matrix2d::Identity();
    W.block<2, 2>(0, 0) = Eigen::Matrix2d::Identity();
    W.block<2, 2>(0, 2) = -Eigen::Matrix2d::Identity();
    Eigen::Matrix2d J;
    J << 0, 1, -1, 0;

    const Vec6 grad_c = E.transpose() * J * w + W.transpose() * J.transpose() * e;
    const Vec6 grad_s = -E.transpose() * e * (s * s * s);
    if (grad) *grad = -(s * grad_c + c * grad_s);
    if (hess) {
        const Mat6 hess_c = E.transpose() * J * W + W.transpose() * J.transpose() * E;
        const Eigen::Matrix2d inner = 3.0 * e * e.transpose() * std::pow(s, 5) - Eigen::Matrix2d::Identity() * (s * s * s);
        const Mat6 hess_s = E.transpose() * inner * E;
        *hess = -(s * hess_c + grad_c * grad_s.transpose() + grad_s * grad_c.transpose() + c * hess_s);
    }
    return g;
}

double projection(const Eigen::Vector2d& p, const Eigen::Vector2d& u, const Eigen::Vector2d& v) {
    const Eigen::Vector2d e = v - u;
    const double len2 = e.squaredNorm();
    return len2 > 0.0 ? (p - u).dot(e) / len2 : 0.0;
}

int crossings(const Eigen::Vector2d& p0, const Eigen::Vector2d& p1, const Eigen::Vector2d& u0,
              const Eigen::Vector2d& u1, const Eigen::Vector2d& v0, const Eigen::Vector2d& v1, Crossing out[2]) {
    // c(t) = e(t) x w(t), with e(t) = e0 + t de and w(t) = w0 + t dw:
    //   c(t) = a t^2 + b t + c0.
    // g = -c / L, so moving from the wound (g > 0) into the tissue (g < 0)
    // means c rising through zero (c'(t) = 2 a t + b > 0); falling = outward.
    const Eigen::Vector2d e0 = v0 - u0, w0 = p0 - u0;
    const Eigen::Vector2d de = (v1 - u1) - e0, dw = (p1 - u1) - w0;
    const double a = cross(de, dw);
    const double b = cross(e0, dw) + cross(de, w0);
    const double c0 = cross(e0, w0);

    const double scale = std::max({std::abs(a), std::abs(b), std::abs(c0)});
    if (!(scale > 0.0)) return 0;  // nothing moves relative to the line (or NaN)

    double roots[2];
    int count = 0;
    if (std::abs(a) <= 1e-12 * scale) {
        // Effectively linear (e.g. the segment only translates).
        if (b != 0.0) roots[count++] = -c0 / b;
    } else {
        const double disc = b * b - 4.0 * a * c0;
        if (disc < 0.0) return 0;  // never on the line
        // The numerically stable form of the quadratic formula: avoids
        // subtracting two nearly equal numbers.
        const double q = -0.5 * (b + std::copysign(std::sqrt(disc), b));
        roots[count++] = q / a;
        if (q != 0.0) roots[count++] = c0 / q;
        if (count == 2 && roots[1] < roots[0]) std::swap(roots[0], roots[1]);
    }

    constexpr double kSlack = 1e-12;
    int found = 0;
    for (int k = 0; k < count; ++k) {
        const double t = roots[k];
        if (!(t >= -kSlack && t <= 1.0 + kSlack)) continue;
        const double slope = 2.0 * a * t + b;
        if (slope == 0.0) continue;  // grazing the line, not crossing it
        const Eigen::Vector2d p = p0 + t * (p1 - p0), u = u0 + t * (u1 - u0), v = v0 + t * (v1 - v0);
        const double s = projection(p, u, v);
        if (s < 0.0 || s > 1.0) continue;  // passed the line beyond the segment's ends
        out[found++] = {std::clamp(t, 0.0, 1.0), slope > 0.0};
    }
    return found;
}

bool crosses_inward(const Eigen::Vector2d& p0, const Eigen::Vector2d& p1, const Eigen::Vector2d& u0,
                    const Eigen::Vector2d& u1, const Eigen::Vector2d& v0, const Eigen::Vector2d& v1, double* time) {
    Crossing c[2];
    const int n = crossings(p0, p1, u0, u1, v0, v1, c);
    for (int k = 0; k < n; ++k)
        if (c[k].inward) {
            if (time) *time = c[k].time;
            return true;
        }
    return false;
}

}  // namespace skinsim::contact
