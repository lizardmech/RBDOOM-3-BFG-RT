#include "precompiled.h"
#pragma hdrstop

#include "PathTraceDoomMaterialClassifier.h"
#include "../Image.h"

bool SmokeNameContainsAny(const idStr& name, const char* const* tokens, int tokenCount)
{
    for (int tokenIndex = 0; tokenIndex < tokenCount; ++tokenIndex)
    {
        if (name.Find(tokens[tokenIndex], false) >= 0)
        {
            return true;
        }
    }
    return false;
}

bool SmokeStageBlendUsesSourceAlpha(const shaderStage_t* stage)
{
    if (!stage)
    {
        return false;
    }

    const uint64 srcBlend = stage->drawStateBits & GLS_SRCBLEND_BITS;
    const uint64 dstBlend = stage->drawStateBits & GLS_DSTBLEND_BITS;
    return srcBlend == GLS_SRCBLEND_SRC_ALPHA ||
        srcBlend == GLS_SRCBLEND_ONE_MINUS_SRC_ALPHA ||
        dstBlend == GLS_DSTBLEND_SRC_ALPHA ||
        dstBlend == GLS_DSTBLEND_ONE_MINUS_SRC_ALPHA;
}

bool SmokeStageIsAdditiveBlend(const shaderStage_t* stage)
{
    if (!stage)
    {
        return false;
    }

    const uint64 srcBlend = stage->drawStateBits & GLS_SRCBLEND_BITS;
    const uint64 dstBlend = stage->drawStateBits & GLS_DSTBLEND_BITS;
    return (srcBlend == GLS_SRCBLEND_ONE || srcBlend == GLS_SRCBLEND_SRC_ALPHA) && dstBlend == GLS_DSTBLEND_ONE;
}

bool SmokeNameHasDefault0200Code(const idStr& name)
{
    const int length = name.Length();
    for (int start = Max(0, length - 16); start <= length - 4; ++start)
    {
        if (idStr::Cmpn(name.c_str() + start, "0200", 4) != 0)
        {
            continue;
        }

        const bool boundedBefore =
            start == 0 ||
            name[start - 1] == '_' ||
            name[start - 1] == '-' ||
            name[start - 1] == '/' ||
            name[start - 1] == '.' ||
            name[start - 1] == '#';
        const bool boundedAfter =
            start + 4 >= length ||
            name[start + 4] == '_' ||
            name[start + 4] == '-' ||
            name[start + 4] == '.' ||
            name[start + 4] == '/';
        if (boundedBefore && boundedAfter)
        {
            return true;
        }
    }
    return false;
}

bool SmokeNameLooksAddDefault0200Texture(const char* imageName)
{
    if (!imageName || !imageName[0])
    {
        return false;
    }

    idStr name = imageName;
    name.BackSlashesToSlashes();
    return name.Find("_add", false) >= 0 && SmokeNameHasDefault0200Code(name);
}

bool SmokeStageIsFilterBlend(const shaderStage_t* stage, bool& blackKey)
{
    blackKey = false;
    if (!stage)
    {
        return false;
    }

    const uint64 srcBlend = stage->drawStateBits & GLS_SRCBLEND_BITS;
    const uint64 dstBlend = stage->drawStateBits & GLS_DSTBLEND_BITS;
    if (srcBlend == GLS_SRCBLEND_DST_COLOR && dstBlend == GLS_DSTBLEND_ZERO)
    {
        blackKey = false;
        return true;
    }
    if (srcBlend == GLS_SRCBLEND_ZERO && dstBlend == GLS_DSTBLEND_SRC_COLOR)
    {
        blackKey = false;
        return true;
    }
    if (srcBlend == GLS_SRCBLEND_ZERO && dstBlend == GLS_DSTBLEND_ONE_MINUS_SRC_COLOR)
    {
        blackKey = true;
        return true;
    }
    if (srcBlend == GLS_SRCBLEND_ONE_MINUS_DST_COLOR && dstBlend == GLS_DSTBLEND_ONE)
    {
        blackKey = true;
        return true;
    }

    return false;
}

bool SmokeStageIsRenderMap(const shaderStage_t* stage)
{
    if (!stage)
    {
        return false;
    }

    return stage->texture.dynamic == DI_GUI_RENDER ||
        stage->texture.dynamic == DI_RENDER_TARGET ||
        stage->texture.dynamic == DI_MIRROR_RENDER ||
        stage->texture.dynamic == DI_REMOTE_RENDER ||
        stage->texture.dynamic == DI_XRAY_RENDER;
}

const char* SmokeStageAlphaSemanticName(const shaderStage_t* stage)
{
    if (!stage || !stage->texture.image)
    {
        return "none";
    }

    const idImage* image = stage->texture.image;
    const textureUsage_t usage = image->GetUsage();
    const textureColor_t colorFormat = image->GetOpts().colorFormat;
    if (usage == TD_COVERAGE || colorFormat == CFM_GREEN_ALPHA || stage->lighting == SL_COVERAGE)
    {
        return "coverage/cutout";
    }
    if (colorFormat == CFM_YCOCG_DXT5)
    {
        return "YCoCg-color-reconstruct";
    }
    if (colorFormat == CFM_NORMAL_DXT5 || usage == TD_BUMP)
    {
        return "normal-packed";
    }
    if (usage == TD_SPECULAR)
    {
        return "specular-no-alpha";
    }
    if (colorFormat == CFM_DEFAULT && (stage->hasAlphaTest || SmokeStageBlendUsesSourceAlpha(stage)))
    {
        return "material-alpha";
    }
    if (colorFormat == CFM_DEFAULT)
    {
        return "rgba-unused-alpha";
    }

    return "unknown";
}

RtSmokeTranslucentClassifierInfo BuildSmokeTranslucentClassifierInfo(const idMaterial* material)
{
    RtSmokeTranslucentClassifierInfo info;
    if (!material)
    {
        return info;
    }

    idStr materialName = material->GetName();
    const float sort = material->GetSort();
    info.sortIsGuiOrSubview = sort <= SS_GUI;
    info.sortIsDecal = sort >= SS_DECAL && sort < SS_FAR;
    info.sortIsPostProcess = sort >= SS_POST_PROCESS;
    info.polygonOffsetDecal = material->TestMaterialFlag(MF_POLYGONOFFSET);

    static const char* guiTokens[] = { "gui", "guis/", "video", "cinematic", "terminal", "console", "pda", "cursor" };
    static const char* particleTokens[] = { "particle", "smoke", "dust", "steam", "fog", "muzzle", "spark", "bloodcloud" };
    static const char* decalTokens[] = { "decal", "stain", "grime", "dirt", "scorch", "burn", "bullet", "mud", "blood", "splat", "mark" };
    static const char* glassTokens[] = { "glass", "window", "visor", "transparent" };
    static const char* glowTokens[] = { "glow", "light", "lamp", "beam", "flare", "strip", "striplight", "tube", "neon", "emissive", "emit", "bulb", "fluoro", "flouro" };
    static const char* signageTokens[] = { "logo", "sign", "label", "snack", "soda", "cola", "add", "screen", "monitor" };
    info.nameLooksGui = SmokeNameContainsAny(materialName, guiTokens, sizeof(guiTokens) / sizeof(guiTokens[0]));
    info.nameLooksParticle = SmokeNameContainsAny(materialName, particleTokens, sizeof(particleTokens) / sizeof(particleTokens[0]));
    info.nameLooksDecal = SmokeNameContainsAny(materialName, decalTokens, sizeof(decalTokens) / sizeof(decalTokens[0]));
    info.nameLooksGlass = SmokeNameContainsAny(materialName, glassTokens, sizeof(glassTokens) / sizeof(glassTokens[0]));
    info.nameLooksGlow = SmokeNameContainsAny(materialName, glowTokens, sizeof(glowTokens) / sizeof(glowTokens[0]));
    info.nameLooksSignage = SmokeNameContainsAny(materialName, signageTokens, sizeof(signageTokens) / sizeof(signageTokens[0]));

    for (int stageIndex = 0; stageIndex < material->GetNumStages(); ++stageIndex)
    {
        const shaderStage_t* stage = material->GetStage(stageIndex);
        if (!stage)
        {
            continue;
        }

        if (stage->texture.texgen == TG_SCREEN || stage->texture.texgen == TG_SCREEN2 || SmokeStageIsRenderMap(stage))
        {
            info.hasScreenTexgen = true;
        }
        if (stage->texture.image && SmokeNameLooksAddDefault0200Texture(stage->texture.image->GetName()))
        {
            info.hasAddDefault0200Texture = true;
        }
        if (stage->lighting == SL_AMBIENT)
        {
            info.hasAmbientStage = true;
        }
        else if (stage->lighting == SL_DIFFUSE)
        {
            info.hasDiffuseStage = true;
        }

        const uint64 srcBlend = stage->drawStateBits & GLS_SRCBLEND_BITS;
        const uint64 dstBlend = stage->drawStateBits & GLS_DSTBLEND_BITS;
        if (SmokeStageIsAdditiveBlend(stage))
        {
            info.hasAdditiveBlend = true;
        }
        if (stage->lighting == SL_AMBIENT && (dstBlend != GLS_DSTBLEND_ZERO || srcBlend == GLS_SRCBLEND_DST_COLOR || srcBlend == GLS_SRCBLEND_ONE_MINUS_DST_COLOR))
        {
            info.hasAmbientBlendStage = true;
        }
    }

    return info;
}

bool IsSmokeDetailDecalCardMaterial(const idMaterial* material, const RtSmokeTranslucentClassifierInfo& classifier)
{
    if (!material)
    {
        return false;
    }
    // Detail decals are surface-locked coplanar cards: level-authored grime, trim,
    // panels, signage, scorch sitting on a wall/floor. Effect cards are excluded by
    // orientation/deform/particle signals ONLY -- a dynamic or additive surface-locked
    // decal is still a detail decal (docs/decal_cards 02 M1 / 07).
    if (material->Deform() != DFRM_NONE)
    {
        return false;
    }
    if (classifier.hasScreenTexgen || classifier.sortIsGuiOrSubview || classifier.sortIsPostProcess)
    {
        return false;
    }
    if (classifier.nameLooksParticle || classifier.nameLooksGui)
    {
        return false;
    }
    return classifier.sortIsDecal || classifier.polygonOffsetDecal;
}

void ResolveSmokeMaterialAlphaInfo(const idMaterial* material, bool& hasAlphaTest, float& alphaCutoff)
{
    hasAlphaTest = false;
    alphaCutoff = 0.0f;
    if (!material)
    {
        return;
    }

    const RtSmokeTranslucentClassifierInfo classifier = BuildSmokeTranslucentClassifierInfo(material);
    ResolveSmokeMaterialAlphaInfo(material, classifier, hasAlphaTest, alphaCutoff);
}

void ResolveSmokeMaterialAlphaInfo(const idMaterial* material, const RtSmokeTranslucentClassifierInfo& classifier, bool& hasAlphaTest, float& alphaCutoff)
{
    hasAlphaTest = false;
    alphaCutoff = 0.0f;
    if (!material)
    {
        return;
    }

    const float* constantRegisters = material->ConstantRegisters();
    const int registerCount = material->GetNumRegisters();
    for (int stageIndex = 0; stageIndex < material->GetNumStages(); ++stageIndex)
    {
        const shaderStage_t* stage = material->GetStage(stageIndex);
        if (!stage || !stage->hasAlphaTest || stage->ignoreAlphaTest)
        {
            continue;
        }

        hasAlphaTest = true;
        alphaCutoff = 0.5f;
        if (constantRegisters && stage->alphaTestRegister >= 0 && stage->alphaTestRegister < registerCount)
        {
            alphaCutoff = idMath::ClampFloat(0.0f, 1.0f, constantRegisters[stage->alphaTestRegister]);
        }
        return;
    }

    // Coverage is a derived material-wide summary. An explicit stage alphatest is
    // authoritative regardless of whether the declaration ended up classified as
    // perforated or translucent, and must not depend on material-name categories.
    (void)classifier;
    if (material->Coverage() == MC_PERFORATED)
    {
        hasAlphaTest = true;
        alphaCutoff = 0.5f;
    }
}

bool IsSmokeAdditiveDecalMaterial(const idMaterial* material)
{
    if (!material)
    {
        return false;
    }

    const RtSmokeTranslucentClassifierInfo info = BuildSmokeTranslucentClassifierInfo(material);
    if (!info.hasAdditiveBlend)
    {
        return false;
    }

    if (info.hasScreenTexgen || info.hasAddDefault0200Texture || info.nameLooksGui || info.nameLooksParticle || info.nameLooksGlass)
    {
        return false;
    }

    // An opaque interaction surface may carry an ambient additive stage as its
    // authored emissive layer (for example striplightxl1).  That does not make
    // the surface a transparent additive card: its diffuse interaction still
    // owns coverage and must terminate visibility/continuation rays.  Keep
    // explicitly sorted or polygon-offset decals eligible because those really
    // are receiver-modifying layers even when the declaration retained opaque
    // coverage.
    if (material->Coverage() == MC_OPAQUE &&
        info.hasDiffuseStage &&
        !info.sortIsDecal &&
        !info.polygonOffsetDecal)
    {
        return false;
    }

    return material->Coverage() == MC_TRANSLUCENT ||
        info.hasAmbientStage ||
        info.sortIsDecal ||
        info.polygonOffsetDecal ||
        info.nameLooksDecal ||
        info.nameLooksSignage ||
        info.nameLooksGlow;
}

bool IsSmokeAdditiveWhiteKeyMaterial(const idMaterial* material, const RtSmokeTranslucentClassifierInfo& classifier)
{
    if (!material || material->Coverage() != MC_TRANSLUCENT || !classifier.hasAdditiveBlend)
    {
        return false;
    }
    if (classifier.hasScreenTexgen || classifier.hasAddDefault0200Texture || classifier.nameLooksGui || classifier.nameLooksParticle || classifier.nameLooksGlass)
    {
        return false;
    }

    for (int stageIndex = 0; stageIndex < material->GetNumStages(); ++stageIndex)
    {
        const shaderStage_t* stage = material->GetStage(stageIndex);
        if (!stage || !SmokeStageIsAdditiveBlend(stage) || !stage->texture.image)
        {
            continue;
        }

        const idImage* image = stage->texture.image;
        if (image->GetUsage() == TD_DEFAULT && image->GetOpts().colorFormat == CFM_DEFAULT && (classifier.nameLooksGlow || classifier.nameLooksSignage))
        {
            return true;
        }
    }

    return false;
}

bool IsSmokeRgbKeyedBlendDecalMaterial(const idMaterial* material, const RtSmokeTranslucentClassifierInfo& classifier)
{
    if (!material || material->Coverage() != MC_TRANSLUCENT)
    {
        return false;
    }
    if (classifier.hasScreenTexgen || classifier.hasAddDefault0200Texture || classifier.nameLooksGui || classifier.nameLooksParticle || classifier.nameLooksGlass)
    {
        return false;
    }
    if (!classifier.sortIsDecal && !classifier.polygonOffsetDecal && !classifier.nameLooksDecal)
    {
        return false;
    }

    for (int stageIndex = 0; stageIndex < material->GetNumStages(); ++stageIndex)
    {
        const shaderStage_t* stage = material->GetStage(stageIndex);
        if (!stage || (stage->lighting != SL_AMBIENT && stage->lighting != SL_DIFFUSE) || !stage->texture.image)
        {
            continue;
        }

        const idImage* image = stage->texture.image;
        if (image->GetUsage() == TD_DIFFUSE || image->GetOpts().colorFormat == CFM_YCOCG_DXT5)
        {
            return true;
        }
    }

    return false;
}

bool IsSmokeYCoCgDiffuseMapDecalMaterial(const idMaterial* material, const RtSmokeTranslucentClassifierInfo& classifier)
{
    if (!IsSmokeRgbKeyedBlendDecalMaterial(material, classifier))
    {
        return false;
    }

    for (int stageIndex = 0; stageIndex < material->GetNumStages(); ++stageIndex)
    {
        const shaderStage_t* stage = material->GetStage(stageIndex);
        if (!stage || stage->lighting != SL_DIFFUSE || !stage->texture.image)
        {
            continue;
        }

        const idImage* image = stage->texture.image;
        if (image->GetUsage() == TD_DIFFUSE || image->GetOpts().colorFormat == CFM_YCOCG_DXT5)
        {
            return true;
        }
    }

    return false;
}
