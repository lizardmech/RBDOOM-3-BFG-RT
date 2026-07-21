#pragma once

// Retired ReSTIR PT diagnostic modes normalize to the production route at
// every public boundary. Keep this policy helper until those callers are
// collapsed; the old pass-plan and material-feature descriptions are gone.

inline int NormalizePathTraceDebugMode(int debugMode)
{
    if ((debugMode >= 26 && debugMode <= 33) ||
        debugMode == 50 || debugMode == 51 ||
        (debugMode >= 53 && debugMode <= 56))
    {
        return 0;
    }
    return debugMode;
}

inline bool IsPathTraceRestirPTDebugMode(int debugMode)
{
    return false;
}
