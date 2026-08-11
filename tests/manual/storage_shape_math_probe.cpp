// Standalone probe: does StorageGeometry.hpp reproduce legacy node.c:713-752 exactly,
// and do the quadratic/cubic relations invert correctly?
// Build: g++ -std=c++17 -I src/engine tests/manual/storage_shape_math_probe.cpp -o /tmp/probe
#include "data/StorageGeometry.hpp"
#include <cmath>
#include <cstdio>
using namespace openswmm;

static const double PI = 3.141592653589793;

// Legacy node.c:713-752, transcribed independently as the oracle.
static void legacy_coeffs(int m, double y0, double y1, double y2,
                          double* a1, double* a2, double* a0) {
    double A, B, L, W, Z;
    *a1 = 0; *a2 = 0; *a0 = 0;
    switch (m) {
        case 2: A = y0/2.; B = y1/2.; *a1 = 0.0; *a2 = 0.0; *a0 = PI*A*B; break;         // CYLINDRICAL
        case 3: A = y0/2.; B = y1/2.; Z = y2;
                *a1 = 2.0*PI*B*Z; *a2 = PI*B/A*Z*Z; *a0 = PI*A*B; break;                // CONICAL
        case 4: A = y0/2.; B = y1/2.; Z = y2; *a1 = PI*A*B/Z; *a2 = 0.0; *a0 = 0.0; break; // PARABOLOID
        case 5: L = y0; W = y1; Z = y2; *a1 = 2.0*(L+W)*Z; *a2 = 4.0*Z*Z; *a0 = L*W; break; // PYRAMIDAL
    }
}
// Legacy node.c:955-957 / 993-999 for the geometric shapes.
static double area(double a,double b,double c,double d){ return c + d*(a + d*b); }
static double vol (double a,double b,double c,double d){ return d*(c + d*(a/2.0 + d*b/3.0)); }

int fails = 0;
static void chk(const char* what, double got, double want, double tol=1e-12) {
    double err = std::fabs(got-want);
    if (!(err <= tol)) { std::printf("  FAIL %-34s got=%.17g want=%.17g\n", what, got, want); ++fails; }
}

int main() {
    struct Case { const char* name; StorageShape s; int legacy; double p1,p2,p3; } cases[] = {
        {"CYLINDRICAL", StorageShape::CYLINDRICAL, 2, 30.0, 20.0, 0.0},
        {"CONICAL",     StorageShape::CONICAL,     3, 30.0, 20.0, 2.5},
        {"PARABOLOID",  StorageShape::PARABOLOID,  4, 30.0, 20.0, 8.0},
        {"PYRAMIDAL",   StorageShape::PYRAMIDAL,   5, 30.0, 20.0, 2.5},
    };
    for (auto& t : cases) {
        double a,b,c; bool ok = storage_shape_coeffs(t.s, t.p1,t.p2,t.p3, a,b,c);
        double la,lb,lc; legacy_coeffs(t.legacy, t.p1,t.p2,t.p3, &la,&lb,&lc);
        std::printf("%-12s a=%-14g b=%-14g c=%-14g  keyword=%s\n", t.name, a,b,c, storage_shape_keyword(t.s));
        if (!ok) { std::printf("  FAIL coeffs returned false\n"); ++fails; continue; }
        chk("a == legacy a1", a, la, 0.0);   // must be BIT-identical: same ops, same order
        chk("b == legacy a2", b, lb, 0.0);
        chk("c == legacy a0", c, lc, 0.0);

        // dV/dd == A(d):  the cubic must be the exact integral of the quadratic.
        for (double d : {0.5, 3.0, 7.25}) {
            const double h = 1e-6;
            double num = (vol(a,b,c,d+h) - vol(a,b,c,d-h)) / (2*h);
            chk("dV/dd == area(d)", num, area(a,b,c,d), 1e-4);
        }
        // Closed-form depth inversions used by getDepth().
        if (t.s == StorageShape::CYLINDRICAL) {
            double d = 4.0, v = vol(a,b,c,d);
            chk("cyl: d == v/c", v/c, d, 1e-12);
        }
        if (t.s == StorageShape::PARABOLOID) {
            double d = 4.0, v = vol(a,b,c,d);
            chk("par: d == sqrt(2v/a)", std::sqrt(2.0*v/a), d, 1e-12);
        }
    }
    // Validation rules (legacy node.c:700-708).
    double a,b,c;
    if ( storage_shape_coeffs(StorageShape::PYRAMIDAL,  0.0, 10.0, 1.0, a,b,c)) { std::printf("  FAIL L=0 accepted\n"); ++fails; }
    if ( storage_shape_coeffs(StorageShape::CONICAL,   10.0,  0.0, 1.0, a,b,c)) { std::printf("  FAIL W=0 accepted\n"); ++fails; }
    if ( storage_shape_coeffs(StorageShape::CONICAL,   10.0, 10.0,-1.0, a,b,c)) { std::printf("  FAIL Z<0 accepted\n"); ++fails; }
    if ( storage_shape_coeffs(StorageShape::PARABOLOID,10.0, 10.0, 0.0, a,b,c)) { std::printf("  FAIL paraboloid Z=0 accepted\n"); ++fails; }
    if ( storage_shape_coeffs(StorageShape::FUNCTIONAL,10.0, 10.0, 1.0, a,b,c)) { std::printf("  FAIL FUNCTIONAL derived coeffs\n"); ++fails; }
    if (!storage_shape_coeffs(StorageShape::CONICAL,   10.0, 10.0, 0.0, a,b,c)) { std::printf("  FAIL Z=0 conical rejected\n"); ++fails; }

    StorageShape s;
    if (!storage_shape_from_keyword("PARABOLIC", s)  || s != StorageShape::PARABOLOID) { std::printf("  FAIL kw PARABOLIC\n"); ++fails; }
    if (!storage_shape_from_keyword("PARABOLOID", s) || s != StorageShape::PARABOLOID) { std::printf("  FAIL kw PARABOLOID alias\n"); ++fails; }
    if ( storage_shape_from_keyword("BANANA", s))                                      { std::printf("  FAIL kw BANANA\n"); ++fails; }
    if (!storage_shape_is_valid_code(5) || storage_shape_is_valid_code(6) || storage_shape_is_valid_code(-1)) { std::printf("  FAIL valid_code\n"); ++fails; }

    std::printf("\n%s  (%d failure%s)\n", fails ? "FAILED" : "ALL PASS", fails, fails==1?"":"s");
    return fails ? 1 : 0;
}
