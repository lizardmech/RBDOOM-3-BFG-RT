#include "precompiled.h"
#pragma hdrstop

#include "PathTraceDoomMaterialClassifier.h"
#include "PathTraceMaterialClassifierNameCache.h"
#include "PathTraceOwnerSemanticKernel.h"
#include "../Image.h"

static_assert(
    RT_SMOKE_TRANSLUCENT_CLASSIFIER_NAME_CAPACITY == MAX_STRING_CHARS,
    "classifier capture bound must match the renderer material-name bound");

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

static bool SmokeDrawStateIsAdditiveBlend(uint64 drawStateBits)
{
    const uint64 srcBlend = drawStateBits & GLS_SRCBLEND_BITS;
    const uint64 dstBlend = drawStateBits & GLS_DSTBLEND_BITS;
    return (srcBlend == GLS_SRCBLEND_ONE || srcBlend == GLS_SRCBLEND_SRC_ALPHA) && dstBlend == GLS_DSTBLEND_ONE;
}

bool SmokeStageIsAdditiveBlend(const shaderStage_t* stage)
{
    if (!stage)
    {
        return false;
    }

    return SmokeDrawStateIsAdditiveBlend(stage->drawStateBits);
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

static RtSmokeTranslucentClassifierStageInput
CaptureSmokeTranslucentClassifierStageInput(const shaderStage_t* stage);

bool CaptureSmokeTranslucentClassifierInput(
    const idMaterial* material,
    RtSmokeTranslucentClassifierInput& input,
    RtSmokeTranslucentClassifierStageInput* stages,
    int stageCapacity)
{
    input = RtSmokeTranslucentClassifierInput();
    if (!material)
    {
        return true;
    }

    const char* materialName = material->GetName();
    const size_t materialNameLength = materialName ? strlen(materialName) : 0;
    input.materialPresent = true;
    input.sort = material->GetSort();
    input.sortIsGuiOrSubview = input.sort <= SS_GUI;
    input.sortIsDecal = input.sort >= SS_DECAL && input.sort < SS_FAR;
    input.sortIsPostProcess = input.sort >= SS_POST_PROCESS;
    input.polygonOffsetDecal = material->TestMaterialFlag(MF_POLYGONOFFSET);
    input.stageCount = material->GetNumStages();
    if (!RtSmokeTranslucentClassifierCaptureFits(
            materialNameLength, input.stageCount, stageCapacity) ||
        (input.stageCount > 0 && !stages))
    {
        return false;
    }
    if (materialNameLength > 0)
    {
        memcpy(input.materialName, materialName, materialNameLength);
    }
    input.materialName[materialNameLength] = '\0';

    for (int stageIndex = 0; stageIndex < input.stageCount; ++stageIndex)
    {
        stages[stageIndex] = CaptureSmokeTranslucentClassifierStageInput(
            material->GetStage(stageIndex));
    }
    return true;
}

static RtSmokeTranslucentClassifierStageInput
CaptureSmokeTranslucentClassifierStageInput(const shaderStage_t* stage)
{
    RtSmokeTranslucentClassifierStageInput destination;
    if (!stage)
    {
        return destination;
    }
    destination.valid = true;
    destination.lighting = static_cast<int>(stage->lighting);
    destination.texgen = static_cast<int>(stage->texture.texgen);
    destination.drawStateBits = stage->drawStateBits;
    destination.conditionRegister = stage->conditionRegister;
    destination.isRenderMap = SmokeStageIsRenderMap(stage);
    destination.hasImage = stage->texture.image != nullptr;
    destination.looksAddDefault0200 =
        destination.hasImage &&
        SmokeNameLooksAddDefault0200Texture(stage->texture.image->GetName());
    destination.hasScreenTexgen =
        stage->texture.texgen == TG_SCREEN ||
        stage->texture.texgen == TG_SCREEN2 ||
        destination.isRenderMap;
    destination.isAdditiveBlend =
        SmokeDrawStateIsAdditiveBlend(stage->drawStateBits);
    destination.isAmbientStage = stage->lighting == SL_AMBIENT;
    destination.isDiffuseStage = stage->lighting == SL_DIFFUSE;
    const uint64 srcBlend = stage->drawStateBits & GLS_SRCBLEND_BITS;
    const uint64 dstBlend = stage->drawStateBits & GLS_DSTBLEND_BITS;
    destination.hasAmbientBlendStage =
        destination.isAmbientStage &&
        (dstBlend != GLS_DSTBLEND_ZERO ||
         srcBlend == GLS_SRCBLEND_DST_COLOR ||
         srcBlend == GLS_SRCBLEND_ONE_MINUS_DST_COLOR);
    return destination;
}

RtSmokeTranslucentClassifierInfo BuildSmokeTranslucentClassifierInfo(const idMaterial* material)
{
    OPTICK_EVENT("PT Material Translucent Classifier Build");
    if (!material)
    {
        return RtSmokeTranslucentClassifierInfo();
    }

    RtSmokeTranslucentClassifierInput input;
    input.materialPresent = true;
    input.sort = material->GetSort();
    input.sortIsGuiOrSubview = input.sort <= SS_GUI;
    input.sortIsDecal = input.sort >= SS_DECAL && input.sort < SS_FAR;
    input.sortIsPostProcess = input.sort >= SS_POST_PROCESS;
    input.polygonOffsetDecal = material->TestMaterialFlag(MF_POLYGONOFFSET);
    input.stageCount = material->GetNumStages();
    const RtSmokeClassifierNameInfo names = SmokeThreadClassifierNameCache().Get(
        material->GetName(), [](const char* name) {
            OPTICK_EVENT("PT Material Classifier Name Build");
            return BuildSmokeClassifierNameInfo(name);
        });
    return BuildSmokeTranslucentClassifierInfoFromSource(
        input,
        material->GetName(),
        input.stageCount,
        [material](int stageIndex) {
            return CaptureSmokeTranslucentClassifierStageInput(
                material->GetStage(stageIndex));
        }, &names);
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
