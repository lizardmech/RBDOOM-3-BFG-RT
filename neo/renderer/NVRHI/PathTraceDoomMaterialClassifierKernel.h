#pragma once

#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstring>

constexpr std::size_t RT_SMOKE_TRANSLUCENT_CLASSIFIER_NAME_CAPACITY = 1024;

struct RtSmokeTranslucentClassifierInfo
{
    bool sortIsGuiOrSubview = false;
    bool sortIsDecal = false;
    bool sortIsPostProcess = false;
    bool polygonOffsetDecal = false;
    bool hasScreenTexgen = false;
    bool hasAdditiveBlend = false;
    bool hasAmbientStage = false;
    bool hasAmbientBlendStage = false;
    bool hasDiffuseStage = false;
    bool hasAddDefault0200Texture = false;
    bool nameLooksGui = false;
    bool nameLooksParticle = false;
    bool nameLooksDecal = false;
    bool nameLooksGlass = false;
    bool nameLooksGlow = false;
    bool nameLooksSignage = false;
};

struct RtSmokeTranslucentClassifierStageInput
{
    std::int32_t lighting = 0;
    std::int32_t texgen = 0;
    std::uint64_t drawStateBits = 0;
    std::int32_t conditionRegister = 0;
    bool valid = false;
    bool isRenderMap = false;
    bool hasImage = false;
    bool looksAddDefault0200 = false;
    bool hasScreenTexgen = false;
    bool isAdditiveBlend = false;
    bool isAmbientStage = false;
    bool isDiffuseStage = false;
    bool hasAmbientBlendStage = false;
};

struct RtSmokeTranslucentClassifierInput
{
    char materialName[RT_SMOKE_TRANSLUCENT_CLASSIFIER_NAME_CAPACITY] = {};
    float sort = 0.0f;
    std::int32_t stageCount = 0;
    bool materialPresent = false;
    bool sortIsGuiOrSubview = false;
    bool sortIsDecal = false;
    bool sortIsPostProcess = false;
    bool polygonOffsetDecal = false;
};

// Shared declaration-level decision used by the persistent material universe
// and by frontend capture.  It deliberately consumes only copied scalar facts;
// neither caller needs to acquire or mutate the other's persistent records.
inline bool RtSmokeMaterialEmissiveFactFromLocalFacts(
    bool emissive,
    bool hasEmissiveImage,
    bool hasSafeEmissiveTexture,
    bool skyEnvironment)
{
    return skyEnvironment ||
        (emissive && (hasSafeEmissiveTexture || !hasEmissiveImage));
}

inline bool RtSmokeTranslucentClassifierCaptureFits(
    std::size_t materialNameLength,
    std::int32_t stageCount,
    std::int32_t stageCapacity)
{
    return materialNameLength < RT_SMOKE_TRANSLUCENT_CLASSIFIER_NAME_CAPACITY &&
        stageCount >= 0 &&
        stageCapacity >= 0 &&
        stageCount <= stageCapacity;
}

inline bool RtSmokeClassifierNameContainsToken(
    const char* name,
    const char* token)
{
    if (!name || !token)
    {
        return false;
    }
    const std::size_t nameLength = std::strlen(name);
    const std::size_t tokenLength = std::strlen(token);
    if (tokenLength > nameLength)
    {
        return false;
    }
    for (std::size_t start = 0; start <= nameLength - tokenLength; ++start)
    {
        std::size_t tokenIndex = 0;
        for (; tokenIndex < tokenLength; ++tokenIndex)
        {
            if (::toupper(name[start + tokenIndex]) !=
                ::toupper(token[tokenIndex]))
            {
                break;
            }
        }
        if (tokenIndex == tokenLength)
        {
            return true;
        }
    }
    return false;
}

inline bool RtSmokeClassifierNameContainsAny(
    const char* name,
    const char* const* tokens,
    std::size_t tokenCount)
{
    for (std::size_t tokenIndex = 0; tokenIndex < tokenCount; ++tokenIndex)
    {
        if (RtSmokeClassifierNameContainsToken(name, tokens[tokenIndex]))
        {
            return true;
        }
    }
    return false;
}

struct RtSmokeClassifierNameInfo
{
    bool nameLooksGui = false;
    bool nameLooksParticle = false;
    bool nameLooksDecal = false;
    bool nameLooksGlass = false;
    bool nameLooksGlow = false;
    bool nameLooksSignage = false;
};

inline RtSmokeClassifierNameInfo BuildSmokeClassifierNameInfo(const char* materialName)
{
    RtSmokeClassifierNameInfo info;
    static const char* const guiTokens[] = {
        "gui", "guis/", "video", "cinematic", "terminal", "console", "pda", "cursor"};
    static const char* const particleTokens[] = {
        "particle", "smoke", "dust", "steam", "fog", "muzzle", "spark", "bloodcloud"};
    static const char* const decalTokens[] = {
        "decal", "stain", "grime", "dirt", "scorch", "burn", "bullet", "mud", "blood", "splat", "mark"};
    static const char* const glassTokens[] = {
        "glass", "window", "visor", "transparent"};
    static const char* const glowTokens[] = {
        "glow", "light", "lamp", "beam", "flare", "strip", "striplight", "tube", "neon", "emissive", "emit", "bulb", "fluoro", "flouro"};
    static const char* const signageTokens[] = {
        "logo", "sign", "label", "snack", "soda", "cola", "add", "screen", "monitor"};
    info.nameLooksGui = RtSmokeClassifierNameContainsAny(
        materialName, guiTokens, sizeof(guiTokens) / sizeof(guiTokens[0]));
    info.nameLooksParticle = RtSmokeClassifierNameContainsAny(
        materialName, particleTokens, sizeof(particleTokens) / sizeof(particleTokens[0]));
    info.nameLooksDecal = RtSmokeClassifierNameContainsAny(
        materialName, decalTokens, sizeof(decalTokens) / sizeof(decalTokens[0]));
    info.nameLooksGlass = RtSmokeClassifierNameContainsAny(
        materialName, glassTokens, sizeof(glassTokens) / sizeof(glassTokens[0]));
    info.nameLooksGlow = RtSmokeClassifierNameContainsAny(
        materialName, glowTokens, sizeof(glowTokens) / sizeof(glowTokens[0]));
    info.nameLooksSignage = RtSmokeClassifierNameContainsAny(
        materialName, signageTokens, sizeof(signageTokens) / sizeof(signageTokens[0]));

    return info;
}

template <typename StageProvider>
inline RtSmokeTranslucentClassifierInfo
BuildSmokeTranslucentClassifierInfoFromSource(
    const RtSmokeTranslucentClassifierInput& input,
    const char* materialName,
    std::int32_t stageCount,
    StageProvider stageProvider,
    const RtSmokeClassifierNameInfo* preparedNames = nullptr)
{
    RtSmokeTranslucentClassifierInfo info;
    if (!input.materialPresent || !materialName || stageCount < 0)
    {
        return info;
    }

    info.sortIsGuiOrSubview = input.sortIsGuiOrSubview;
    info.sortIsDecal = input.sortIsDecal;
    info.sortIsPostProcess = input.sortIsPostProcess;
    info.polygonOffsetDecal = input.polygonOffsetDecal;

    const RtSmokeClassifierNameInfo names = preparedNames
        ? *preparedNames : BuildSmokeClassifierNameInfo(materialName);
    info.nameLooksGui = names.nameLooksGui;
    info.nameLooksParticle = names.nameLooksParticle;
    info.nameLooksDecal = names.nameLooksDecal;
    info.nameLooksGlass = names.nameLooksGlass;
    info.nameLooksGlow = names.nameLooksGlow;
    info.nameLooksSignage = names.nameLooksSignage;

    for (std::int32_t stageIndex = 0; stageIndex < stageCount; ++stageIndex)
    {
        const RtSmokeTranslucentClassifierStageInput stage =
            stageProvider(stageIndex);
        if (!stage.valid)
        {
            continue;
        }
        info.hasScreenTexgen |= stage.hasScreenTexgen;
        info.hasAddDefault0200Texture |=
            stage.hasImage && stage.looksAddDefault0200;
        info.hasAdditiveBlend |= stage.isAdditiveBlend;
        info.hasAmbientStage |= stage.isAmbientStage;
        info.hasDiffuseStage |= stage.isDiffuseStage;
        info.hasAmbientBlendStage |= stage.hasAmbientBlendStage;
    }
    return info;
}

RtSmokeTranslucentClassifierInfo BuildSmokeTranslucentClassifierInfo(
    const RtSmokeTranslucentClassifierInput& input,
    const RtSmokeTranslucentClassifierStageInput* stages,
    std::int32_t stageCount);

inline bool RtSmokeTranslucentClassifierInfoEqual(
    const RtSmokeTranslucentClassifierInfo& lhs,
    const RtSmokeTranslucentClassifierInfo& rhs)
{
    return lhs.sortIsGuiOrSubview == rhs.sortIsGuiOrSubview &&
        lhs.sortIsDecal == rhs.sortIsDecal &&
        lhs.sortIsPostProcess == rhs.sortIsPostProcess &&
        lhs.polygonOffsetDecal == rhs.polygonOffsetDecal &&
        lhs.hasScreenTexgen == rhs.hasScreenTexgen &&
        lhs.hasAdditiveBlend == rhs.hasAdditiveBlend &&
        lhs.hasAmbientStage == rhs.hasAmbientStage &&
        lhs.hasAmbientBlendStage == rhs.hasAmbientBlendStage &&
        lhs.hasDiffuseStage == rhs.hasDiffuseStage &&
        lhs.hasAddDefault0200Texture == rhs.hasAddDefault0200Texture &&
        lhs.nameLooksGui == rhs.nameLooksGui &&
        lhs.nameLooksParticle == rhs.nameLooksParticle &&
        lhs.nameLooksDecal == rhs.nameLooksDecal &&
        lhs.nameLooksGlass == rhs.nameLooksGlass &&
        lhs.nameLooksGlow == rhs.nameLooksGlow &&
        lhs.nameLooksSignage == rhs.nameLooksSignage;
}
