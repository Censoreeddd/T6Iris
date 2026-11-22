// tinyexr's implementation, on its own (without the precompiled header, its warnings off): OpenEXR images for the Sky
// (HdrImage). OpenMP and threads off: one image read at a time, on the render thread.
#define TINYEXR_USE_OPENMP 0
#define TINYEXR_USE_THREAD 0
#define TINYEXR_IMPLEMENTATION
#include "tinyexr.h"
