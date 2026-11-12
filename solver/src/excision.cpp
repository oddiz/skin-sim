#include "skinsim/excision.hpp"

#include <algorithm>
#include <cmath>

namespace skinsim {

Polyline fusiform_outline(const Eigen::Vector2d& a, const Eigen::Vector2d& b, double length_to_width,
                          double apex_angle_deg, double spacing) {
    Polyline out;
    const Eigen::Vector2d axis = b - a;
    const double length = axis.norm();
    if (!(length > 0.0) || !std::isfinite(length)) return out;
    const double ratio = std::clamp(std::isfinite(length_to_width) ? length_to_width : 3.0, 1.0, 10.0);
    const double apex = std::clamp(std::isfinite(apex_angle_deg) ? apex_angle_deg : 30.0, 5.0, 170.0);
    const double half_width = 0.5 * length / ratio;
    // Slope of f at the tip, in units where both the half-length and the
    // half-width are 1. Kept below 3 so the outline stays convex at the
    // middle (f' < 0 all along): very blunt tips on a slim ellipse are not
    // possible with this shape and are clamped.
    const double m = std::clamp(std::tan(0.5 * apex * 3.14159265358979323846 / 180.0) * ratio, 0.05, 2.9);
    auto f = [&](double t) { return 1.0 + (m - 3.0) * t * t + (2.0 - m) * t * t * t; };

    const Eigen::Vector2d u = axis / length, n(-u.y(), u.x());
    const Eigen::Vector2d centre = 0.5 * (a + b);
    const int steps = std::max(8, static_cast<int>(std::ceil(length / std::max(spacing, 1e-3))));
    // s runs from -1 (tip a) to +1 (tip b) along one side, then back.
    for (int side : {1, -1})
        for (int k = 0; k < steps; ++k) {
            const double s = side > 0 ? -1.0 + 2.0 * k / steps : 1.0 - 2.0 * k / steps;
            out.push_back(centre + 0.5 * length * s * u + side * half_width * f(std::abs(s)) * n);
        }
    out.push_back(out.front());  // closed
    return out;
}

}  // namespace skinsim
