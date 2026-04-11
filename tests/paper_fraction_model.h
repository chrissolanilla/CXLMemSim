#include <algorithm>
#include <cmath>
// The paper models:
// cost1 = 1 / r
// cost2(x) = x / p + 1 / q(x)
// with
// p = k * r
// q(x) = (m*x + 1) * r
// Then define g(x) = cost1 / cost2(x)
// Larger g(x) means better benefit from moving x fraction.
//
// Closed-form:
// x* = (sqrt(m*k) - 1) / m
// Important:
// x* here is NOT the system-wide DRAM:CXL interleaving ratio.
// It is the paper's recommended FRACTION OF DATA TO MOVE
// from CXL memory to DRAM before execution.
//
// If m*k < 1, moving is not worthwhile, so choose x = 0.
//
// alpha is the paper's upper bound on valid moved fraction.
// In practice alpha is often the max useful DRAM fraction.
//
// This header is intentionally tiny and explicit.

struct PaperModelResult {
    double x_star = 0.0;
    double cost1 = 0.0;
    double cost2 = 0.0;
    double g = 1.0;
    bool movement_worth_it = false;
};

inline double clamp01(double x) {
    return std::max(0.0, std::min(1.0, x));
}

inline double paper_cost1(double r) {
    if (r <= 0.0) return 0.0;
    return 1.0 / r;
}

inline double paper_cost2(double x, double r, double k, double m) {
    if (r <= 0.0 || k <= 0.0) return 0.0;

    //p = k*r as paper defines
    const double p = k * r;

    //q(x) = (m*x + 1)*r as paper defines
    const double q = (m * x + 1.0) * r;

    if (q <= 0.0) return 0.0;

    return (x / p) + (1.0 / q);
}

inline double paper_g_of_x(double x, double r, double k, double m) {
    const double c1 = paper_cost1(r);
    const double c2 = paper_cost2(x, r, k, m);

    if (c2 <= 0.0) return 1.0;
    return c1 / c2;
}

inline double paper_optimal_fraction(double m, double k, double alpha) {
    if (m <= 0.0 || k <= 0.0 || alpha <= 0.0) {
        return 0.0;
    }

    //if m*k < 1, paper says movement is not worthwhile
    if (m * k < 1.0) {
        return 0.0;
    }

    const double raw = (std::sqrt(m * k) - 1.0) / m;
    return std::min(clamp01(raw), clamp01(alpha));
}

inline PaperModelResult evaluate_paper_model(double r, double k, double m, double alpha) {
    PaperModelResult out{};
    out.x_star = paper_optimal_fraction(m, k, alpha);
    out.cost1 = paper_cost1(r);
    out.cost2 = paper_cost2(out.x_star, r, k, m);
    out.g = paper_g_of_x(out.x_star, r, k, m);
    out.movement_worth_it = (m * k >= 1.0) && (out.x_star > 0.0);
    return out;
}
