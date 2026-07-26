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

inline bool IsPathTraceBoundsOverlayDebugMode(int debugMode)
{
    return debugMode == 21 || debugMode == 22;
}

inline bool PathTraceDebugModeNeedsTextureProbe(int debugMode)
{
    return (debugMode >= 8 && debugMode <= 18) ||
        (debugMode >= 38 && debugMode <= 49) ||
        debugMode == 57;
}

inline bool PathTraceDebugModeNeedsTextureTable(int debugMode)
{
    return (debugMode >= 8 && debugMode <= 15) ||
        debugMode == 18 ||
        (debugMode >= 38 && debugMode <= 49);
}

inline bool PathTraceDebugModeUsesRigidRoute(int debugMode)
{
    return (debugMode >= 23 && debugMode <= 25) ||
        (debugMode >= 39 && debugMode <= 43) ||
        (debugMode >= 47 && debugMode <= 49) ||
        debugMode == 52 ||
        debugMode == 58;
}

inline bool PathTraceDebugModeRemovesRoutedRigidDynamic(int debugMode)
{
    return debugMode == 24 || debugMode == 25 ||
        (debugMode >= 39 && debugMode <= 43) ||
        (debugMode >= 47 && debugMode <= 49) ||
        debugMode == 52;
}
