#include "precompiled.h"
#pragma hdrstop

#include "PathTraceCleanRtxdiDiMaterialFeatureShaders.h"

namespace {

static const RtPathTraceMaterialFeatureShaderDesc kCleanRtxdiDiMaterialFeatureShaders[] = {
    {
        "clean-room RTXDI DI transmission producer",
        "builtin/pathtracing/cleanroom_rtxdi/pathtrace_clean_rtxdi_di_transmission_producer.rt.bin"
    },
    {
        "clean-room RTXDI DI glass proof",
        "builtin/pathtracing/cleanroom_rtxdi/pathtrace_clean_rtxdi_di_glass.rt.bin"
    }
};

static_assert(
    sizeof(kCleanRtxdiDiMaterialFeatureShaders) / sizeof(kCleanRtxdiDiMaterialFeatureShaders[0]) ==
        static_cast<size_t>(RtPathTraceCleanRtxdiDiMaterialFeatureShaderId::Count),
    "Clean RTXDI DI material feature shader table is out of sync with shader ids");

}

RtPathTraceMaterialFeatureShaderDesc PathTraceCleanRtxdiDiMaterialFeatureShaderDesc(
    RtPathTraceCleanRtxdiDiMaterialFeatureShaderId shaderId)
{
    const size_t index = static_cast<size_t>(shaderId);
    if (index >= sizeof(kCleanRtxdiDiMaterialFeatureShaders) / sizeof(kCleanRtxdiDiMaterialFeatureShaders[0]))
    {
        return RtPathTraceMaterialFeatureShaderDesc();
    }

    return kCleanRtxdiDiMaterialFeatureShaders[index];
}
