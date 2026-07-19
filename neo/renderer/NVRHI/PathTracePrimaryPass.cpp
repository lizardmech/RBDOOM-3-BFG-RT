#include "precompiled.h"
#pragma hdrstop

// Frame-level shell for the experimental RT smoke/path tracing path.
//
// Keep this file focused on renderer entry/present flow. The private methods it
// calls are split into PathTraceSmoke* modules so the pass class remains a small
// owner of state rather than the place where every RT detail accumulates.

#include "PathTraceCVars.h"
#include "PathTraceDLSSRRBridge.h"
#include "PathTracePrimaryPass.h"
#include "../RenderCommon.h"
#include "../RenderBackend.h"
#include "../Passes/CommonPasses.h"
#include "../Passes/TonemapPass.h"
#include "../../framework/Common_local.h"
#include "../../sys/DeviceManager.h"

#include <cmath>

extern DeviceManager* deviceManager;

namespace {

const int RT_SMOKE_MIN_OUTPUT_WIDTH = 16;
const int RT_SMOKE_MIN_OUTPUT_HEIGHT = 16;
const int RT_SMOKE_MAX_OUTPUT_WIDTH = 3840;
const int RT_SMOKE_MAX_OUTPUT_HEIGHT = 2160;

void ApplyRestirPTPreviewResolutionCap(int debugMode, int& outputWidth, int& outputHeight)
{
    if (debugMode != 32 && debugMode != 33)
    {
        return;
    }

    const int maxPixels = r_pathTracingRestirPTPreviewMaxPixels.GetInteger();
    if (maxPixels <= 0)
    {
        return;
    }

    const uint64 requestedPixels = static_cast<uint64>(outputWidth) * static_cast<uint64>(outputHeight);
    if (requestedPixels <= static_cast<uint64>(maxPixels))
    {
        return;
    }

    const double scale = std::sqrt(static_cast<double>(maxPixels) / static_cast<double>(requestedPixels));
    int cappedWidth = idMath::ClampInt(
        RT_SMOKE_MIN_OUTPUT_WIDTH,
        outputWidth,
        static_cast<int>(std::floor(static_cast<double>(outputWidth) * scale)));
    int cappedHeight = idMath::ClampInt(
        RT_SMOKE_MIN_OUTPUT_HEIGHT,
        outputHeight,
        static_cast<int>(std::floor(static_cast<double>(outputHeight) * scale)));

    while (static_cast<uint64>(cappedWidth) * static_cast<uint64>(cappedHeight) > static_cast<uint64>(maxPixels) &&
        (cappedWidth > RT_SMOKE_MIN_OUTPUT_WIDTH || cappedHeight > RT_SMOKE_MIN_OUTPUT_HEIGHT))
    {
        if (cappedWidth > RT_SMOKE_MIN_OUTPUT_WIDTH)
        {
            --cappedWidth;
        }
        if (static_cast<uint64>(cappedWidth) * static_cast<uint64>(cappedHeight) <= static_cast<uint64>(maxPixels))
        {
            break;
        }
        if (cappedHeight > RT_SMOKE_MIN_OUTPUT_HEIGHT)
        {
            --cappedHeight;
        }
    }

    static int s_lastLoggedRequestedWidth = 0;
    static int s_lastLoggedRequestedHeight = 0;
    static int s_lastLoggedCappedWidth = 0;
    static int s_lastLoggedCappedHeight = 0;
    if (s_lastLoggedRequestedWidth != outputWidth ||
        s_lastLoggedRequestedHeight != outputHeight ||
        s_lastLoggedCappedWidth != cappedWidth ||
        s_lastLoggedCappedHeight != cappedHeight)
    {
        common->Printf("PathTracePrimaryPass: mode %d ReSTIR PT preview capped output %dx%d -> %dx%d (maxPixels=%d)\n",
            debugMode, outputWidth, outputHeight, cappedWidth, cappedHeight, maxPixels);
        s_lastLoggedRequestedWidth = outputWidth;
        s_lastLoggedRequestedHeight = outputHeight;
        s_lastLoggedCappedWidth = cappedWidth;
        s_lastLoggedCappedHeight = cappedHeight;
    }

    outputWidth = cappedWidth;
    outputHeight = cappedHeight;
}

nvrhi::ObjectType GetPathTraceCommandObjectType()
{
    if (deviceManager && deviceManager->GetGraphicsAPI() == nvrhi::GraphicsAPI::VULKAN)
    {
        return nvrhi::ObjectTypes::VK_CommandBuffer;
    }
    return nvrhi::ObjectTypes::D3D12_GraphicsCommandList;
}

bool ProjectPathTraceOverlayPoint(
    const idVec3& worldPosition,
    const float modelViewMatrix[16],
    const float projectionMatrix[16],
    idVec2& outUv)
{
    idPlane view;
    idPlane clip;
    for (int i = 0; i < 4; ++i)
    {
        view[i] =
            modelViewMatrix[i + 0 * 4] * worldPosition.x +
            modelViewMatrix[i + 1 * 4] * worldPosition.y +
            modelViewMatrix[i + 2 * 4] * worldPosition.z +
            modelViewMatrix[i + 3 * 4];
    }

    for (int i = 0; i < 4; ++i)
    {
        clip[i] =
            projectionMatrix[i + 0 * 4] * view[0] +
            projectionMatrix[i + 1 * 4] * view[1] +
            projectionMatrix[i + 2 * 4] * view[2] +
            projectionMatrix[i + 3 * 4] * view[3];
    }

    if (idMath::Fabs(clip[3]) <= 1.0e-5f)
    {
        return false;
    }

    const float invW = 1.0f / clip[3];
    const float ndcX = clip[0] * invW;
    const float ndcY = clip[1] * invW;
    const float ndcZ = clip[2] * invW;
    if (!std::isfinite(ndcX) || !std::isfinite(ndcY) || !std::isfinite(ndcZ) ||
        idMath::Fabs(ndcX) > 8.0f || idMath::Fabs(ndcY) > 8.0f)
    {
        return false;
    }

    outUv.x = ndcX * 0.5f + 0.5f;
    outUv.y = 0.5f - ndcY * 0.5f;
    return true;
}

void DrawPathTraceOverlayMarker(
    CommonRenderPasses& commonPasses,
    nvrhi::ICommandList* commandList,
    nvrhi::IFramebuffer* targetFramebuffer,
    const nvrhi::Viewport& targetViewport,
    const idVec4& targetBox)
{
    nvrhi::BlendState::RenderTarget blendState;
    blendState.blendEnable = true;
    blendState.setSrcBlend(nvrhi::BlendFactor::One);
    blendState.setDestBlend(nvrhi::BlendFactor::One);
    blendState.setSrcBlendAlpha(nvrhi::BlendFactor::One);
    blendState.setDestBlendAlpha(nvrhi::BlendFactor::One);

    BlitParameters markerBlit;
    markerBlit.targetFramebuffer = targetFramebuffer;
    markerBlit.targetViewport = targetViewport;
    markerBlit.sourceTexture = commonPasses.m_WhiteTexture;
    markerBlit.sampler = BlitSampler::Point;
    markerBlit.blendState = blendState;
    markerBlit.targetBox = targetBox;
    commonPasses.BlitTexture(commandList, markerBlit, nullptr);
}

}

PathTracePrimaryPass::PathTracePrimaryPass(idRenderBackend* backend)
    : m_backend(backend)
    , m_reportedMode(false)
    , m_rayTracingSupported(false)
    , m_smokeTestInitialized(false)
    , m_smokeSceneBuilt(false)
    , m_smokeSceneRebuildLogged(false)
    , m_smokeTestDispatched(false)
    , m_smokeWaitingForDoomSurfaceLogged(false)
    , m_smokeSceneLogCooldownFrames(0)
    , m_smokeStaticBlasCacheValid(false)
    , m_smokeStaticBlasSignature(0)
    , m_smokeStaticBlasCacheHitCount(0)
    , m_smokeStaticBlasCacheMissCount(0)
    , m_smokeGeometryFrameIndex(0)
    , m_smokeSceneSourceLast(-1)
    , m_smokeSceneSource2RigidEntitiesLast(-1)
    , m_smokeSceneUniverseStaticBuildGeneration(0)
    , m_smokeSceneRenderWorld(nullptr)
    , m_smokeSceneMapTimeStamp(0)
    , m_smokeLightUniverseRenderWorld(nullptr)
    , m_smokeTextureProbeMaterialId(0)
    , m_smokeTextureProbeRequestedIndex(-1)
    , m_smokeSceneOrigin(vec3_origin)
{
    nvrhi::IDevice* device = deviceManager ? deviceManager->GetDevice() : nullptr;
    if (device)
    {
        m_rayTracingSupported =
            device->queryFeatureSupport(nvrhi::Feature::RayTracingAccelStruct) &&
            device->queryFeatureSupport(nvrhi::Feature::RayTracingPipeline);
    }

    common->Printf("PathTracePrimaryPass: initialized, ray tracing %s\n",
        m_rayTracingSupported ? "available" : "unavailable");
}

PathTracePrimaryPass::~PathTracePrimaryPass()
{
    ResetRayTracingSmokeSceneResources();
    m_frameResources.ResetOutputSizedResources(RT_FRAME_RESET_SCENE_RESOURCES);
    m_smokeConstantsBuffer = nullptr;
    m_restirPTConstantsBuffer = nullptr;
    m_smokeBoundsOverlayLineBuffer = nullptr;
    m_liquidPoolStatusBuffer = nullptr;
    m_liquidPoolStatusReadbackBuffer = nullptr;
    m_liquidPoolStatusReadbackQueued = false;
    m_liquidPoolStatusReadbackDelayFrames = 0;
    m_smokeCleanRtxdiDiCurrentReservoirBuffer = nullptr;
    m_smokeCleanRtxdiDiTemporalReservoirBuffer = nullptr;
    m_smokeCleanRtxdiDiPreviousReservoirBuffer = nullptr;
    m_smokeCleanRtxdiDiSpatialReservoirBuffer = nullptr;
    m_smokeCleanRtxdiDiCurrentReservoirCount = 0;
    m_smokeCleanRtxdiDiTemporalReservoirCount = 0;
    m_smokeCleanRtxdiDiPreviousReservoirCount = 0;
    m_smokeCleanRtxdiDiSpatialReservoirCount = 0;
    m_smokeCleanRtxdiDiCurrentReservoirBytes = 0;
    m_smokeCleanRtxdiDiTemporalReservoirBytes = 0;
    m_smokeCleanRtxdiDiPreviousReservoirBytes = 0;
    m_smokeCleanRtxdiDiSpatialReservoirBytes = 0;
    m_smokeCleanRtxdiDiFrameIndex = 0;
    m_smokeCleanRtxdiDiPreviousReservoirValid = false;
    m_smokeCleanRtxdiDiHistorySignature = 0;
    m_smokeCleanRtxdiDiHistoryResetCount = 0;
    m_smokeCleanRtxdiDiBlueNoise.Release();
    m_smokeTlas = nullptr;
    m_smokeRestirCombinedShaderTable = nullptr;
    m_smokePrimarySurfaceProducerShaderTable = nullptr;
    m_smokeRestirIndirectInitialProducerShaderTable = nullptr;
    m_smokeRestirDirectTemporalProducerShaderTable = nullptr;
    m_smokeRestirDirectSpatialReservoirProducerShaderTable = nullptr;
    m_smokeRestirCombinedResolveShaderTable = nullptr;
    m_smokeRestirPdfNeeRluCurrentShaderTable = nullptr;
    m_smokePdfNeeVerifierShaderTable = nullptr;
    m_smokeCleanRtxdiDiSentinelShaderTable = nullptr;
    m_smokeCleanRtxdiDiInitialShaderTable = nullptr;
    m_smokeCleanRtxdiDiTemporalShaderTable = nullptr;
    m_smokeCleanRtxdiDiSpatialShaderTable = nullptr;
    m_cleanRestirGiState.ReleaseResources();
    m_smokeReGIRDebugShaderTable = nullptr;
    m_smokeRestirAttributionShaderTable = nullptr;
    m_smokeRestirSpatialShaderTable = nullptr;
    m_smokeRestirSpatialAttributionShaderTable = nullptr;
    m_smokeRestirTemporalShadingShaderTable = nullptr;
    m_smokeRestirInitialShaderTable = nullptr;
    m_smokeRestirShaderTable = nullptr;
    m_smokeShaderTable = nullptr;
    m_smokeSkinnedGpuSkinningBindingSet = nullptr;
    m_smokeCleanRtxdiDiBoilingFilterBindingSet = nullptr;
    m_smokeSkinnedGpuSkinningOutputBuffer = nullptr;
    m_smokeSkinnedGpuSkinningPreviousPositionBuffer = nullptr;
    m_smokeCleanRtxdiDiSentinelConstantsBuffer = nullptr;
    m_smokeMaterialFeatureRuntimeConstantsBuffer = nullptr;
    m_smokeCleanRtxdiDiBoilingFilterConstantsBuffer = nullptr;
    m_smokeCleanRtxdiDiBoilingFilterInputTexture = nullptr;
    m_smokeCleanRtxdiDiBoilingFilterOutputTexture = nullptr;
    m_smokeSkinnedGpuSkinningPipeline = nullptr;
    m_smokeCleanRtxdiDiBoilingFilterPipeline = nullptr;
    m_smokeNeeCachePrimarySurfaceUpdatePipeline = nullptr;
    m_smokeSkinnedGpuSkinningShader = nullptr;
    m_smokeCleanRtxdiDiBoilingFilterShader = nullptr;
    m_smokeNeeCachePrimarySurfaceUpdateShader = nullptr;
    m_smokeRestirCombinedPipeline = nullptr;
    m_smokePrimarySurfaceProducerPipeline = nullptr;
    m_smokeRestirIndirectInitialProducerPipeline = nullptr;
    m_smokeRestirDirectTemporalProducerPipeline = nullptr;
    m_smokeRestirDirectSpatialReservoirProducerPipeline = nullptr;
    m_smokeRestirCombinedResolvePipeline = nullptr;
    m_smokeRestirPdfNeeRluCurrentPipeline = nullptr;
    m_smokePdfNeeVerifierPipeline = nullptr;
    m_smokeCleanRtxdiDiSentinelPipeline = nullptr;
    m_smokeCleanRtxdiDiInitialPipeline = nullptr;
    m_smokeCleanRtxdiDiTemporalPipeline = nullptr;
    m_smokeCleanRtxdiDiSpatialPipeline = nullptr;
    m_smokeReGIRDebugPipeline = nullptr;
    m_smokeRestirAttributionPipeline = nullptr;
    m_smokeRestirSpatialPipeline = nullptr;
    m_smokeRestirSpatialAttributionPipeline = nullptr;
    m_smokeRestirTemporalShadingPipeline = nullptr;
    m_smokeRestirInitialPipeline = nullptr;
    m_smokeRestirPipeline = nullptr;
    m_smokePipeline = nullptr;
    m_smokeTextureDescriptorTable = nullptr;
    m_smokeSkinnedGpuSkinningBindingLayout = nullptr;
    m_smokeCleanRtxdiDiBoilingFilterBindingLayout = nullptr;
    m_smokeNeeCachePrimarySurfaceUpdateBindingLayout = nullptr;
    m_smokeCleanRtxdiDiSentinelBindingLayout = nullptr;
    m_smokeReGIRDebugBindingLayout = nullptr;
    m_smokeBindingLayout = nullptr;
    m_smokeTextureBindlessLayout = nullptr;
    m_smokeRestirCombinedShaderLibrary = nullptr;
    m_smokePrimarySurfaceProducerShaderLibrary = nullptr;
    m_smokeRestirIndirectInitialProducerShaderLibrary = nullptr;
    m_smokeRestirDirectTemporalProducerShaderLibrary = nullptr;
    m_smokeRestirDirectSpatialReservoirProducerShaderLibrary = nullptr;
    m_smokeRestirCombinedResolveShaderLibrary = nullptr;
    m_smokeRestirPdfNeeRluCurrentShaderLibrary = nullptr;
    m_smokePdfNeeVerifierShaderLibrary = nullptr;
    m_smokeCleanRtxdiDiSentinelShaderLibrary = nullptr;
    m_smokeCleanRtxdiDiInitialShaderLibrary = nullptr;
    m_smokeCleanRtxdiDiTemporalShaderLibrary = nullptr;
    m_smokeCleanRtxdiDiSpatialShaderLibrary = nullptr;
    m_smokeReGIRDebugShaderLibrary = nullptr;
    m_smokeRestirAttributionShaderLibrary = nullptr;
    m_smokeRestirSpatialShaderLibrary = nullptr;
    m_smokeRestirSpatialAttributionShaderLibrary = nullptr;
    m_smokeRestirTemporalShadingShaderLibrary = nullptr;
    m_smokeRestirInitialShaderLibrary = nullptr;
    m_smokeRestirShaderLibrary = nullptr;
    m_smokeShaderLibrary = nullptr;
    m_smokeTestInitialized = false;
}

void PathTracePrimaryPass::Execute(const viewDef_t* viewDef)
{
    OPTICK_EVENT("PT Execute");

    const int mode = r_pathTracing.GetInteger();

    if (!m_reportedMode)
    {
        common->Printf("PathTracePrimaryPass: mode %d (%s)\n",
            mode, mode == 2 ? "pure primary rays" : "hybrid");

        if (!m_rayTracingSupported)
        {
            common->Printf("PathTracePrimaryPass: RT device features are not available; restart with r_pathTracing enabled before device creation\n");
        }

        m_reportedMode = true;
    }

    if (!m_rayTracingSupported)
    {
        if (r_pathTracingRestirPdfNeeVerifierDump.GetInteger() != 0)
        {
            common->Printf(
                "PathTracePrimaryPass: PDFNEE verifier execute earlyReturn=rt-unsupported enable=%d view=%d r_pathTracing=%d output=none temporal=0 spatial=0 mode56=0 task=PDFNEE-01\n",
                r_pathTracingRestirPdfNeeVerifierEnable.GetInteger() != 0 ? 1 : 0,
                idMath::ClampInt(0, 8, r_pathTracingRestirPdfNeeVerifierView.GetInteger()),
                r_pathTracing.GetInteger());
            r_pathTracingRestirPdfNeeVerifierDump.SetInteger(0);
        }
        return;
    }

    InitRayTracingSmokeTest();
    int outputWidth = idMath::ClampInt(RT_SMOKE_MIN_OUTPUT_WIDTH, RT_SMOKE_MAX_OUTPUT_WIDTH, r_pathTracingDebugWidth.GetInteger());
    int outputHeight = idMath::ClampInt(RT_SMOKE_MIN_OUTPUT_HEIGHT, RT_SMOKE_MAX_OUTPUT_HEIGHT, r_pathTracingDebugHeight.GetInteger());
    const int debugMode = idMath::ClampInt(0, 57, r_pathTracingDebugMode.GetInteger());
    ApplyRestirPTPreviewResolutionCap(debugMode, outputWidth, outputHeight);
    int renderWidth = outputWidth;
    int renderHeight = outputHeight;
    PathTraceDLSSRRBridge_QueryOptimalRenderSize(outputWidth, outputHeight, renderWidth, renderHeight);
    renderWidth = idMath::ClampInt(RT_SMOKE_MIN_OUTPUT_WIDTH, RT_SMOKE_MAX_OUTPUT_WIDTH, renderWidth);
    renderHeight = idMath::ClampInt(RT_SMOKE_MIN_OUTPUT_HEIGHT, RT_SMOKE_MAX_OUTPUT_HEIGHT, renderHeight);
    m_frameResources.ClearResetReasons();
    m_frameResources.settings.debugMode = debugMode;
    m_frameResources.settings.checkerboardMode = RtRestirPTCheckerboardMode::Off;
    m_frameResources.settings.frameIndex = m_frameResources.restirPTFrameIndex;
    m_frameResources.settings.width = renderWidth;
    m_frameResources.settings.height = renderHeight;
    m_frameResources.settings.outputWidth = outputWidth;
    m_frameResources.settings.outputHeight = outputHeight;
    if (!ResizeRayTracingSmokeOutput(renderWidth, renderHeight, outputWidth, outputHeight))
    {
        if (r_pathTracingRestirPdfNeeVerifierDump.GetInteger() != 0)
        {
            common->Printf(
                "PathTracePrimaryPass: PDFNEE verifier execute earlyReturn=resize-output enable=%d view=%d r_pathTracing=%d requestedRender=%dx%d requestedOutput=%dx%d output=none temporal=0 spatial=0 mode56=0 task=PDFNEE-01\n",
                r_pathTracingRestirPdfNeeVerifierEnable.GetInteger() != 0 ? 1 : 0,
                idMath::ClampInt(0, 8, r_pathTracingRestirPdfNeeVerifierView.GetInteger()),
                r_pathTracing.GetInteger(),
                renderWidth,
                renderHeight,
                outputWidth,
                outputHeight);
            r_pathTracingRestirPdfNeeVerifierDump.SetInteger(0);
        }
        return;
    }

    BuildRayTracingSmokeTestScene(viewDef);
    ExecuteRayTracingSmokeTest(viewDef);
    ReadBackRayTracingSmokeTest();
}

void PathTracePrimaryPass::PresentDebugOutput()
{
    OPTICK_EVENT("PT Present Debug Output");

    if (!deviceManager)
    {
        return;
    }

    nvrhi::IFramebuffer* targetFramebuffer = deviceManager->GetCurrentFramebuffer();
    if (!targetFramebuffer)
    {
        return;
    }

    BlitDebugOutput(targetFramebuffer, nvrhi::Viewport(renderSystem->GetNativeWidth(), renderSystem->GetNativeHeight()));
}

void PathTracePrimaryPass::BlitDebugOutput(nvrhi::IFramebuffer* targetFramebuffer, const nvrhi::Viewport& targetViewport)
{
    OPTICK_EVENT("PT Blit Debug Output");

    if (!m_smokeTestDispatched || !m_frameResources.outputTexture || !m_backend || !targetFramebuffer)
    {
        return;
    }

    nvrhi::ICommandList* commandList = m_backend->GL_GetCommandList();
    if (!commandList)
    {
        return;
    }
    OPTICK_GPU_CONTEXT((void*)commandList->getNativeObject(GetPathTraceCommandObjectType()));

    {
        OPTICK_GPU_EVENT("PT GPU Blit Output Barriers");
        commandList->setTextureState(m_frameResources.outputTexture, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);
        commandList->commitBarriers();
    }

    BlitParameters blitParms;
    blitParms.sourceTexture = m_frameResources.outputTexture;
    blitParms.targetFramebuffer = targetFramebuffer;
    blitParms.targetViewport = targetViewport;
    blitParms.sampler = BlitSampler::Point;
    {
        OPTICK_GPU_EVENT("PT GPU Blit Debug Output");
        m_backend->GetCommonPasses().BlitTexture(commandList, blitParms, nullptr);
    }
}

void PathTracePrimaryPass::TonemapDebugOutput(TonemapPass* tonemapPass, const viewDef_t* viewDef, nvrhi::IFramebuffer* targetFramebuffer)
{
    OPTICK_EVENT("PT Tonemap Debug Output");

    if (!m_smokeTestDispatched || !m_frameResources.outputTexture || !m_backend || !tonemapPass || !viewDef || !targetFramebuffer)
    {
        return;
    }

    nvrhi::ICommandList* commandList = m_backend->GL_GetCommandList();
    if (!commandList)
    {
        return;
    }
    OPTICK_GPU_CONTEXT((void*)commandList->getNativeObject(GetPathTraceCommandObjectType()));

    {
        OPTICK_GPU_EVENT("PT GPU Tonemap Output Barriers");
        commandList->setTextureState(m_frameResources.outputTexture, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);
        commandList->commitBarriers();
    }

    ToneMappingParameters params;
    params.exposureBias = r_pathTracingPostExposure.GetFloat();
    params.minAdaptedLuminance = Max(0.0001f, r_pathTracingPostMinLuminance.GetFloat());
    params.maxAdaptedLuminance = Max(params.minAdaptedLuminance + 0.0001f, r_pathTracingPostMaxLuminance.GetFloat());
    params.whitePoint = Max(0.001f, r_pathTracingPostWhitePoint.GetFloat());
    params.contrast = Max(0.0f, r_pathTracingPostContrast.GetFloat());
    params.saturation = Max(0.0f, r_pathTracingPostSaturation.GetFloat());
    params.enableACES = r_pathTracingPostACES.GetInteger() != 0;
    params.enableColorLUT = false;
    params.colorLUTDebugMode = static_cast<uint>(Max(0, r_pathTracingPostLUTDebug.GetInteger()));
    params.useGlobalExposureSettings = false;

    const char* lutName = r_pathTracingPostLUTImage.GetString();
    if (r_pathTracingPostLUT.GetInteger() != 0 && lutName && lutName[0] != '\0')
    {
        auto uploadPathTracePostLut = [&](const char* loadName) -> bool
        {
            byte* lutPixels = nullptr;
            int lutWidth = 0;
            int lutHeight = 0;
            ID_TIME_T lutTimestamp = FILE_NOT_FOUND_TIMESTAMP;
            LoadSTB_RGBA8(loadName, &lutPixels, &lutWidth, &lutHeight, &lutTimestamp);
            if (!lutPixels || lutWidth <= 0 || lutHeight <= 0)
            {
                if (lutPixels)
                {
                    R_StaticFree(lutPixels);
                }
                m_pathTracePostLutTexture = nullptr;
                m_pathTracePostLutWidth = 0;
                m_pathTracePostLutHeight = 0;
                return false;
            }

            if (lutWidth != lutHeight * lutHeight)
            {
                R_StaticFree(lutPixels);
                m_pathTracePostLutTexture = nullptr;
                m_pathTracePostLutWidth = lutWidth;
                m_pathTracePostLutHeight = lutHeight;
                return false;
            }

            nvrhi::IDevice* device = deviceManager ? deviceManager->GetDevice() : nullptr;
            if (!device)
            {
                R_StaticFree(lutPixels);
                m_pathTracePostLutTexture = nullptr;
                m_pathTracePostLutWidth = 0;
                m_pathTracePostLutHeight = 0;
                return false;
            }

            const bool createLutTexture = !m_pathTracePostLutTexture ||
                m_pathTracePostLutTexture->getDesc().width != static_cast<uint32_t>(lutWidth) ||
                m_pathTracePostLutTexture->getDesc().height != static_cast<uint32_t>(lutHeight);
            if (createLutTexture)
            {
                nvrhi::TextureDesc lutTextureDesc;
                lutTextureDesc.width = static_cast<uint32_t>(lutWidth);
                lutTextureDesc.height = static_cast<uint32_t>(lutHeight);
                lutTextureDesc.mipLevels = 1;
                lutTextureDesc.format = nvrhi::Format::RGBA8_UNORM;
                lutTextureDesc.debugName = "_pathTracePostLUTTexture";
                lutTextureDesc.initialState = nvrhi::ResourceStates::ShaderResource;
                lutTextureDesc.keepInitialState = true;
                m_pathTracePostLutTexture = device->createTexture(lutTextureDesc);
                if (!m_pathTracePostLutTexture)
                {
                    R_StaticFree(lutPixels);
                    m_pathTracePostLutWidth = 0;
                    m_pathTracePostLutHeight = 0;
                    return false;
                }
            }

            commandList->setTextureState(m_pathTracePostLutTexture, nvrhi::AllSubresources, nvrhi::ResourceStates::CopyDest);
            commandList->commitBarriers();
            commandList->writeTexture(m_pathTracePostLutTexture, 0, 0, lutPixels, static_cast<size_t>(lutWidth) * 4u);
            commandList->setTextureState(m_pathTracePostLutTexture, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);
            commandList->commitBarriers();
            R_StaticFree(lutPixels);
            m_pathTracePostLutWidth = lutWidth;
            m_pathTracePostLutHeight = lutHeight;
            return m_pathTracePostLutTexture != nullptr;
        };

        if (m_pathTracePostLutName.Icmp(lutName) != 0 || !m_pathTracePostLutTexture || params.colorLUTDebugMode != 0)
        {
            m_pathTracePostLutName = lutName;
            if (!uploadPathTracePostLut(lutName) && !m_pathTracePostLutInvalidLogged)
            {
                m_pathTracePostLutInvalidLogged = true;
                common->Printf("PathTracePrimaryPass: PT post LUT '%s' failed to load\n",
                    m_pathTracePostLutName.c_str());
            }
            else
            {
                m_pathTracePostLutInvalidLogged = false;
            }
        }
        if (r_pathTracingPostLUTReload.GetInteger() != 0)
        {
            if (!uploadPathTracePostLut(m_pathTracePostLutName.c_str()) && !m_pathTracePostLutInvalidLogged)
            {
                m_pathTracePostLutInvalidLogged = true;
                common->Printf("PathTracePrimaryPass: PT post LUT '%s' failed to reload\n",
                    m_pathTracePostLutName.c_str());
            }
            else
            {
                m_pathTracePostLutInvalidLogged = false;
            }
            r_pathTracingPostLUTReload.SetInteger(0);
        }

        if (m_pathTracePostLutTexture)
        {
            const int lutWidth = m_pathTracePostLutWidth;
            const int lutHeight = m_pathTracePostLutHeight;
            if (lutHeight > 0 && lutWidth == lutHeight * lutHeight)
            {
                params.enableColorLUT = true;
                params.colorLUTUseOverride = 1;
                params.colorLUTTextureOverrideSize = lutHeight;
                params.colorLUTTextureOverride = m_pathTracePostLutTexture;
            }
            else if (!m_pathTracePostLutInvalidLogged)
            {
                m_pathTracePostLutInvalidLogged = true;
                common->Printf("PathTracePrimaryPass: PT post LUT '%s' ignored, expected width=height*height but got %dx%d\n",
                    m_pathTracePostLutName.c_str(),
                    lutWidth,
                    lutHeight);
            }
        }
        else if (!m_pathTracePostLutInvalidLogged)
        {
            m_pathTracePostLutInvalidLogged = true;
            common->Printf("PathTracePrimaryPass: PT post LUT '%s' failed to load\n",
                m_pathTracePostLutName.c_str());
        }
    }

    tonemapPass->SimpleRender(commandList, params, viewDef, m_frameResources.outputTexture, targetFramebuffer);
}

void PathTracePrimaryPass::DrawBoundsOverlayRaster(nvrhi::IFramebuffer* targetFramebuffer, const nvrhi::Viewport& targetViewport)
{
    if (r_pathTracingSceneBoundsOverlay.GetInteger() == 0 ||
        r_pathTracingSceneBoundsOverlayGpu.GetInteger() != 0 ||
        !m_backend ||
        !targetFramebuffer)
    {
        return;
    }

    nvrhi::ICommandList* commandList = m_backend->GL_GetCommandList();
    if (!commandList)
    {
        return;
    }

    CommonRenderPasses& commonPasses = m_backend->GetCommonPasses();
    if (!commonPasses.m_WhiteTexture)
    {
        return;
    }

    const float viewportWidth = Max(targetViewport.width(), 1.0f);
    const float viewportHeight = Max(targetViewport.height(), 1.0f);

    DrawPathTraceOverlayMarker(
        commonPasses,
        commandList,
        targetFramebuffer,
        targetViewport,
        idVec4(16.0f / viewportWidth, 16.0f / viewportHeight, 32.0f / viewportWidth, 32.0f / viewportHeight));

    if (!m_smokeBoundsOverlayViewValid ||
        m_smokeBoundsOverlayLineCount <= 0 ||
        m_smokeBoundsOverlayLines.empty())
    {
        return;
    }

    const float markerPixels = 3.0f;
    const float markerStepPixels = 7.0f;
    const int maxMarkerBlits = 2048;
    int markerBlits = 0;
    int projectedLines = 0;
    const int lineCount = idMath::ClampInt(0, Min(m_smokeBoundsOverlayLineCount, static_cast<int>(m_smokeBoundsOverlayLines.size())), RT_PT_BOUNDS_OVERLAY_MAX_LINES);
    for (int lineIndex = 0; lineIndex < lineCount; ++lineIndex)
    {
        if (markerBlits >= maxMarkerBlits)
        {
            break;
        }

        const RtPathTraceBoundsOverlayLine& line = m_smokeBoundsOverlayLines[lineIndex];
        idVec2 startUv;
        idVec2 endUv;
        if (!ProjectPathTraceOverlayPoint(line.startAndPad.ToVec3(), m_smokeBoundsOverlayModelViewMatrix, m_smokeBoundsOverlayProjectionMatrix, startUv) ||
            !ProjectPathTraceOverlayPoint(line.endAndPad.ToVec3(), m_smokeBoundsOverlayModelViewMatrix, m_smokeBoundsOverlayProjectionMatrix, endUv))
        {
            continue;
        }
        ++projectedLines;

        const idVec2 startPixel(startUv.x * viewportWidth, startUv.y * viewportHeight);
        const idVec2 endPixel(endUv.x * viewportWidth, endUv.y * viewportHeight);
        const idVec2 delta = endPixel - startPixel;
        const float length = delta.LengthFast();
        if (length <= 1.0f)
        {
            continue;
        }

        const int markerCount = idMath::ClampInt(2, 32, static_cast<int>(length / markerStepPixels) + 1);
        for (int markerIndex = 0; markerIndex < markerCount && markerBlits < maxMarkerBlits; ++markerIndex)
        {
            const float t = markerCount > 1 ? static_cast<float>(markerIndex) / static_cast<float>(markerCount - 1) : 0.0f;
            const idVec2 markerCenter = startPixel + delta * t;
            const float minX = markerCenter.x - markerPixels * 0.5f;
            const float minY = markerCenter.y - markerPixels * 0.5f;
            if (minX > viewportWidth || minY > viewportHeight || minX + markerPixels < 0.0f || minY + markerPixels < 0.0f)
            {
                continue;
            }

            idVec4 markerBox;
            markerBox.Set(
                idMath::ClampFloat(0.0f, 1.0f, minX / viewportWidth),
                idMath::ClampFloat(0.0f, 1.0f, minY / viewportHeight),
                markerPixels / viewportWidth,
                markerPixels / viewportHeight);
            DrawPathTraceOverlayMarker(commonPasses, commandList, targetFramebuffer, targetViewport, markerBox);
            ++markerBlits;
        }
    }

    static int lastOverlayDiagnosticMs = 0;
    const int nowMs = Sys_Milliseconds();
    if (nowMs - lastOverlayDiagnosticMs > 1000)
    {
        lastOverlayDiagnosticMs = nowMs;
        common->Printf("PathTracePrimaryPass: bounds overlay raster lines=%d projected=%d markers=%d\n", lineCount, projectedLines, markerBlits);
    }
}
