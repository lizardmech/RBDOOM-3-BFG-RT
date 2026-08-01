#pragma once

#include <array>
#include <cstdint>

namespace rb::upt {

struct RandomSlot
{
    uint32_t streamNamespace;
    uint32_t dimension;
};

#define UPT_RANDOM_SLOT(name, streamValue, dimensionValue) \
    inline constexpr RandomSlot kRandomSlot##name{ streamValue, dimensionValue };
#include "../../shaders/PathTraceUnifiedPtRandomDimensions.inc"
#undef UPT_RANDOM_SLOT

inline constexpr auto kRandomSlots = std::array{
#define UPT_RANDOM_SLOT(name, streamValue, dimensionValue) kRandomSlot##name,
#include "../../shaders/PathTraceUnifiedPtRandomDimensions.inc"
#undef UPT_RANDOM_SLOT
};

constexpr bool RandomSlotsAreUnique()
{
    for (size_t left = 0; left < kRandomSlots.size(); ++left)
    {
        for (size_t right = left + 1; right < kRandomSlots.size(); ++right)
        {
            if (kRandomSlots[left].streamNamespace == kRandomSlots[right].streamNamespace &&
                kRandomSlots[left].dimension == kRandomSlots[right].dimension)
            {
                return false;
            }
        }
    }
    return true;
}

static_assert(RandomSlotsAreUnique(), "UPT random stream/dimension slots must be collision-free");

} // namespace rb::upt
