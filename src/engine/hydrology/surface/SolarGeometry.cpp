// SPDX-License-Identifier: Apache-2.0
#include "SolarGeometry.hpp"
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <vector>

namespace openswmm::surface {
namespace {
constexpr double pi = 3.14159265358979323846, rad = pi / 180;
struct Harmonic {
    double a, b, c;
    double value(double x) const { return a + b * std::cos(x) + c * std::sin(x); }
    double primitive(double x) const { return a*x + b*std::sin(x) - c*std::cos(x); }
    void roots(double lo, double hi, std::vector<double>& cuts) const {
        const double r = std::hypot(b, c);
        if (r == 0 || std::abs(a) >= r) return;
        const double offset = std::atan2(c, b), angle = std::acos(-a/r);
        for (double base : {offset-angle, offset+angle}) {
            const int first = int(std::ceil((lo-base)/(2*pi)));
            const int last = int(std::floor((hi-base)/(2*pi)));
            for (int k = first; k <= last; ++k) {
                const double x = base + k*2*pi;
                if (x > lo && x < hi) cuts.push_back(x);
            }
        }
    }
};
}
SolarIntegral solarIntegral(double latitude, double declination, double hour_start,
                            double duration_hours, double slope, double aspect) {
    for (double v : {latitude, declination, hour_start, duration_hours, slope, aspect})
        if (!std::isfinite(v)) throw std::invalid_argument("Nonfinite solar geometry.");
    if (std::abs(latitude) > 90 || std::abs(declination) > 90 || duration_hours <= 0 || duration_hours > 24 || slope < 0 || slope >= 90)
        throw std::invalid_argument("Invalid solar geometry range.");
    const double lat = latitude*rad, dec = declination*rad, tilt = slope*rad, azi = aspect*rad;
    const double north = std::sin(tilt)*std::cos(azi), east = std::sin(tilt)*std::sin(azi), up = std::cos(tilt);
    const Harmonic horizontal{std::sin(lat)*std::sin(dec), std::cos(lat)*std::cos(dec), 0};
    const Harmonic plane{std::sin(dec)*(north*std::cos(lat)+up*std::sin(lat)),
        std::cos(dec)*(up*std::cos(lat)-north*std::sin(lat)), -east*std::cos(dec)};
    const double lo = std::remainder(hour_start, 360.)*rad, hi = lo+duration_hours*pi/12;
    std::vector<double> cuts{lo, hi}; horizontal.roots(lo, hi, cuts); plane.roots(lo, hi, cuts);
    std::sort(cuts.begin(), cuts.end());
    SolarIntegral result;
    for (std::size_t i = 1; i < cuts.size(); ++i) {
        const double a = cuts[i-1], b = cuts[i], mid = .5*(a+b);
        if (horizontal.value(mid) <= 0) continue;
        result.horizontal += horizontal.primitive(b)-horizontal.primitive(a);
        if (plane.value(mid) > 0) result.cosine += plane.primitive(b)-plane.primitive(a);
    }
    result.cosine = std::clamp(result.cosine/(hi-lo), 0., 1.);
    result.horizontal = std::clamp(result.horizontal/(hi-lo), 0., 1.);
    result.hri = result.cosine/up;
    return result;
}
} // namespace openswmm::surface
