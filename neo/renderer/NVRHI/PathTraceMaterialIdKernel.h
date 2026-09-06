#pragma once

#include <cstdint>

// Single pointer-free material-name identity authority shared by the live
// idMaterial wrapper and Lane A. The caller owns name extraction/lifetime.
inline std::uint32_t HashPathTraceMaterialName(const char* materialName)
{
    std::uint32_t hash = 2166136261u;
    const char* cursor = materialName ? materialName : "<none>";
    while (*cursor)
    {
        hash ^= static_cast<std::uint8_t>(*cursor++);
        hash *= 16777619u;
    }
    return hash != 0u ? hash : 1u;
}
