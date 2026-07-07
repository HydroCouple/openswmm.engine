/*
 * legacy_xsect_stubs.c
 *
 * Stub global definitions so the legacy cross-section code (xsect.c) can be
 * compiled directly into the parity test in isolation, without dragging in the
 * full legacy globals.c / object model.
 *
 * The legacy dylib hides its internal xsect_* symbols, so the harness compiles
 * xsect.c (+ findroot.c) into the test executable instead. xsect.c references
 * these four global arrays only in its IRREGULAR / CUSTOM / STREET code paths,
 * which the parity harness deliberately does NOT exercise (those need real
 * transect/curve data). The stubs exist solely to satisfy the linker.
 */
#include "headers.h"

TTable*    Curve    = NULL;
TTransect* Transect = NULL;
TStreet*   Street   = NULL;
TShape*    Shape    = NULL;
