#include "skinsim/flaps.hpp"

#include <algorithm>
#include <cmath>
#include <functional>

namespace skinsim {

namespace {

using V = Eigen::Vector2d;
constexpr double kPi = 3.14159265358979323846;
constexpr double kDeg = kPi / 180.0;

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

// A bite off the middle of segment PQ, on the side away from point g.
V beside(const V& P, const V& Q, const V& g, double bite) {
    const V mid = 0.5 * (P + Q);
    V nrm = perp(Q - P).normalized();
    if (nrm.dot(g - mid) > 0.0) nrm = -nrm;
    return mid + bite * nrm;
}

Polyline closed(std::initializer_list<V> pts) {
    Polyline p(pts);
    p.push_back(p.front());
    return p;
}

void close(Polyline& p) {
    if (!p.empty()) p.push_back(p.front());
}

V centroid(const Polyline& closed_poly) {
    V g = V::Zero();
    const size_t n = closed_poly.size() > 1 ? closed_poly.size() - 1 : closed_poly.size();
    for (size_t i = 0; i < n; ++i) g += closed_poly[i];
    return n > 0 ? V(g / static_cast<double>(n)) : g;
}

// The design's frame: u from the defect towards the flap, n to its side
// (flipped by "mirror"). at(x, y) = c + x u + y n.
struct Frame {
    V c, u, n;
    V at(double x, double y) const { return c + x * u + y * n; }
    V dir(double angle) const { return std::cos(angle) * u + std::sin(angle) * n; }
    double angle_of(const V& p, const V& about) const { return std::atan2((p - about).dot(n), (p - about).dot(u)); }
};

// Points on a circle (centre o, radius r, angles in the frame) from a0 to
// a1 (either direction), about `step` mm apart, both ends included.
Polyline arc(const Frame& f, const V& o, double r, double a0, double a1, double step = 1.0) {
    const int n = std::max(2, static_cast<int>(std::ceil(std::abs(a1 - a0) * r / step)));
    Polyline p;
    for (int k = 0; k <= n; ++k) p.push_back(o + r * f.dir(a0 + (a1 - a0) * k / n));
    return p;
}

// Append b to a, skipping b's first point if it repeats a's last.
void join(Polyline& a, const Polyline& b) {
    for (size_t i = 0; i < b.size(); ++i)
        if (!(i == 0 && !a.empty() && (a.back() - b[0]).norm() < 1e-9)) a.push_back(b[i]);
}

// A polygon with the round defect inside. Its corners are 10 degrees apart
// (36 sides), or up to ~13 where an incision's start replaces one: radius
// R / cos(7 degrees) keeps the circle of radius R inside every edge.
constexpr int kCircleSides = 36;
double polygon_radius(double R) { return R / std::cos(7.0 * kDeg); }

// Burow's triangle on the outer side of an incision near its end: one side
// along the incision (length t, ending at `end`, running in direction
// `along` away from the defect), apex `t` out to `outward`. The incision
// stops where the triangle starts. Returns the triangle; shortens `limb`.
Polyline burow_triangle(Polyline& limb, const V& along, const V& outward, double t) {
    const V end = limb.back();
    const V start = end - t * along;
    limb.back() = start;
    return closed({start, end, V(start + 0.5 * t * along + t * outward)});
}

// --- advancement family -----------------------------------------------------

void advancement(FlapDesign& d, const Frame& f, double R, const FlapShape& shape, double bite) {
    const double L = std::clamp(shape.length_to_width, 0.5, 10.0) * 2.0 * R;
    // Square defect of side 2R: its edge facing the flap is the flap's
    // leading edge.
    const V lead_l = f.at(R, R), lead_r = f.at(R, -R);
    const V far_l = f.at(-R, R), far_r = f.at(-R, -R);
    d.defect = closed({lead_l, far_l, far_r, lead_r});
    const V base_l = lead_l + L * f.u, base_r = lead_r + L * f.u;
    Polyline limb_l{lead_l, base_l}, limb_r{lead_r, base_r};
    if (shape.burow) {
        // The outer lip of each limb is longer than the flap's by the
        // distance advanced (2R): a triangle of that base takes it up.
        const double t = std::min(2.0 * R, 0.8 * L);
        d.burow.push_back(burow_triangle(limb_l, f.u, f.n, t));
        d.burow.push_back(burow_triangle(limb_r, f.u, -f.n, t));
    }
    d.incisions = {limb_l, limb_r};
    d.flaps = {closed({lead_l, base_l, base_r, lead_r})};
    d.bases = {{base_l, base_r}};
    d.pivot = 0.5 * (base_l + base_r);
    d.length_to_width = L / (2.0 * R);
    // Two key sutures, a quarter of the width either side of the centre
    // line: they draw the leading edge across to the far edge.
    for (double s : {0.5 * R, -0.5 * R}) d.key_sutures.push_back({f.at(R + bite, s), f.at(-R - bite, s)});
}

void h_plasty(FlapDesign& d, const Frame& f, double R, const FlapShape& shape, double bite) {
    // Two advancement flaps facing each other, each moving R (half the defect).
    const double L = std::clamp(shape.length_to_width, 0.5, 10.0) * 2.0 * R;
    const V a_l = f.at(R, R), a_r = f.at(R, -R), b_l = f.at(-R, R), b_r = f.at(-R, -R);
    d.defect = closed({a_l, b_l, b_r, a_r});
    Polyline limbs[4] = {{a_l, a_l + L * f.u}, {a_r, a_r + L * f.u}, {b_l, b_l - L * f.u}, {b_r, b_r - L * f.u}};
    if (shape.burow) {
        const double t = std::min(R, 0.8 * L);
        d.burow.push_back(burow_triangle(limbs[0], f.u, f.n, t));
        d.burow.push_back(burow_triangle(limbs[1], f.u, -f.n, t));
        d.burow.push_back(burow_triangle(limbs[2], -f.u, f.n, t));
        d.burow.push_back(burow_triangle(limbs[3], -f.u, -f.n, t));
    }
    d.incisions.assign(std::begin(limbs), std::end(limbs));
    d.flaps = {closed({a_l, a_l + L * f.u, a_r + L * f.u, a_r}), closed({b_l, b_l - L * f.u, b_r - L * f.u, b_r})};
    d.bases = {{a_l + L * f.u, a_r + L * f.u}, {b_l - L * f.u, b_r - L * f.u}};
    d.pivot = f.c;
    d.length_to_width = L / (2.0 * R);
    // The two leading edges meet in the middle.
    for (double s : {0.5 * R, -0.5 * R}) d.key_sutures.push_back({f.at(R + bite, s), f.at(-R - bite, s)});
}

void a_t_plasty(FlapDesign& d, const Frame& f, double R, double bite) {
    // Equilateral triangle around the circle (inradius R): base on the u
    // side (the T's bar), apex 2R the other way.
    const double h = std::sqrt(3.0) * R;
    const V A = f.at(-2.0 * R, 0.0), Bl = f.at(R, h), Br = f.at(R, -h);
    d.defect = closed({A, Bl, Br});
    // The base is extended both ways, 1.5 x the defect's diameter each.
    const double L = 3.0 * R;
    const V El = Bl + L * f.n, Er = Br - L * f.n;
    d.incisions = {{Bl, El}, {Br, Er}};
    // The flaps: the skin beside the triangle under the extended base,
    // attached at the far end and below.
    d.flaps = {closed({A, Bl, El, V(El - 3.0 * R * f.u)}), closed({A, Br, Er, V(Er - 3.0 * R * f.u)})};
    d.bases = {{El, V(El - 3.0 * R * f.u)}, {Er, V(Er - 3.0 * R * f.u)}};
    d.pivot = A;
    d.length_to_width = L / (2.0 * h);
    // The corners meet in the middle of the base (the T's junction), and
    // the sides close into the T's stem.
    d.key_sutures.push_back({bite_into_corner(Bl, El, A, bite), bite_into_corner(Br, Er, A, bite)});
    const V g = centroid(d.defect);
    d.key_sutures.push_back({beside(Bl, A, g, bite), beside(Br, A, g, bite)});
    // The T's bar: one suture across each extended limb, two thirds out,
    // its lips sliding past each other as the flaps move in.
    for (const V& E : {El, Er}) {
        const V B = (E - Bl).norm() < (E - Br).norm() ? Bl : Br;
        const V m = B + (2.0 / 3.0) * (E - B);
        d.key_sutures.push_back({V(m - bite * f.u), V(m + bite * f.u)});
    }
}

// A circle polygon (radius r around f.c) with extra vertices at the given
// angles, so incisions can start exactly on a corner of it.
Polyline circle_with(const Frame& f, double r, std::vector<double> extra) {
    std::vector<double> a;
    for (int k = 0; k < kCircleSides; ++k) a.push_back(2.0 * kPi * k / kCircleSides);
    for (double e : extra) {
        e = std::fmod(e, 2.0 * kPi);
        if (e < 0.0) e += 2.0 * kPi;
        a.push_back(e);
    }
    std::sort(a.begin(), a.end());
    Polyline p;
    for (double x : a)  // (corners closer than ~3 degrees merge)
        if (p.empty() || (f.c + r * f.dir(x) - p.back()).norm() > 0.05 * r) p.push_back(f.c + r * f.dir(x));
    if (p.size() > 2 && (p.front() - p.back()).norm() <= 0.05 * r) p.pop_back();
    // The extra angles must be exact vertices: overwrite the nearest.
    for (double e : extra) {
        const V q = f.c + r * f.dir(e);
        size_t best = 0;
        for (size_t i = 1; i < p.size(); ++i)
            if ((p[i] - q).norm() < (p[best] - q).norm()) best = i;
        p[best] = q;
    }
    close(p);
    return p;
}

void v_y_island(FlapDesign& d, const Frame& f, double R, const FlapShape& shape, double bite) {
    const double Rp = polygon_radius(R);
    // The V's apex; its legs leave the defect tangentially (no thin wedge).
    const double apex_dist = std::max(std::clamp(shape.length_to_width, 1.0, 5.0) * 2.0 * R, 1.5 * Rp);
    const V apex = f.at(apex_dist, 0.0);
    const double t = std::acos(Rp / apex_dist);  // tangent points at +-t
    d.defect = circle_with(f, Rp, {t, -t});
    const V Tl = f.c + Rp * f.dir(t), Tr = f.c + Rp * f.dir(-t);
    d.incisions = {{Tl, apex, Tr}};
    Polyline island = arc(f, f.c, Rp, -t, t);
    island.push_back(apex);
    close(island);
    d.flaps = {island};
    d.islands = {island};
    d.bases = {};
    d.pivot = f.c;
    d.length_to_width = apex_dist / (2.0 * R);
    // The island's leading edge onto the far side of the defect: on the
    // axis, and either side of it, halfway to the legs (each point meets
    // its mirror image across the defect).
    const double x = Rp + bite;
    const double leg_y = (Tl - f.c).dot(f.n) + (x - (Tl - f.c).dot(f.u)) * (apex - Tl).dot(f.n) / (apex - Tl).dot(f.u);
    const double side = std::min(0.45 * R, 0.5 * leg_y);
    for (double y : {0.0, side, -side}) {
        const double along = std::sqrt(std::max(x * x - y * y, 0.0));
        d.key_sutures.push_back({f.at(along, y), f.at(-along, y)});
    }
    // ...and the V closes behind it into the Y's stem.
    const double back = std::min(2.0 * R, 0.6 * (apex - Tl).norm());
    const V on_l = apex + back * (Tl - apex).normalized(), on_r = apex + back * (Tr - apex).normalized();
    d.key_sutures.push_back({beside(on_l, apex, f.c, bite), beside(on_r, apex, f.c, bite)});
}

void keystone(FlapDesign& d, const Frame& f, double R, double bite) {
    // Fusiform (lens) defect, 3:1, its long axis across u: two circular
    // arcs of radius rho through the tips.
    // (1% wider than the lesion circle: the arcs are drawn as chords.)
    const double a = 3.0 * R, Rw = 1.01 * R;
    const double rho = (a * a + Rw * Rw) / (2.0 * Rw);
    const V near_c = f.at(Rw - rho, 0.0);  // centre of the arc on the flap's side
    const V far_c = f.at(rho - Rw, 0.0);
    const double half = std::atan2(a, rho - Rw);  // half the angle each arc spans
    const V Tl = f.at(0.0, a), Tr = f.at(0.0, -a);
    // The flap's side of the lens: from Tr to Tl around near_c (angles
    // -half..half), the far side around far_c.
    Polyline near_arc = arc(f, near_c, rho, -half, half);
    Polyline far_arc = arc(f, far_c, rho, kPi - half, kPi + half);  // Tl .. Tr
    near_arc.front() = Tr;
    near_arc.back() = Tl;
    far_arc.front() = Tl;
    far_arc.back() = Tr;
    d.defect = near_arc;
    join(d.defect, far_arc);  // closed: ends at Tr = start
    // The keystone: as wide as the defect, its ends radial (90 degrees to the
    // defect's edge), so its outer edge is longer than its inner one.
    const double W = 2.0 * R;
    const V Ol = near_c + (rho + W) * f.dir(half), Or = near_c + (rho + W) * f.dir(-half);
    // The cut: from one tip out to Ol, round the outer arc to Or, back in
    // to the other tip.
    Polyline outer{Tl};
    join(outer, arc(f, near_c, rho + W, half, -half));  // Ol .. Or
    outer.push_back(Tr);
    d.incisions = {outer};
    Polyline island = near_arc;  // Tr .. Tl
    for (auto it = outer.begin() + 1; it != outer.end(); ++it) island.push_back(*it);  // Ol .. Or, Tr
    d.flaps = {island};
    d.islands = {island};
    d.pivot = f.c;
    d.length_to_width = W / (2.0 * R);
    // The keystone's inner edge onto the far side of the defect...
    for (double s : {0.0, 0.5, -0.5}) {
        const double ang = s * half;
        const V p = near_c + (rho + bite) * f.dir(ang);
        const V q = far_c + (rho + bite) * f.dir(kPi - ang);  // (its mirror image)
        d.key_sutures.push_back({p, q});
    }
    // ...its outer edge to the skin beyond (closed as the keystone moves
    // in, by recruiting the skin around it)...
    for (double s : {0.0, 0.55, -0.55}) {
        const double ang = s * half;
        d.key_sutures.push_back({V(near_c + (rho + W - bite) * f.dir(ang)), V(near_c + (rho + W + bite) * f.dir(ang))});
    }
    // ...and its outer corners to the defect's tips (V-Y at each end).
    d.key_sutures.push_back({bite_into_corner(Ol, Tl, V(near_c + (rho + W) * f.dir(0.8 * half)), bite),
                             V(Tl + bite * f.n)});
    d.key_sutures.push_back({bite_into_corner(Or, Tr, V(near_c + (rho + W) * f.dir(-0.8 * half)), bite),
                             V(Tr - bite * f.n)});
}

// --- rotation family ----------------------------------------------------------

void rotation(FlapDesign& d, const Frame& f, double R, const FlapShape& shape, double bite) {
    const V& u = f.u;
    const V& w = f.n;
    const V& c = f.c;
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
    auto angle = [&](const V& p) { return f.angle_of(p, A); };
    auto at = [&](double radius, double a) { return V(A + radius * f.dir(a)); };
    const double r = (B - A).norm();
    const double aB = angle(B), aC = angle(C);
    // Increasing angle from B moves away from C, around the far side.
    const double sweep = std::clamp(shape.arc_angle_deg, 30.0, 270.0) * kDeg;
    Polyline arc_pts = arc(f, A, r, aB, aB + sweep);
    arc_pts.front() = B;
    const V P = arc_pts.back();
    d.incisions = {arc_pts};
    Polyline flap{A};
    flap.insert(flap.end(), arc_pts.begin(), arc_pts.end());
    flap.push_back(A);
    d.flaps = {flap};
    d.bases = {{P, A}};
    d.pivot = A;
    d.turn_deg = (aB - aC) / kDeg;  // the triangle's apex angle
    d.length_to_width = r * sweep / (2.0 * b);  // arc length over the defect's width (textbooks: ~4)
    // Key sutures: the flap's leading corner to the triangle's far corner,
    // and halfway along those edges. Each needle hole a bite inside the
    // flap (turned back from edge AB) or outside the defect (beyond AC).
    for (double rad : {r - bite, 0.5 * r}) {
        const double da = bite / rad;
        d.key_sutures.push_back({at(rad, aB + da), at(rad, aC - da)});
    }
}

void o_z(FlapDesign& d, const Frame& f, double R, const FlapShape& shape, double bite) {
    const double Rp = polygon_radius(R);
    d.defect = circle_with(f, Rp, {0.0, kPi});
    const V P1 = f.at(Rp, 0.0), P2 = f.at(-Rp, 0.0);
    // Arc 1 leaves the defect at P1, about 27 degrees off the tangent (no
    // sliver of a corner), curving around the +n side, centred two radii
    // beyond the defect; arc 2 is its copy turned 180 degrees about the
    // centre. The defect closes along the diameter P1-P2: the scar is arc 1,
    // the diameter and arc 2, a Z (an S). (Tried: centring the arc on the
    // far pole needs a much tighter closure for the same defect.)
    const V Q1 = f.at(0.0, 2.0 * Rp);
    const double r = (P1 - Q1).norm();
    const double a0 = f.angle_of(P1, Q1);
    const double sweep = std::clamp(shape.arc_angle_deg, 45.0, 160.0) * kDeg;
    Polyline arc1 = arc(f, Q1, r, a0, a0 + sweep);
    arc1.front() = P1;
    Polyline arc2;
    for (const V& p : arc1) arc2.push_back(2.0 * f.c - p);
    d.incisions = {arc1, arc2};
    // The base runs from the arc's end back to the far pole, passing outside
    // the defect (via a point beside the pole).
    const V K1 = f.at(-Rp, 1.5 * Rp), K2 = 2.0 * f.c - K1;
    auto flap_of = [&](const Polyline& a, const V& from, const V& via, double side) {
        // The defect's half on that side, from the other pole round to the
        // arc's start, then the arc, then back along the base.
        Polyline p = arc(f, f.c, Rp, side > 0 ? 0.0 : kPi, side > 0 ? kPi : 2.0 * kPi);
        std::reverse(p.begin(), p.end());  // other pole .. arc start
        p.back() = from;
        join(p, a);
        p.push_back(via);
        close(p);
        return p;
    };
    d.flaps = {flap_of(arc1, P1, K1, 1.0), flap_of(arc2, P2, K2, -1.0)};
    d.bases = {{arc1.back(), K1, P2}, {arc2.back(), K2, P1}};
    d.pivot = P2;
    d.turn_deg = 30.0;  // each flap turns until its edge reaches the diameter
    d.length_to_width = r * sweep / (2.0 * R);
    // The two flaps' leading edges meet across the middle.
    d.key_sutures.push_back({f.at(0.0, Rp + bite), f.at(0.0, -Rp - bite)});
    // Each point of a flap's edge meets its mirror image across the
    // closure line (the diameter through the arcs' starts).
    for (double a : {50.0, 130.0})
        d.key_sutures.push_back({f.c + (Rp + bite) * f.dir(a * kDeg), f.c + (Rp + bite) * f.dir(-a * kDeg)});
}

// --- transposition family -------------------------------------------------------

void rhomboid(FlapDesign& d, const Frame& f, double R, double bite) {
    const V& u = f.u;
    const V& a = f.n;
    const V& c = f.c;
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
    d.flaps = {closed({A, B, E, F})};
    d.bases = {{F, A}};
    d.pivot = A;
    d.turn_deg = 60.0;
    d.length_to_width = 1.0;
    // Key sutures: flap corner E to defect corner C, flap corner B to D.
    d.key_sutures.push_back({bite_into_corner(E, B, F, bite), out_of_corner(C, c, bite)});
    d.key_sutures.push_back({bite_into_corner(B, A, E, bite), out_of_corner(D, c, bite)});
    // The donor site: the V the flap leaves between its two limbs closes in
    // a straight line, the lip along B-E onto the lip along E-F.
    const V g = 0.25 * (A + B + E + F);  // the flap's centre
    d.key_sutures.push_back({beside(B, E, g, bite), beside(E, F, g, bite)});
}

void dufourmentel(FlapDesign& d, const Frame& f, double R, const FlapShape& shape, double bite) {
    const V& u = f.u;
    const V& a = f.n;
    const V& c = f.c;
    // Rhombus with acute angle alpha and inscribed circle radius R: side
    // s = 2R / sin(alpha). Long diagonal along a, short along u.
    const double alpha = std::clamp(shape.rhombus_angle_deg, 50.0, 90.0) * kDeg;
    const double s = 2.0 * R / std::sin(alpha);
    const V A = c + s * std::cos(0.5 * alpha) * a;  // acute corner
    const V B = c + s * std::sin(0.5 * alpha) * u;  // obtuse corner, on the flap's side
    const V C = c - s * std::cos(0.5 * alpha) * a;
    const V D = c - s * std::sin(0.5 * alpha) * u;
    d.defect = closed({A, B, C, D});
    // First limb: bisects the angle between the extended short diagonal (u)
    // and the extended side A-B, one side long. Second limb: parallel to the
    // long diagonal, back towards A's side, one side long.
    const V E = B + s * (u + (B - A).normalized()).normalized();
    const V F = E + s * a;
    d.incisions = {{B, E, F}};
    d.flaps = {closed({A, B, E, F})};
    d.bases = {{F, A}};
    d.pivot = A;
    // How far the flap turns: its axis (base middle to tip E) onto the
    // defect's (A's side to C).
    const V axis_flap = E - 0.5 * (A + F), axis_defect = C - A;
    d.turn_deg = std::acos(std::clamp(axis_flap.normalized().dot(axis_defect.normalized()), -1.0, 1.0)) / kDeg;
    d.length_to_width = 1.0;
    d.key_sutures.push_back({bite_into_corner(E, B, F, bite), out_of_corner(C, c, bite)});
    d.key_sutures.push_back({bite_into_corner(B, A, E, bite), out_of_corner(D, c, bite)});
    const V g = 0.25 * (A + B + E + F);
    d.key_sutures.push_back({beside(B, E, g, bite), beside(E, F, g, bite)});
}

void bilobed(FlapDesign& d, const Frame& f, double R, double bite) {
    // Zitelli's design, in polar coordinates around the pivot P, one radius
    // from the defect's edge (2R from its centre): the defect at angle 0,
    // the first lobe (as wide as the defect) at 45 degrees, the second
    // (narrower, pointed) at 90. Angles grow towards u (the flap's side).
    const double Rp = polygon_radius(R);
    const V v = -f.n;  // P -> defect centre
    const V P = f.c - 2.0 * R * v;
    auto e = [&](double th) { return V(std::cos(th) * v + std::sin(th) * f.u); };
    auto at = [&](double rho, double th) { return V(P + rho * e(th)); };
    const double D = 2.0 * R;  // pivot to the centre of each lobe
    // Far edge of a circle of radius a centred at angle phi, along the ray
    // at angle th from P (NaN if the ray misses it).
    auto out = [&](double a, double phi, double th) {
        const double dth = th - phi;
        const double disc = a * a - D * D * std::sin(dth) * std::sin(dth);
        return disc < 0.0 ? std::nan("") : D * std::cos(dth) + std::sqrt(disc);
    };
    const double R1 = R, R2 = 0.8 * R;
    const double phi1 = 45.0 * kDeg, phi2 = 90.0 * kDeg;
    const double half2 = std::asin(R2 / D);
    auto envelope = [&](double th) {
        double r = 0.0;
        const double r1 = out(R1, phi1, th), r2 = out(R2, phi2, th);
        if (std::isfinite(r1)) r = std::max(r, r1);
        if (std::isfinite(r2)) r = std::max(r, r2);
        // The second lobe's pointed tip (it closes in a line).
        const double dt = std::abs(th - phi2) / kDeg;
        if (dt < 18.0) r = std::max(r, 3.4 * R - 0.8 * R * dt / 15.0);
        return r;
    };
    const double defect_out_r = Rp;  // the defect polygon's circle
    auto defect_out = [&](double th) { return out(defect_out_r, 0.0, th); };
    // Where the first lobe's outline leaves the defect: bisection on angle.
    double lo = 1.0 * kDeg, hi = 30.0 * kDeg;
    for (int k = 0; k < 60; ++k) {
        const double mid = 0.5 * (lo + hi);
        const double dd = defect_out(mid);
        if (!std::isfinite(dd) || envelope(mid) > dd) hi = mid; else lo = mid;
    }
    const double th_x = hi;
    const V X = at(envelope(th_x), th_x);
    // Burow's triangle: from P tangent to the defect circle.
    const double th_t = std::asin(Rp / D);
    const double rho_t = std::sqrt(D * D - Rp * Rp);
    const V Tp = at(rho_t, th_t), Tm = at(rho_t, -th_t);
    // Defect + Burow's triangle as one outline (a teardrop), with X on it.
    Frame fc{f.c, f.u, f.n};
    const double ang_X = fc.angle_of(X, f.c), ang_Tp = fc.angle_of(Tp, f.c), ang_Tm = fc.angle_of(Tm, f.c);
    Polyline circ = circle_with(fc, Rp, {ang_X, ang_Tp, ang_Tm});
    circ.pop_back();
    // Walk the circle from Tm the long way round (away from P) to Tp.
    auto index_of = [&](const V& q) {
        size_t best = 0;
        for (size_t i = 1; i < circ.size(); ++i)
            if ((circ[i] - q).norm() < (circ[best] - q).norm()) best = i;
        return best;
    };
    const size_t iTm = index_of(Tm), iTp = index_of(Tp);
    const size_t n = circ.size();
    // Direction: the neighbour of Tm farther from P.
    const int step = (circ[(iTm + 1) % n] - P).norm() > (circ[(iTm + n - 1) % n] - P).norm() ? 1 : -1;
    Polyline teardrop{P};
    for (size_t i = iTm;; i = (i + n + step) % n) {
        teardrop.push_back(circ[i]);
        if (i == iTp) break;
    }
    close(teardrop);
    d.defect = teardrop;
    // The lobes' outer incision, from X round to the second lobe's far side.
    const double th_end = phi2 + half2;
    Polyline outer;
    const double th_step = 1.0 / (3.0 * R);  // about 1 mm along the outline
    for (double th = th_x; th < th_end; th += th_step) outer.push_back(at(envelope(th), th));
    outer.front() = X;
    outer.push_back(at(D * std::cos(half2), th_end));
    d.incisions = {outer};
    // The flap: P, the Burow edge to Tp, the defect's edge to X, the lobes,
    // and the base back to P.
    Polyline flap{P};
    for (size_t i = iTp;; i = (i + n - step) % n) {
        flap.push_back(circ[i]);
        if ((circ[i] - X).norm() < 1e-9) break;
        if (flap.size() > n + 2) break;  // safety
    }
    join(flap, outer);
    close(flap);
    d.flaps = {flap};
    d.bases = {{outer.back(), P}};
    d.pivot = P;
    d.turn_deg = 45.0;  // each lobe, about P (90 in all)
    d.length_to_width = 1.0;
    // Key sutures: the first lobe's tip onto the defect's far edge (45
    // degrees round)...
    d.key_sutures.push_back({at(envelope(phi1) - bite, phi1), at(D + Rp + bite, 0.0)});
    // ...the flap's edge beside the Burow's triangle onto its other side...
    const double rho_b = 0.6 * rho_t;
    const double db = bite / rho_b;
    d.key_sutures.push_back({at(rho_b, th_t + db), at(rho_b, -th_t - db)});
    // ...and the second lobe's donor site closed in a line.
    const double a1 = phi2 - 15.0 * kDeg, a2 = phi2 + 15.0 * kDeg;
    d.key_sutures.push_back({at(envelope(a1) + bite, a1), at(envelope(a2) + bite, a2)});
}

void z_plasty(FlapDesign& d, const Frame& f, double L, const FlapShape& shape, double bite) {
    const double alpha = std::clamp(shape.z_angle_deg, 30.0, 90.0) * kDeg;
    const V C1 = f.c, C2 = f.c + L * f.u;
    const V T1 = C1 + L * f.dir(alpha);
    const V T2 = C2 - L * f.dir(alpha);
    d.defect.clear();
    d.incisions = {{T1, C1, C2, T2}};
    // Flap A: tip C1 (between the central limb and limb C1-T1), base T1-C2.
    // Flap B: tip C2, base T2-C1. Each turns by alpha about its base's end
    // on the central limb, A's tip landing on T2 and B's on T1.
    d.flaps = {closed({C1, C2, T1}), closed({C2, C1, T2})};
    d.bases = {{T1, C2}, {T2, C1}};
    d.pivot = C2;
    d.turn_deg = alpha / kDeg;
    d.length_to_width = 1.0;
    d.lengthening = (T1 - T2).norm() / L - 1.0;
    const V mid = 0.5 * (C1 + C2);
    d.key_sutures.push_back({bite_into_corner(C1, C2, T1, bite), out_of_corner(T2, mid, bite)});
    d.key_sutures.push_back({bite_into_corner(C2, C1, T2, bite), out_of_corner(T1, mid, bite)});
}

}  // namespace

FlapDesign design_flap(FlapType type, const Eigen::Vector2d& centre, const Eigen::Vector2d& direction,
                       double defect_diameter, const FlapShape& shape, double bite) {
    FlapDesign d;
    d.type = type;
    d.centre = centre;
    d.defect_diameter = defect_diameter;
    if (!centre.allFinite() || !direction.allFinite() || direction.norm() < 1e-9) return d;
    const V u = direction.normalized();
    const V side = shape.mirror ? V(-perp(u)) : perp(u);
    const Frame f{centre, u, side};
    if (type == FlapType::ZPlasty) {
        const double L = std::max(direction.norm(), 4.0);
        d.defect_diameter = 0.0;
        z_plasty(d, f, L, shape, std::clamp(std::isfinite(bite) ? bite : 2.0, 0.2, 0.15 * L));
        d.valid = true;
        return d;
    }
    if (!(defect_diameter > 0.0) || !std::isfinite(defect_diameter)) return d;
    const double R = 0.5 * defect_diameter;
    bite = std::clamp(std::isfinite(bite) ? bite : 2.0, 0.2, R);
    switch (type) {
        case FlapType::Advancement: advancement(d, f, R, shape, bite); break;
        case FlapType::Rotation: rotation(d, f, R, shape, bite); break;
        case FlapType::Rhomboid: rhomboid(d, f, R, bite); break;
        case FlapType::HPlasty: h_plasty(d, f, R, shape, bite); break;
        case FlapType::ATPlasty: a_t_plasty(d, f, R, bite); break;
        case FlapType::VYIsland: v_y_island(d, f, R, shape, bite); break;
        case FlapType::Keystone: keystone(d, f, R, bite); break;
        case FlapType::OZ: o_z(d, f, R, shape, bite); break;
        case FlapType::Dufourmentel: dufourmentel(d, f, R, shape, bite); break;
        case FlapType::Bilobed: bilobed(d, f, R, bite); break;
        default: return d;
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
