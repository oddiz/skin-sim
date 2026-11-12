#include "skinsim/wrinkling.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

#include <Eigen/Cholesky>
#include <Eigen/Eigenvalues>
#include <Eigen/LU>

namespace skinsim {
namespace membrane {

namespace {

constexpr double kInf = std::numeric_limits<double>::infinity();
constexpr double kTiny = 1e-12;  // b or k2 below this: treated as exactly 0 (see material.cpp)

using Vec2 = Eigen::Vector2d;
using Mat2 = Eigen::Matrix2d;

Mat2 cofactor(const Mat2& A) {
    Mat2 C;
    C << A(1, 1), -A(1, 0), -A(0, 1), A(0, 0);
    return C;
}

// d(vec cof A)/d(vec A), constant in 2D (same matrix as in material.cpp).
Mat4 cofactor_derivative() {
    Mat4 H;
    H << 0, 0, 0, 1, 0, 0, -1, 0, 0, -1, 0, 0, 1, 0, 0, 0;
    return H;
}

Vec4 vec(const Mat2& A) { return Eigen::Map<const Vec4>(A.data()); }

// Projector onto symmetric changes: vec(A) -> vec((A + A^T)/2). Formulas below
// treat the 4 entries of C as independent, which is fine for W itself but
// would let the Hessian "see" the meaningless antisymmetric part; sandwiching
// it between this projector removes that.
Mat4 sym_projector() {
    Mat4 P = Mat4::Zero();
    P(0, 0) = P(3, 3) = 1.0;
    P(1, 1) = P(1, 2) = P(2, 1) = P(2, 2) = 0.5;
    return P;
}

// Scale of stresses in this material, for tolerances.
double stress_scale(const Material& m) { return m.mu + m.fibres.k1 + 1e-300; }

// The two derivatives of W along the ray C + beta u u^T, for the 1D search.
double slope_along(const Mat2& C, const Vec2& u, double beta, const Material& m) {
    const MetricDerivatives d = metric_derivatives(C + beta * u * u.transpose(), m, false);
    return u.dot(d.S * u);
}

// Wrinkled case: find w minimising W(C + w w^T). First a 1D search along the
// most compressed direction (exact for an isotropic material, where the
// wrinkles run along a principal direction), then 2D Newton to let the
// direction rotate (fibres make the material anisotropic).
Vec2 solve_wrinkle(const Mat2& C, const Mat2& S0, const Material& m) {
    Eigen::SelfAdjointEigenSolver<Mat2> eig(S0);
    const Vec2 u = eig.eigenvectors().col(0);  // most compressive direction

    // 1D: phi(beta) = W(C + beta u u^T) is convex, phi'(0) < 0. Bracket the
    // root of phi' and refine by bisection-safeguarded Newton.
    double lo = 0.0, hi = std::max(1.0 - u.dot(C * u), 0.1 * u.dot(C * u));
    for (int k = 0; k < 60 && slope_along(C, u, hi, m) < 0.0; ++k) {
        lo = hi;
        hi *= 2.0;
    }
    double beta = 0.5 * (lo + hi);
    const Vec4 uu = vec(u * u.transpose());
    for (int k = 0; k < 100 && hi - lo > 1e-15 * (1.0 + hi); ++k) {
        const MetricDerivatives d = metric_derivatives(C + beta * u * u.transpose(), m);
        const double g = u.dot(d.S * u);
        if (std::abs(g) <= 1e-15 * stress_scale(m)) break;  // converged: keep this beta
        (g < 0.0 ? lo : hi) = beta;
        const double h = uu.dot(d.K * uu);
        const double newton = beta - g / h;
        beta = (h > 0.0 && newton >= lo && newton <= hi) ? newton : 0.5 * (lo + hi);
    }
    Vec2 w = std::sqrt(std::max(beta, 0.0)) * u;
    if (m.fibres.k1 <= 0.0) return w;  // isotropic: already exact

    // 2D Newton on f(w) = W(C + w w^T), gradient 2 S* w, Hessian
    // 2 S* + Jw^T K* Jw, with Jw the derivative of vec(w w^T).
    auto f = [&](const Vec2& v) { return metric_derivatives(C + v * v.transpose(), m, false).W; };
    double fw = f(w);
    for (int it = 0; it < 30; ++it) {
        const MetricDerivatives d = metric_derivatives(C + w * w.transpose(), m);
        const Vec2 g = 2.0 * d.S * w;
        if (g.norm() <= 1e-14 * stress_scale(m) * std::max(w.norm(), 1.0)) break;
        Eigen::Matrix<double, 4, 2> Jw;
        for (int k = 0; k < 2; ++k) {
            Mat2 E = Mat2::Zero();
            E.row(k) = w.transpose();
            Jw.col(k) = vec(E + E.transpose());
        }
        Mat2 H = 2.0 * d.S + Jw.transpose() * d.K * Jw;
        // Away from the solution H may be indefinite: clip its eigenvalues.
        Eigen::SelfAdjointEigenSolver<Mat2> he(H);
        const double floor = 1e-8 * std::max(he.eigenvalues().cwiseAbs().maxCoeff(), 1e-300);
        H = he.eigenvectors() * he.eigenvalues().cwiseMax(floor).asDiagonal() * he.eigenvectors().transpose();
        const Vec2 step = -H.ldlt().solve(g);
        if (step.norm() < 1e-13 * std::max(w.norm(), 1e-6)) break;
        if (step.norm() < 1e-6 * w.norm()) {
            // Final polish: energy differences are now below rounding error, so
            // the line search cannot judge the step; Newton converges
            // quadratically here, so take it. (The stress depends on w to
            // first order, so w must be accurate, not only the energy.)
            w += step;
            continue;
        }
        double t = 1.0;
        bool moved = false;
        for (int cut = 0; cut < 30; ++cut, t *= 0.5) {
            const double ft = f(w + t * step);
            if (ft <= fw + 1e-4 * t * g.dot(step)) {
                w += t * step;
                fw = ft;
                moved = true;
                break;
            }
        }
        if (!moved) break;  // at the solution to within roundoff
    }
    return w;
}

}  // namespace

MetricDerivatives metric_derivatives(const Mat2& C, const Material& m, bool with_hessian) {
    MetricDerivatives out;
    const double JC = C.determinant();  // = J^2, squared area ratio
    if (!(JC > 0.0)) {
        out.W = kInf;
        return out;
    }
    // Q = I1 - 3 = tr C + 1/det C - 3: as in material.hpp, written with C.
    const double Q = C.trace() + 1.0 / JC - 3.0;
    const Vec4 c = vec(cofactor(C));  // d det C / d vec C
    const Vec4 q = vec(Mat2::Identity()) - c / (JC * JC);
    Vec4 grad = Vec4::Zero();
    Mat4 hess = Mat4::Zero();
    Mat4 Qcc = Mat4::Zero();
    if (with_hessian) Qcc = -cofactor_derivative() / (JC * JC) + (2.0 / (JC * JC * JC)) * c * c.transpose();

    // Isotropic matrix: W = mu/(2b) (exp(bQ) - 1).
    const double b = m.b;
    const double e = b > kTiny ? std::exp(b * Q) : 1.0;
    out.W = b > kTiny ? 0.5 * m.mu * std::expm1(b * Q) / b : 0.5 * m.mu * Q;
    grad += 0.5 * m.mu * e * q;
    if (with_hessian) hess += 0.5 * m.mu * e * (Qcc + (b > kTiny ? b : 0.0) * q * q.transpose());

    // Fibres: E = kappa Q + (1 - 3 kappa)(I4 - 1), I4 = a^T C a; only when taut.
    const Fibres& f = m.fibres;
    if (f.k1 > 0.0) {
        const Vec2& a = f.direction;
        const double E = f.kappa * Q + (1.0 - 3.0 * f.kappa) * (a.dot(C * a) - 1.0);
        if (E > 0.0) {
            const double x = f.k2 * E * E;
            const double ex = std::exp(x);
            out.W += f.k2 > kTiny ? 0.5 * f.k1 * std::expm1(x) / f.k2 : 0.5 * f.k1 * E * E;
            const Vec4 Ec = f.kappa * q + (1.0 - 3.0 * f.kappa) * vec(a * a.transpose());
            grad += f.k1 * E * ex * Ec;
            if (with_hessian)
                hess += f.k1 * (1.0 + 2.0 * x) * ex * Ec * Ec.transpose() + f.k1 * E * ex * f.kappa * Qcc;
        }
    }
    const Mat2 G = Eigen::Map<const Mat2>(grad.data());
    out.S = 0.5 * (G + G.transpose());
    if (with_hessian) {
        const Mat4 P = sym_projector();
        out.K = P * hess * P;
    }
    return out;
}

RelaxedState relax(const Mat2& F, const Material& m) {
    RelaxedState s;
    s.C = F.transpose() * F;
    s.C_eff = s.C;
    const MetricDerivatives d = metric_derivatives(s.C, m, false);
    if (!std::isfinite(d.W)) return s;  // collapsed: leave to the caller (energy is infinite)

    // Taut: the stress S (and so the Cauchy stress F S F^T) has no negative
    // principal value. Checked first so that the unstressed state counts as
    // taut and keeps its stiffness.
    Eigen::SelfAdjointEigenSolver<Mat2> se(d.S);
    if (se.eigenvalues()[0] >= 0.0) return s;

    // Slack: shorter than natural (C <= I) in every direction. Then D = I - C
    // is allowed and reaches W = 0, the lowest possible.
    Eigen::SelfAdjointEigenSolver<Mat2> ce(s.C);
    if (ce.eigenvalues()[1] <= 1.0) {
        s.state = MembraneState::Slack;
        s.C_eff = Mat2::Identity();
        return s;
    }

    s.state = MembraneState::Wrinkled;
    s.w = solve_wrinkle(s.C, d.S, m);
    s.C_eff = s.C + s.w * s.w.transpose();
    return s;
}

double relaxed_energy_density(const Mat2& F, const Material& m) {
    if (!(F.determinant() > 0.0)) return kInf;
    const RelaxedState s = relax(F, m);
    if (s.state == MembraneState::Slack) return 0.0;
    return metric_derivatives(s.C_eff, m, false).W;
}

Mat2 relaxed_piola(const Mat2& F, const Material& m) {
    const RelaxedState s = relax(F, m);
    if (s.state == MembraneState::Slack) return Mat2::Zero();
    // W(F) = W(F^T F): P = dW/dF = 2 F dW/dC. In the relaxed case dW/dC is
    // taken at the effective state (envelope theorem).
    return 2.0 * F * metric_derivatives(s.C_eff, m, false).S;
}

Mat4 relaxed_stiffness(const Mat2& F, const Material& m) {
    const RelaxedState s = relax(F, m);
    if (s.state == MembraneState::Slack) return Mat4::Zero();
    const MetricDerivatives d = metric_derivatives(s.C_eff, m);
    Mat4 Kc = d.K;  // d2W_rel/dC2
    if (s.state == MembraneState::Wrinkled) {
        // W_rel(C) = f(C, w(C)) with f(C, w) = W(C + w w^T) and df/dw = 0 at
        // w(C). Differentiating twice, w's dependence on C gives the "Schur
        // complement": K_rel = K - K Jw H^-1 Jw^T K, with H = d2f/dw2. It
        // softens the material along the wrinkles.
        const Vec2& w = s.w;
        Eigen::Matrix<double, 4, 2> Jw;
        for (int k = 0; k < 2; ++k) {
            Mat2 E = Mat2::Zero();
            E.row(k) = w.transpose();
            Jw.col(k) = vec(E + E.transpose());
        }
        const Mat2 H = 2.0 * d.S + Jw.transpose() * d.K * Jw;
        const Eigen::Matrix<double, 4, 2> KJ = d.K * Jw;
        Kc -= KJ * H.inverse() * KJ.transpose();
    }

    // Back from C to F: P = 2 F S(C), C = F^T F.
    //   dP = 2 dF S + 2 F dS,   dS = Kc dC,   dC_ij = dF_ki F_kj + F_ki dF_kj.
    // Indices in vec order: entry (i, j) of a 2x2 matrix is i + 2j.
    Mat4 B = Mat4::Zero();  // B(ij, kl) = dC_ij / dF_kl
    for (int i = 0; i < 2; ++i)
        for (int j = 0; j < 2; ++j)
            for (int k = 0; k < 2; ++k) {
                B(i + 2 * j, k + 2 * i) += F(k, j);
                B(i + 2 * j, k + 2 * j) += F(k, i);
            }
    Mat4 A = Mat4::Zero();  // A(ab, mb) = 2 F_am: vec(2 F X) = A vec(X)
    for (int b = 0; b < 2; ++b)
        for (int a = 0; a < 2; ++a)
            for (int mm = 0; mm < 2; ++mm) A(a + 2 * b, mm + 2 * b) = 2.0 * F(a, mm);
    Mat4 K = A * Kc * B;
    for (int a = 0; a < 2; ++a)
        for (int b = 0; b < 2; ++b)
            for (int l = 0; l < 2; ++l) K(a + 2 * b, a + 2 * l) += 2.0 * d.S(l, b);
    return 0.5 * (K + K.transpose());  // symmetric up to roundoff; make it exact
}

double excess_length(const RelaxedState& s) {
    switch (s.state) {
        case MembraneState::Taut: return 0.0;
        case MembraneState::Wrinkled: {
            // C^-1 (C + w w^T) has eigenvalues 1 and 1 + w^T C^-1 w: the
            // squared ratio of material length to spanned length along the
            // wrinkled direction.
            const double r = s.w.dot(s.C.inverse() * s.w);
            return std::sqrt(1.0 + std::max(r, 0.0)) - 1.0;
        }
        case MembraneState::Slack: {
            Eigen::SelfAdjointEigenSolver<Mat2> ce(s.C);
            const double lmin = ce.eigenvalues()[0];
            return lmin > 0.0 ? 1.0 / std::sqrt(lmin) - 1.0 : kInf;
        }
    }
    return 0.0;
}

}  // namespace membrane
}  // namespace skinsim
