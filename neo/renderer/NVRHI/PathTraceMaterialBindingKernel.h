#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <tuple>
#include <unordered_map>
#include <vector>

// Resource eligibility was frozen by the owner; names and decisions are CPU-owned.
struct RtCpuMaterialBindingRule
{
    std::string name;
    bool descriptorEligible = false;
};
struct RtCpuMaterialBindingInput
{
    bool allowGui = false;
    uint64_t ownerCharge = 0;
    std::vector<RtCpuMaterialBindingRule> rules;
    uint64_t ChargedBytes() const
    {
        uint64_t bytes = uint64_t(rules.size()) * 512;
        for (const auto& rule : rules) bytes += uint64_t(rule.name.size()) * 3;
        return std::max(bytes, ownerCharge);
    }
};

inline bool RtCpuMaterialBindingNameSafe(const std::string& original, bool allowGui)
{
    if (original.empty()) return false;
    std::string name = original;
    for (char& c : name)
    {
        if (c == '\\') c = '/';
        else if (c >= 'A' && c <= 'Z') c = char(c + ('a' - 'A'));
    }
    const bool unstable = name[0] == '_' ||
        name.compare(0,5,"guis/") == 0 || name.compare(0,4,"gui/") == 0 ||
        name.compare(0,6,"video/") == 0 || name.compare(0,7,"videos/") == 0 ||
        name.compare(0,11,"cinematics/") == 0 || name.compare(0,10,"generated/") == 0 ||
        name.find("cinematic") != std::string::npos || name.find("scratch") != std::string::npos ||
        name.find("render") != std::string::npos;
    // Every unsafe nonempty name is GUI-like under the existing policy. A .swf
    // name without an unsafe prefix/substring is already accepted without override.
    return !unstable || allowGui;
}

inline std::vector<uint8_t> BuildRtCpuMaterialBindingSafety(const RtCpuMaterialBindingInput& input)
{
    std::vector<uint8_t> safety; safety.reserve(input.rules.size());
    for (const auto& rule : input.rules)
        safety.push_back(rule.descriptorEligible && RtCpuMaterialBindingNameSafe(rule.name,input.allowGui) ? 1u : 0u);
    return safety;
}

// Complete CPU binding plan. Resource indices are opaque ordinals into an
// owner-only pinned frame; they are never pointers or GPU handles.
struct RtCpuMaterialBindingSlot
{
    std::string name;
    uint32_t resource = 0;
    bool cube = false, descriptorEligible = false;
};
struct RtCpuMaterialBindingRow
{
    uint32_t materialId = 0;
    std::array<RtCpuMaterialBindingSlot,6> slots;
};
struct RtCpuMaterialBindingPlanInput
{
    static constexpr uint64_t kMaxBytes = 64ull * 1024 * 1024;
    bool allowGui = false;
    uint64_t ownerCharge = 0;
    std::vector<RtCpuMaterialBindingRow> rows;
    uint64_t ChargedBytes() const
    {
        uint64_t bytes = 256 + uint64_t(rows.size()) * (160 + 6 * 512);
        for (const auto& row : rows) for (const auto& slot : row.slots)
            bytes += uint64_t(slot.name.size()) * 5;
        return std::max(bytes, ownerCharge);
    }
};
struct RtCpuMaterialBindingPlan
{
    std::vector<RtCpuMaterialBindingSlot> rules;
    std::unordered_map<uint32_t,std::array<uint32_t,6>> rows;
    std::vector<uint8_t> safety;
};
inline bool BuildRtCpuMaterialBindingPlan(const RtCpuMaterialBindingPlanInput& input,
    RtCpuMaterialBindingPlan& output)
{
    if (input.rows.size() > 65535 || input.ChargedBytes() > input.kMaxBytes) return false;
    RtCpuMaterialBindingPlan candidate;
    candidate.rows.reserve(input.rows.size());
    std::map<std::tuple<uint32_t,std::string,bool>,uint32_t> rules;
    for (const auto& source : input.rows)
    {
        std::array<uint32_t,6> row;
        for (size_t i = 0; i < row.size(); ++i)
        {
            const auto& slot = source.slots[i];
            const auto key = std::make_tuple(slot.resource,slot.name,slot.cube);
            auto found = rules.find(key);
            if (found == rules.end())
            {
                const auto index = static_cast<uint32_t>(candidate.rules.size());
                candidate.rules.push_back(slot);
                candidate.safety.push_back(slot.descriptorEligible &&
                    RtCpuMaterialBindingNameSafe(slot.name,input.allowGui) ? 1u : 0u);
                found = rules.emplace(key,index).first;
            }
            else if (candidate.rules[found->second].descriptorEligible != slot.descriptorEligible) return false;
            row[i] = found->second;
        }
        if (!candidate.rows.emplace(source.materialId,row).second) return false;
    }
    output = std::move(candidate);
    return true;
}
