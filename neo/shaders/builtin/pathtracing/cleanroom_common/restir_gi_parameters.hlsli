#ifndef RB_PATH_TRACING_CLEANROOM_COMMON_RESTIR_GI_PARAMETERS_HLSLI
#define RB_PATH_TRACING_CLEANROOM_COMMON_RESTIR_GI_PARAMETERS_HLSLI

#include "cleanroom_common/restir_shared_parameters.hlsli"

// rbdoom-owned GI parameter structs, replacing Rtxdi/GI/ReSTIRGIParameters.h.
// Field set inferred from this repo's call sites only
// (restir_gi_temporal_reuse.rt.hlsl, pathtrace_clean_restir_gi.rt.hlsl).
//
struct RTXDI_GITemporalResamplingParameters
{
    float depthThreshold;
    float normalThreshold;
    uint maxHistoryLength;
    uint maxReservoirAge;
    uint biasCorrectionMode;
    uint enableFallbackSampling;
    uint enablePermutationSampling;
    uint uniformRandomNumber;
    uint enableDlssRrCompatibility;
    float dlssRrTemporalRandomizationRadius;
    float dlssRrDiffuseProbability;
};

// Field set matches the staged rbdoom GI spatial replacement
// (docs/rtxdi_license_remediation/replacements/GISpatialResampling.hlsli).
// TASK 9 NOTE: that staged file defines this struct itself; when wiring it,
// delete its local definition (or wrap it in
// #ifndef RB_PATH_TRACING_CLEANROOM_COMMON_RESTIR_GI_PARAMETERS_HLSLI) so
// this header stays the single owner.
struct RTXDI_GISpatialResamplingParameters
{
    float samplingRadius;      // disc radius in pixels
    uint numSamples;           // neighbor count
    float depthThreshold;      // relative linear-depth gate
    float normalThreshold;     // min dot(normalA, normalB)
    uint biasCorrectionMode;   // RTXDI_BIAS_CORRECTION_*
    float jacobianCutoff;      // reject shifts with J outside
                               // [1/cutoff, cutoff]; 0 = default (10.0)
    float pairwiseCentralWeight; // pairwise technique prior for the canonical
                                 // temporal reservoir; 1 = equal prior
    uint fastHistoryLength;      // confidence below this uses recovery search
    uint sharedTilePattern;      // share neighbor locations over mature 4x4 tiles
};

#endif
