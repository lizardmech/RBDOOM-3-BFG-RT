#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <vector>

static uint32_t CountCopies17x17(
    const std::vector<uint32_t>& ids,
    uint32_t width,
    uint32_t height,
    uint32_t x,
    uint32_t y)
{
    const uint32_t center = ids[y * width + x];
    if (center == 0u)
        return 0u;
    uint32_t count = 0u;
    for (int32_t dy = -8; dy <= 8; ++dy)
    for (int32_t dx = -8; dx <= 8; ++dx)
    {
        if (dx == 0 && dy == 0)
            continue;
        const int32_t nx = int32_t(x) + dx;
        const int32_t ny = int32_t(y) + dy;
        if (nx >= 0 && ny >= 0 && nx < int32_t(width) && ny < int32_t(height)
            && ids[uint32_t(ny) * width + uint32_t(nx)] == center)
            ++count;
    }
    return count;
}

static uint32_t QuantizeCorrelation(uint32_t copies)
{
    return std::min((copies * 255u + 144u) / 288u, 255u);
}

static uint32_t AdaptiveCap(uint32_t configuredCap, uint32_t score)
{
    const uint32_t defaultCap = std::min(configuredCap, 20u);
    const float c = float(score) / 255.0f;
    const float blend = std::pow(c, 0.1f);
    return std::max(uint32_t(std::lround(
        float(defaultCap) + (1.0f - float(defaultCap)) * blend)), 1u);
}

int main()
{
    constexpr uint32_t width = 25u;
    constexpr uint32_t height = 25u;
    std::vector<uint32_t> ids(width * height, 0u);

    // Invalid ID zero never correlates with another invalid lane.
    assert(CountCopies17x17(ids, width, height, 12u, 12u) == 0u);
    assert(AdaptiveCap(20u, QuantizeCorrelation(0u)) == 20u);
    assert(AdaptiveCap(32u, QuantizeCorrelation(0u)) == 20u);
    assert(AdaptiveCap(8u, QuantizeCorrelation(0u)) == 8u);

    // One isolated valid seed retains the configured temporal confidence.
    ids[12u * width + 12u] = 7u;
    assert(CountCopies17x17(ids, width, height, 12u, 12u) == 0u);

    // A completely duplicated 17x17 window excludes its center: 288 copies,
    // score 255, and the adaptive confidence cap collapses to one.
    for (uint32_t y = 4u; y <= 20u; ++y)
    for (uint32_t x = 4u; x <= 20u; ++x)
        ids[y * width + x] = 7u;
    assert(CountCopies17x17(ids, width, height, 12u, 12u) == 288u);
    assert(QuantizeCorrelation(288u) == 255u);
    assert(AdaptiveCap(20u, 255u) == 1u);

    // Packed UNORM8 transport must preserve all four adjacent scores.
    const uint32_t packed = 0u | (17u << 8u) | (128u << 16u) | (255u << 24u);
    assert(((packed >> 0u) & 0xffu) == 0u);
    assert(((packed >> 8u) & 0xffu) == 17u);
    assert(((packed >> 16u) & 0xffu) == 128u);
    assert(((packed >> 24u) & 0xffu) == 255u);
    return 0;
}
