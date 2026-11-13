#include "skinsim/flaps.hpp"

#include <algorithm>
#include <cmath>

namespace skinsim {

namespace {

using V = Eigen::Vector2d;
constexpr double kPi = 3.14159265358979323846;

V perp(const V& v) { return V(-v.y(), v.x()); }

// Point at distance d from corner X along the bisector of the angle between
// the directions to P and Q (towards the inside of that angle).
V into_corner(const V& X, const V& P, const V& Q, double d) {
    const V bis = (P - X).normalized() + (Q - X).normalized();
    return bis.norm() > 1e-12 ? V(X + d * bis.normalized()) : X;
}

// The bisector point at a corner a bite away from both edges: for a corner
// of angle a, that is bite / sin(a/2) from the tip.
V bite_into_corner(const V& X, const V& P, const V& Q, double bite) {
    const double c = std::clamp((P - X).normalized().dot((Q - X).normalized()), -1.0, 1.0);
    const double half = 0.5 * std::acos(c);
    return into_corner(X, P, Q, bite / std::max(std::sin(half), 0.1));
}

// The point a distance d straight out from a convex polygon's corner X
// (away from its centre c).
V out_of_corner(const V& X, const V& c, double d) { return X + d * (X - c).normalized(); }

Polyline closed(std::initializer_list<V> pts) {
    Polyline p(pts);
    p.push_back(p.front());
    return p;
}

void advancement(FlapDesign& d, const V& u, const V& n, double R, const FlapShape& shape, double bite) {
    const V& c = d.centre;
    const double L = std::clamp(shape.length_to_width, 0.5, 10.0) * 2.0 * R;
    // Square defect of side 2R: its edge facing the flap is the flap's
    // leading edge.
    const V lead_l = c + R * u + R * n, lead_r = c + R * u - R * n;
    const V far_l = c - R * u + R * n, far_r = c - R * u - R * n;
    d.defect = closed({lead_l, far_l, far_r, lead_r});
    const V base_l = lead_l + L * u, base_r = lead_r + L * u;
    d.incisions = {{lead_l, base_l}, {lead_r, base_r}};
    d.flap = closed({lead_l, base_l, base_r, lead_r});
    d.base = {base_l, base_r};
    d.pivot = 0.5 * (base_l + base_r);
    d.turn_deg = 0.0;
    d.length_to_width = L / (2.0 * R);
    // Two key sutures, a quarter of the width either side of the centre
    // line: they draw the leading edge across to the far edge.
    for (double s : {0.5 * R, -0.5 * R})
        d.key_sutures.push_back({c + (R + bite) * u + s * n, c - (R + bite) * u + s * n});
}

void rotation(FlapDesign& d, const V& u, const V& w, double R, const FlapShape& shape, double bite) {
    const V& c = d.centre;
    // Triangle with its base across the flap direction, apex along w,
    // height H = 2 x the defect's diameter, wide enough that the circle of
    // radius R around c fits inside: the base is tangent to the circle, and
    // a base half-width b = R sqrt(H / (H - 2R)) makes both sides tangent too.
    const double H = 4.0 * R;
    const double b = R * std::sqrt(H / (H - 2.0 * R));
    const V A = c + (H - R) * w;   // apex (the centre of the arc)
    const V B = c - R * w + b * u; // base corner on the flap's side
    const V C = c - R * w - b * u;
    d.defect = closed({A, B, C});

    // Angles measured in the (u, w) frame, around A.
    auto angle = [&](const V& p) { return std::atan2((p - A).dot(w), (p - A).dot(u)); };
    auto at = [&](double radius, double a) { return V(A + radius * (std::cos(a) * u + std::sin(a) * w)); };
    const double r = (B - A).norm();
    const double aB = angle(B), aC = angle(C);
    // Increasing angle from B moves away from C, around the far side.
    const double sweep = std::clamp(shape.arc_angle_deg, 30.0, 270.0) * kPi / 180.0;
    const int steps = std::max(8, static_cast<int>(std::ceil(r * sweep / 1.0)));  // ~1 mm apart
    Polyline arc;
    for (int k = 0; k <= steps; ++k) arc.push_back(at(r, aB + sweep * k / steps));
    const V P = arc.back();
    d.incisions = {arc};
    d.flap = {A};
    d.flap.insert(d.flap.end(), arc.begin(), arc.end());
    d.flap.push_back(A);
    d.base = {P, A};
    d.pivot = A;
    d.turn_deg = (aB - aC) * 180.0 / kPi;  // the triangle's apex angle
    d.length_to_width = r * sweep / (2.0 * b);  // arc length over the defect's width (textbooks: ~4)
    // Key sutures: the flap's leading corner to the triangle's far corner,
    // and halfway along those edges. Each needle hole a bite inside the
    // flap (turned back from edge AB) or outside the defect (beyond AC).
    for (double rad : {r - bite, 0.5 * r}) {
        const double da = bite / rad;
        d.key_sutures.push_back({at(rad, aB + da), at(rad, aC - da)});
    }
}

void rhomboid(FlapDesign& d, const V& u, const V& a, double R, double bite) {
    const V& c = d.centre;
    // Rhombus with 60 / 120 degree angles whose inscribed circle has radius
    // R: side s = 4R / sqrt(3). Long diagonal along a, short along u.
    const double s = 4.0 * R / std::sqrt(3.0);
    const double h = 0.5 * std::sqrt(3.0) * s;
    const V A = c + h * a;        // 60 degree corner
    const V B = c + 0.5 * s * u;  // 120 degree corner, on the flap's side
    const V C = c - h * a;        // 60 degree corner
    const V D = c - 0.5 * s * u;  // 120 degree corner
    d.defect = closed({A, B, C, D});
    // First limb continues the short diagonal from B, one side long; the
    // second runs back parallel to side BA. The flap A-B-E-F is a copy of
    // the defect; turned 60 degrees about A it fills it (B -> D, E -> C).
    const V E = B + s * u;
    const V F = E + (A - B);
    d.incisions = {{B, E, F}};
    d.flap = closed({A, B, E, F});
    d.base = {F, A};
    d.pivot = A;
    d.turn_deg = 60.0;
    d.length_to_width = 1.0;
    // Key sutures: flap corner E to defect corner C, flap corner B to D.
    d.key_sutures.push_back({bite_into_corner(E, B, F, bite), out_of_corner(C, c, bite)});
    d.key_sutures.push_back({bite_into_corner(B, A, E, bite), out_of_corner(D, c, bite)});
    // The donor site: the 60 degree V the flap leaves between its two limbs
    // closes in a straight line, the lip along B-E onto the lip along E-F.
    // One suture across the middle of the V, a bite outside the flap on each limb.
    const V g = 0.25 * (A + B + E + F);  // the flap's centre
    auto outside = [&](const V& P, const V& Q) {
        const V mid = 0.5 * (P + Q);
        V nrm = perp(Q - P).normalized();
        if (nrm.dot(g - mid) > 0.0) nrm = -nrm;
        return V(mid + bite * nrm);
    };
    d.key_sutures.push_back({outside(B, E), outside(E, F)});
}

}  // namespace

FlapDesign design_flap(FlapType type, const Eigen::Vector2d& centre, const Eigen::Vector2d& direction,
                       double defect_diameter, const FlapShape& shape, double bite) {
    FlapDesign d;
    d.type = type;
    d.centre = centre;
    d.defect_diameter = defect_diameter;
    if (!(defect_diameter > 0.0) || !std::isfinite(defect_diameter) || !centre.allFinite() ||
        !direction.allFinite() || direction.norm() < 1e-9)
        return d;
    const double R = 0.5 * defect_diameter;
    bite = std::clamp(std::isfinite(bite) ? bite : 2.0, 0.2, R);
    const V u = direction.normalized();
    const V side = shape.mirror ? V(-perp(u)) : perp(u);
    switch (type) {
        case FlapType::Advancement: advancement(d, u, side, R, shape, bite); break;
        case FlapType::Rotation: rotation(d, u, side, R, shape, bite); break;
        case FlapType::Rhomboid: rhomboid(d, u, side, R, bite); break;
    }
    d.valid = true;
    return d;
}

bool inside_polygon(const Polyline& polygon, const Eigen::Vector2d& p) {
    bool in = false;
    const size_t n = polygon.size();
    for (size_t i = 0, j = n - 1; i < n; j = i++) {
        const V& a = polygon[i];
        const V& b = polygon[j];
        if ((a.y() > p.y()) != (b.y() > p.y()) && p.x() < (b.x() - a.x()) * (p.y() - a.y()) / (b.y() - a.y()) + a.x())
            in = !in;
    }
    return in;
}

}  // namespace skinsim
