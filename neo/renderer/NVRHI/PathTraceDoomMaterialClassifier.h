#pragma once

// Doom material classification for the RT smoke/path tracing prototype.
//
// Reduces Doom material declarations and stage properties into coarse material
// traits used by capture, texture discovery, dynamic material table construction,
// and diagnostic dumps.

#include "../Material.h"
#include "PathTraceDoomMaterialClassifierKernel.h"

bool SmokeNameContainsAny(const idStr& name, const char* const* tokens, int tokenCount);
bool SmokeStageBlendUsesSourceAlpha(const shaderStage_t* stage);
bool SmokeStageIsAdditiveBlend(const shaderStage_t* stage);
bool SmokeStageIsFilterBlend(const shaderStage_t* stage, bool& blackKey);
bool SmokeStageIsRenderMap(const shaderStage_t* stage);
const char* SmokeStageAlphaSemanticName(const shaderStage_t* stage);
bool CaptureSmokeTranslucentClassifierInput(
    const idMaterial* material,
    RtSmokeTranslucentClassifierInput& input,
    RtSmokeTranslucentClassifierStageInput* stages,
    int stageCapacity);
RtSmokeTranslucentClassifierInfo BuildSmokeTranslucentClassifierInfo(const idMaterial* material);
void ResolveSmokeMaterialAlphaInfo(const idMaterial* material, bool& hasAlphaTest, float& alphaCutoff);
void ResolveSmokeMaterialAlphaInfo(const idMaterial* material, const RtSmokeTranslucentClassifierInfo& classifier, bool& hasAlphaTest, float& alphaCutoff);
bool IsSmokeAdditiveDecalMaterial(const idMaterial* material);
bool IsSmokeDetailDecalCardMaterial(const idMaterial* material, const RtSmokeTranslucentClassifierInfo& classifier);
bool IsSmokeAdditiveWhiteKeyMaterial(const idMaterial* material, const RtSmokeTranslucentClassifierInfo& classifier);
bool IsSmokeRgbKeyedBlendDecalMaterial(const idMaterial* material, const RtSmokeTranslucentClassifierInfo& classifier);
bool IsSmokeYCoCgDiffuseMapDecalMaterial(const idMaterial* material, const RtSmokeTranslucentClassifierInfo& classifier);
