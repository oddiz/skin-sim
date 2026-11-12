// Geometry for contact between incision lips (no state, no mesh: pure maths,
// so it can be tested on its own).
//
// The lips of an incision are boundary edges of the mesh. Each lip edge
// ("segment") is stored oriented u -> v with the tissue on its LEFT (that is
// the counter-clockwise order of the one triangle it belongs to), so its
// outward normal n, pointing out of the tissue into the wound, is on the right:
//
//     n = (e_y, -e_x) / |e|,   e = v - u.
//
// The signed gap of a point p from that segment's line is
//
//     g = n . (p - u)   > 0: p is in the wound (free)
//                       < 0: p has entered the tissue behind the segment.
//
// Simulation uses it (as |g|) for the depth of a lip node that has pressed
// into the skin behind a lip edge, and turns that into a penalty energy
// 1/2 k g^2: a stiff spring that only acts when the lips overlap.
#pragma once

#include <Eigen/Core>

namespace skinsim::contact {

using Vec6 = Eigen::Matrix<double, 6, 1>;
using Mat6 = Eigen::Matrix<double, 6, 6>;

// Signed gap of p from the line through the segment (u, v). The 6 unknowns
// are ordered (p_x, p_y, u_x, u_y, v_x, v_y). If grad / hess are given, they
// receive dg/dq and d2g/dq2. The segment must have non-zero length.
double signed_gap(const Eigen::Vector2d& p, const Eigen::Vector2d& u, const Eigen::Vector2d& v,
                  Vec6* grad = nullptr, Mat6* hess = nullptr);

// Where p projects onto the segment: 0 at u, 1 at v (outside [0, 1] = beyond its ends).
double projection(const Eigen::Vector2d& p, const Eigen::Vector2d& u, const Eigen::Vector2d& v);

// Continuous collision detection (CCD). Over one solver step, every node
// moves in a straight line: p goes from p0 to p1, u from u0 to u1, v from v0
// to v1. Does p cross the segment from its outside (the wound) into the tissue
// at some moment of the step? If so, `time` receives the first such moment
// (0..1). Contact only looks at the end of a step, so without this a node
// moving fast enough could pass right through a narrow piece of skin within
// one step and never be noticed ("tunnelling").
//
// Why it is a quadratic: p lies on the segment's line exactly when the cross
// product (v - u) x (p - u) is zero, and with everything moving linearly that
// cross product is a quadratic polynomial in time.
bool crosses_inward(const Eigen::Vector2d& p0, const Eigen::Vector2d& p1, const Eigen::Vector2d& u0,
                    const Eigen::Vector2d& u1, const Eigen::Vector2d& v0, const Eigen::Vector2d& v1,
                    double* time = nullptr);

// Every moment of the step at which p passes through the segment (at most
// two: the cross product is quadratic), in time order, with its direction.
struct Crossing {
    double time;  // 0..1 through the step
    bool inward;  // from the wound into the tissue (else out of it)
};
int crossings(const Eigen::Vector2d& p0, const Eigen::Vector2d& p1, const Eigen::Vector2d& u0,
              const Eigen::Vector2d& u1, const Eigen::Vector2d& v0, const Eigen::Vector2d& v1, Crossing out[2]);

}  // namespace skinsim::contact
