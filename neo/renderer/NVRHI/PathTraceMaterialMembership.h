#pragma once

#include <cstdint>
#include <cstddef>
#include <unordered_set>

// Pointer-free exact membership; hash collisions never establish identity.
class RtCpuMaterialMembership
{
public:
    static constexpr std::size_t kMaxRows = 65535;
    struct Key
    {
        uint32_t entity, surface, material;
        bool operator==(const Key& other) const
        { return entity == other.entity && surface == other.surface && material == other.material; }
    };
    struct Hash
    {
        std::size_t operator()(const Key& key) const noexcept
        {
            uint64_t value = key.entity;
            value = (value ^ key.surface) * 1099511628211ull;
            return static_cast<std::size_t>((value ^ key.material) * 1099511628211ull);
        }
    };
    void Clear() { keys.clear(); }
    void Reserve(std::size_t count) { keys.reserve(count < kMaxRows ? count : kMaxRows); }
    bool Add(uint32_t entity, uint32_t surface, uint32_t material)
    {
        const Key key{entity, surface, material};
        if (keys.size() == kMaxRows) return keys.find(key) != keys.end();
        keys.insert(key);
        return true;
    }
    bool Contains(uint32_t entity, uint32_t surface, uint32_t material) const
    { return !material || keys.find({entity, surface, material}) != keys.end(); }
    std::size_t Size() const { return keys.size(); }
private:
    std::unordered_set<Key, Hash> keys;
};
