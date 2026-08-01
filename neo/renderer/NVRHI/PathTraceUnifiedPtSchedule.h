#pragma once

#include <cstdint>

namespace rb::upt {

enum class PrimaryProducerBlockReason : uint32_t {
    None = 0,
    NotRequested,
    PipelineCreationDeferred,
    PipelineUnavailable,
    ResourcesUnavailable,
    DispatchDeferred
};

struct PrimaryProducerScheduleInput {
    bool unifiedPtRequested = false;
    bool cleanDiRequested = false;
    int cleanDiView = 0;
    bool pipelineReady = false;
    bool resourcesReady = false;
    bool isolationActive = false;
    bool isolationAllowsPipelineCreation = true;
    bool isolationAllowsDispatch = true;
};

struct PrimaryProducerSchedule {
    bool requestedByUnifiedPt = false;
    bool requestedByCleanDi = false;
    bool requested = false;
    bool createPipeline = false;
    bool dispatch = false;
    bool returnBeforeLegacyRoutes = false;
    PrimaryProducerBlockReason blockReason = PrimaryProducerBlockReason::None;
};

// Pure host scheduling contract for the single shared camera/primary producer.
// It deliberately knows nothing about DI, GI, reservoirs, or presentation.
inline PrimaryProducerSchedule BuildPrimaryProducerSchedule(
    const PrimaryProducerScheduleInput& input)
{
    PrimaryProducerSchedule result;
    result.requestedByUnifiedPt = input.unifiedPtRequested;
    result.requestedByCleanDi =
        input.cleanDiRequested && input.cleanDiView >= 2 && input.cleanDiView <= 25;
    result.requested = result.requestedByUnifiedPt || result.requestedByCleanDi;
    result.returnBeforeLegacyRoutes = result.requestedByUnifiedPt && !input.cleanDiRequested;

    if (!result.requested)
    {
        result.blockReason = PrimaryProducerBlockReason::NotRequested;
        return result;
    }

    if (!input.pipelineReady)
    {
        if (input.isolationActive && !input.isolationAllowsPipelineCreation)
        {
            result.blockReason = PrimaryProducerBlockReason::PipelineCreationDeferred;
            return result;
        }
        result.createPipeline = true;
        result.blockReason = PrimaryProducerBlockReason::PipelineUnavailable;
        return result;
    }

    if (!input.resourcesReady)
    {
        result.blockReason = PrimaryProducerBlockReason::ResourcesUnavailable;
        return result;
    }

    if (input.isolationActive && !input.isolationAllowsDispatch)
    {
        result.blockReason = PrimaryProducerBlockReason::DispatchDeferred;
        return result;
    }

    result.dispatch = true;
    result.blockReason = PrimaryProducerBlockReason::None;
    return result;
}

} // namespace rb::upt
