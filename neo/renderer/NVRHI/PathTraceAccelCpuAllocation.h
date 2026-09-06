#pragma once

// Dependency-light failure seam shared by the real Lane-B snapshot fillers
// and product transaction. Production leaves it disabled (ordinal -1).
void PathTraceAccelCpuPackMaybeInjectAllocationFailure();
void PathTraceAccelCpuPackSetAllocationFailureForTest(
    int reserveOrdinal, bool lengthError);
