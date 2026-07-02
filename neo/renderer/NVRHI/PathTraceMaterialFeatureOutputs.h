#pragma once

// Material feature output helpers.
//
// Frame resources own the textures; this module maps logical feature outputs
// to those textures and applies the corresponding NVRHI resource operations.

#include "PathTraceFrameResources.h"

#include <cstddef>
#include <cstdint>

struct RtPathTraceMaterialFeaturePassDesc;
struct RtPathTraceMaterialFeatureRuntimePass;

struct RtPathTraceMaterialFeatureOutputDesc
{
    uint32_t resource = 0;
    uint32_t uavSlot = 0xffffffffu;
    const char* debugName = "unknown";
    nvrhi::TextureHandle RtPathTraceFrameResources::* textureMember = nullptr;
};

const RtPathTraceMaterialFeatureOutputDesc* FindPathTraceMaterialFeatureOutputDesc(uint32_t resource);
bool PathTraceMaterialFeatureOutputResourceDeclared(uint32_t resource);
bool PathTraceMaterialFeatureOutputResourcesDeclared(uint32_t resources);
nvrhi::TextureHandle PathTraceMaterialFeatureOutputTexture(const RtPathTraceFrameResources& frameResources, uint32_t resource);
bool PathTraceMaterialFeatureOutputAvailable(const RtPathTraceMaterialFeaturePassDesc& passDesc, const RtPathTraceFrameResources& frameResources, uint32_t resource);
bool PathTraceMaterialFeatureOutputsAvailable(const RtPathTraceMaterialFeaturePassDesc& passDesc, const RtPathTraceFrameResources& frameResources);
bool PathTraceMaterialFeaturePrimaryOutputAvailable(const RtPathTraceMaterialFeaturePassDesc& passDesc, const RtPathTraceFrameResources& frameResources);
bool PathTraceMaterialFeaturePrimaryOutputAvailable(const RtPathTraceMaterialFeatureRuntimePass& pass, const RtPathTraceFrameResources& frameResources);
void SetPathTraceMaterialFeatureOutputState(
    nvrhi::ICommandList* commandList,
    const RtPathTraceMaterialFeaturePassDesc& passDesc,
    const RtPathTraceFrameResources& frameResources,
    uint32_t resource,
    nvrhi::ResourceStates state);
void SetPathTraceMaterialFeatureOutputsState(
    nvrhi::ICommandList* commandList,
    const RtPathTraceMaterialFeaturePassDesc& passDesc,
    const RtPathTraceFrameResources& frameResources,
    nvrhi::ResourceStates state);
void SetPathTraceMaterialFeatureOutputsState(
    nvrhi::ICommandList* commandList,
    const RtPathTraceMaterialFeatureRuntimePass& pass,
    const RtPathTraceFrameResources& frameResources,
    nvrhi::ResourceStates state);
void SetPathTraceMaterialFeatureRuntimePassOutputsState(
    nvrhi::ICommandList* commandList,
    const RtPathTraceMaterialFeatureRuntimePass* passes,
    size_t passCount,
    const RtPathTraceFrameResources& frameResources,
    nvrhi::ResourceStates state);
void SetPathTraceMaterialFeaturePrimaryOutputState(
    nvrhi::ICommandList* commandList,
    const RtPathTraceMaterialFeaturePassDesc& passDesc,
    const RtPathTraceFrameResources& frameResources,
    nvrhi::ResourceStates state);
void SetPathTraceMaterialFeaturePrimaryOutputState(
    nvrhi::ICommandList* commandList,
    const RtPathTraceMaterialFeatureRuntimePass& pass,
    const RtPathTraceFrameResources& frameResources,
    nvrhi::ResourceStates state);
void ClearPathTraceMaterialFeatureOutput(
    nvrhi::ICommandList* commandList,
    const RtPathTraceMaterialFeaturePassDesc& passDesc,
    const RtPathTraceFrameResources& frameResources,
    uint32_t resource,
    const nvrhi::Color& color);
void ClearPathTraceMaterialFeaturePrimaryOutput(
    nvrhi::ICommandList* commandList,
    const RtPathTraceMaterialFeaturePassDesc& passDesc,
    const RtPathTraceFrameResources& frameResources,
    const nvrhi::Color& color);
void ClearPathTraceMaterialFeaturePrimaryOutput(
    nvrhi::ICommandList* commandList,
    const RtPathTraceMaterialFeatureRuntimePass& pass,
    const RtPathTraceFrameResources& frameResources,
    const nvrhi::Color& color);
void ClearPathTraceMaterialFeatureRuntimePassPrimaryOutputs(
    nvrhi::ICommandList* commandList,
    const RtPathTraceMaterialFeatureRuntimePass* passes,
    size_t passCount,
    const RtPathTraceFrameResources& frameResources,
    const nvrhi::Color& color);
void BarrierPathTraceMaterialFeatureOutput(
    nvrhi::ICommandList* commandList,
    const RtPathTraceMaterialFeaturePassDesc& passDesc,
    const RtPathTraceFrameResources& frameResources,
    uint32_t resource);
void BarrierPathTraceMaterialFeatureOutputs(
    nvrhi::ICommandList* commandList,
    const RtPathTraceMaterialFeaturePassDesc& passDesc,
    const RtPathTraceFrameResources& frameResources);
void BarrierPathTraceMaterialFeatureOutputs(
    nvrhi::ICommandList* commandList,
    const RtPathTraceMaterialFeatureRuntimePass& pass,
    const RtPathTraceFrameResources& frameResources);
