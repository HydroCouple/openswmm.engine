// D1 falsifier: a planted SDK include. The guard MUST fail on this file.
#include "hydrocouplesdk/componentabi.h"
int planted() { return 0; }
