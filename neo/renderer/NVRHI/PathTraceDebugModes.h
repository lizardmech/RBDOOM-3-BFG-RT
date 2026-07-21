#pragma once

// Retired diagnostics normalize to the production route at every public
// boundary while retained debug values keep their stable numeric identities.

inline int NormalizePathTraceDebugMode(int debugMode)
{
    if (debugMode == 19 || debugMode == 20 ||
        (debugMode >= 26 && debugMode <= 37) ||
        debugMode == 50 || debugMode == 51 ||
        (debugMode >= 53 && debugMode <= 56))
    {
        return 0;
    }
    return debugMode;
}
