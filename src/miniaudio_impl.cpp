// miniaudio.h is a single-header library: declarations are visible to every
// file that includes it, but the actual function bodies only get compiled
// in wherever MINIAUDIO_IMPLEMENTATION is defined before the include. This
// translation unit exists solely to do that, exactly once for the whole
// binary. Every other .cpp just #includes "miniaudio.h" normally for the
// declarations.
#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"
