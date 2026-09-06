#pragma once

#include "PathTraceDoomMaterialClassifierKernel.h"
#include <array>

struct RtSmokeClassifierNameCacheStats
{
    std::uint64_t hits = 0;
    std::uint64_t misses = 0;
    std::uint64_t bypasses = 0;
};

// Only name-derived facts belong here. No material identities, stages, registers,
// image bindings or renderer pointers survive a call. An exact owned key also
// makes reload, allocator address reuse and world reset independent of this cache.
class RtSmokeClassifierNameCache
{
public:
    template <typename Builder>
    RtSmokeClassifierNameInfo Get(const char* name, Builder build)
    {
        std::uint32_t hash = 2166136261u;
        std::size_t length = 0;
        if (name)
        {
            for (; length < RT_SMOKE_TRANSLUCENT_CLASSIFIER_NAME_CAPACITY; ++length)
            {
                const unsigned char c = static_cast<unsigned char>(name[length]);
                if (c == 0) { break; }
                // Preserve the original locale-sensitive oracle for non-ASCII names.
                if (c >= 128) { ++stats.bypasses; return build(name); }
                hash = (hash ^ c) * 16777619u;
            }
        }
        if (!name || length == RT_SMOKE_TRANSLUCENT_CLASSIFIER_NAME_CAPACITY)
        {
            ++stats.bypasses;
            return build(name);
        }
        Entry& entry = entries[hash % entries.size()];
        if (entry.valid && entry.length == length &&
            std::memcmp(entry.name, name, length + 1) == 0)
        {
            ++stats.hits;
            return entry.value;
        }
        ++stats.misses;
        const RtSmokeClassifierNameInfo value = build(name);
        // Compute first so the old entry remains coherent if the builder throws.
        entry.valid = false;
        std::memcpy(entry.name, name, length + 1);
        entry.length = length;
        entry.value = value;
        entry.valid = true;
        return value;
    }

    RtSmokeClassifierNameCacheStats Stats() const { return stats; }

private:
    struct Entry
    {
        char name[RT_SMOKE_TRANSLUCENT_CLASSIFIER_NAME_CAPACITY] = {};
        std::size_t length = 0;
        RtSmokeClassifierNameInfo value;
        bool valid = false;
    };
    std::array<Entry, 512> entries;
    RtSmokeClassifierNameCacheStats stats;
};

static_assert(sizeof(RtSmokeClassifierNameCache) < 600 * 1024,
    "name cache must remain bounded per thread");

inline RtSmokeClassifierNameCache& SmokeThreadClassifierNameCache()
{
    static thread_local RtSmokeClassifierNameCache cache;
    return cache;
}
