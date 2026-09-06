#pragma once

#include <cstdint>
#include <tuple>
#include <unordered_map>
#include <vector>

// CPU identity only. Indices refer to the owner's frozen retained-vector order.
struct RtCpuRigidResolveKey
{
    uint64_t asset = 0, generation = 0, topology = 0;
    uint32_t surface = 0;
    bool operator==(const RtCpuRigidResolveKey& b) const
    { return std::tie(asset,generation,topology,surface) == std::tie(b.asset,b.generation,b.topology,b.surface); }
};
struct RtCpuRigidResolveRetained
{
    RtCpuRigidResolveKey key;
    uint64_t world = 0, signature = 0;
    bool hasBlas = false;
};
struct RtCpuRigidResolveQuery
{
    RtCpuRigidResolveKey earlyKey, candidateKey;
    uint64_t signature = 0;
    bool validMesh = false;
};
struct RtCpuRigidResolveRow
{
    int32_t early = -1, candidate = -1;
    bool append = false;
};
struct RtCpuRigidResolveWork
{
    static constexpr uint64_t kMaxBytes = 256ull * 1024 * 1024;
    static constexpr size_t kMaxRows = 1048576;
    uint64_t root = 0, world = 0, commitSerial = 0;
    bool haveProduct = false;
    std::vector<RtCpuRigidResolveRetained> retained;
    std::vector<RtCpuRigidResolveQuery> queries;
    std::vector<RtCpuRigidResolveRow> rows;
    uint64_t ChargedBytes() const { return (uint64_t(retained.size()) + queries.size()) * 512; }
    bool WithinCapacity() const
    { return retained.size() <= kMaxRows && queries.size() <= kMaxRows && ChargedBytes() <= kMaxBytes; }
};

inline bool BuildRtCpuRigidResolve(RtCpuRigidResolveWork& work)
{
    if (!work.WithinCapacity()) return false;
    struct Key
    {
        RtCpuRigidResolveKey mesh;
        uint64_t qualifier;
        bool operator==(const Key& b) const { return mesh == b.mesh && qualifier == b.qualifier; }
    };
    struct Hash
    {
        size_t operator()(const Key& k) const
        {
            uint64_t h = 1469598103934665603ull;
            for (uint64_t v : { k.mesh.asset,k.mesh.generation,k.mesh.topology,uint64_t(k.mesh.surface),k.qualifier })
            { h ^= v; h *= 1099511628211ull; h ^= h >> 32; }
            return size_t(h);
        }
    };
    std::unordered_map<Key,int32_t,Hash> early, candidate;
    early.reserve(work.retained.size());
    candidate.reserve(work.retained.size() + work.queries.size());
    for (size_t i = 0; i < work.retained.size(); ++i)
    {
        const auto& r = work.retained[i];
        // emplace retains the original vector's first matching row.
        if (r.hasBlas) early.emplace(Key {r.key,work.haveProduct ? r.signature : 0}, int32_t(i));
        if (work.haveProduct || r.hasBlas)
            candidate.emplace(Key {r.key,work.haveProduct ? r.world : 0}, int32_t(i));
    }
    work.rows.clear(); work.rows.reserve(work.queries.size());
    int32_t next = int32_t(work.retained.size());
    for (const auto& q : work.queries)
    {
        RtCpuRigidResolveRow row;
        if (!work.haveProduct || q.validMesh)
        {
            const auto e = early.find(Key {q.earlyKey,work.haveProduct ? q.signature : 0});
            if (e != early.end()) row.early = e->second;
            const Key key {work.haveProduct ? q.candidateKey : q.earlyKey,work.haveProduct ? work.world : 0};
            auto c = candidate.find(key);
            if (c != candidate.end()) row.candidate = c->second;
            else if (work.haveProduct)
            {
                row.candidate = next++; row.append = true;
                candidate.emplace(key,row.candidate);
            }
        }
        work.rows.push_back(row);
    }
    return true;
}
