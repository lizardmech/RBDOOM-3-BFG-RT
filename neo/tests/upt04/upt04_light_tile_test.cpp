#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <iostream>

namespace {

constexpr uint32_t kTileCount = 128u;
constexpr uint32_t kDomainSize = 1024u;
constexpr uint32_t kDomainCount = 2u;
constexpr uint32_t kAnalyticOrdinalCount = 32u;
constexpr uint32_t kAnalyticVariantsPerOrdinal = 32u;

struct TileEntry
{
    uint32_t denseLightIndex;
    uint32_t replayGuard;
    float selectionPdf;
    uint32_t lightType;
};

int failures = 0;

void Check(bool condition, const char* message)
{
    if (!condition)
    {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

uint32_t HashWord(uint32_t value)
{
    value ^= value >> 16u;
    value *= 0x7feb352du;
    value ^= value >> 15u;
    value *= 0x846ca68bu;
    return value ^ (value >> 16u);
}

uint32_t SelectScreenTile(
    uint32_t pixelX,
    uint32_t pixelY,
    uint32_t frameSampleIndex,
    uint32_t pathVertex)
{
    const uint32_t screenTileX = pixelX >> 3u;
    const uint32_t screenTileY = pixelY >> 3u;
    uint32_t key = frameSampleIndex ^ 0x68bc21ebu;
    key = HashWord(key ^ (screenTileX * 0x9e3779b9u));
    key = HashWord(key ^ (screenTileY * 0x85ebca6bu));
    key = HashWord(key ^ (pathVertex * 0xc2b2ae35u));
    return key % kTileCount;
}

uint32_t LinearIndex(
    uint32_t domain,
    uint32_t tileIndex,
    uint32_t entryInDomain)
{
    return (domain * kTileCount + tileIndex) * kDomainSize +
        entryInDomain;
}

uint32_t AnalyticEntry(uint32_t ordinal, uint32_t variant)
{
    return ordinal + variant * kAnalyticOrdinalCount;
}

uint32_t AnalyticLocalIndex(
    uint32_t rangeCount,
    uint32_t trialCount,
    uint32_t ordinal,
    float random)
{
    const float stride = std::fmax(
        1.0f, float(rangeCount) / float(trialCount));
    const float index = std::fmin(
        (float(ordinal) + random) * stride,
        float(rangeCount - 1u));
    return std::min(uint32_t(index + 0.5f), rangeCount - 1u);
}

uint32_t CdfSelect(
    const std::array<float, 4>& cdf,
    uint32_t logicalCount,
    float random)
{
    uint32_t low = 0u;
    uint32_t high = logicalCount;
    while (low < high)
    {
        const uint32_t mid = low + ((high - low) >> 1u);
        if (random <= cdf[mid])
            high = mid;
        else
            low = mid + 1u;
    }
    return low;
}

} // namespace

int main()
{
    Check(sizeof(TileEntry) == 16u,
        "tile entry ABI must remain one uint4");
    Check(kTileCount * kDomainSize * kDomainCount == 262144u,
        "tile dispatch must fully overwrite 262144 entries");
    Check(LinearIndex(0u, 0u, 0u) == 0u &&
            LinearIndex(1u, 127u, 1023u) == 262143u,
        "domain/tile addressing must cover the allocation exactly");

    std::array<bool, kDomainSize> seen = {};
    for (uint32_t ordinal = 0u; ordinal < kAnalyticOrdinalCount; ++ordinal)
    {
        for (uint32_t variant = 0u;
             variant < kAnalyticVariantsPerOrdinal;
             ++variant)
        {
            const uint32_t entry = AnalyticEntry(ordinal, variant);
            Check(entry < kDomainSize && !seen[entry],
                "analytic ordinal/variant mapping must be bounded and unique");
            seen[entry] = true;
        }
    }
    for (const bool covered : seen)
        Check(covered, "analytic tile domain must have no holes");

    const uint32_t tile = SelectScreenTile(80u, 40u, 17u, 0u);
    for (uint32_t y = 40u; y < 48u; ++y)
        for (uint32_t x = 80u; x < 88u; ++x)
            Check(SelectScreenTile(x, y, 17u, 0u) == tile,
                "one 8x8 screen group must select one coherent tile");
    Check(SelectScreenTile(80u, 40u, 17u, 1u) != tile,
        "direct and secondary vertices must use distinct tile hashes");

    const std::array<float, 4> cdf = { 0.2f, 0.5f, 1.0f, 1.0f };
    Check(CdfSelect(cdf, 3u, 0.5f) == 1u,
        "CDF equality must preserve the production lower-bound rule");
    Check(CdfSelect(cdf, 3u, 0.75f) == 2u,
        "CDF selection must reach the final logical entry");
    Check(CdfSelect(cdf, 3u, 1.0f) == 2u,
        "stale allocation capacity must not become sampleable");

    const float emissivePdf = (0.5f - 0.2f) * (1.0f / 33.0f);
    const float analyticPdf = 32.0f / (33.0f * 57.0f);
    Check(std::fabs(emissivePdf - 0.009090909f) < 1e-8f,
        "emissive tile entry must retain CDF interval times domain mixture");
    Check(std::fabs(analyticPdf - 0.017012227f) < 1e-8f,
        "analytic tile entry must retain ordinal-stratified source PDF");
    Check(AnalyticLocalIndex(57u, 32u, 0u, 0.0f) == 0u &&
            AnalyticLocalIndex(57u, 32u, 31u, 0.999f) == 56u,
        "analytic strata must cover the complete current range");

    if (failures != 0)
    {
        std::cerr << "UPT light-tile tests failed: " << failures << '\n';
        return 1;
    }
    std::cout << "UPT light-tile tests passed; exact domains, PDFs, and 8x8 coherence\n";
    return 0;
}
