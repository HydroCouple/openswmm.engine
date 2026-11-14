// This file names HydroCoupleSDK and hydrocouplesdk/componentabi.h only in
// comments and a string, as the files stating decision D-C6 must. The guard
// MUST pass: it polices includes and links, not the word.
#include "hydrocouple.h"   // the header-only interfaces are fine
static const char* note = "never include hydrocouplesdk/ here (D-C6)";
int prose() { return note[0]; }
