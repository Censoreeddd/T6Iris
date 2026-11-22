#pragma once
#include "StdInclude.hpp"

// Diagnostic only, for finding the theater's times: while a film plays it logs a set of candidate fields once a second,
// and every change of the demo state. Addresses are t6mpv43.exe's (see RE-NOTES.md) and are only read when the game's
// PE timestamp is that build's; every read is guarded. Off unless the environment variable T6IRIS_PROBE is set.
namespace IWXMVM::T6::TheaterProbe
{
    void OnFrame();
}  // namespace IWXMVM::T6::TheaterProbe
