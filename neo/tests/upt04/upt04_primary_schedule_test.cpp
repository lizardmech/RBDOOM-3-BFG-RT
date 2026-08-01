#include "PathTraceUnifiedPtSchedule.h"

#include <iostream>

namespace {

using namespace rb::upt;

int failures = 0;

void Check(bool condition, const char* message)
{
    if (!condition)
    {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

PrimaryProducerSchedule ReadySchedule(bool upt, bool clean, int view)
{
    PrimaryProducerScheduleInput input;
    input.unifiedPtRequested = upt;
    input.cleanDiRequested = clean;
    input.cleanDiView = view;
    input.pipelineReady = true;
    input.resourcesReady = true;
    return BuildPrimaryProducerSchedule(input);
}

} // namespace

int main()
{
    {
        const PrimaryProducerSchedule plan = ReadySchedule(false, false, 0);
        Check(!plan.requested && !plan.dispatch,
            "disabled routes must not produce a camera ray");
        Check(plan.blockReason == PrimaryProducerBlockReason::NotRequested,
            "disabled routes must report not requested");
    }
    {
        const PrimaryProducerSchedule plan = ReadySchedule(true, false, 0);
        Check(plan.requestedByUnifiedPt && !plan.requestedByCleanDi,
            "UPT-only must own an independent primary request");
        Check(plan.dispatch, "UPT-only must dispatch the primary producer once");
        Check(plan.returnBeforeLegacyRoutes,
            "UPT-only must return before legacy DI/GI execution");
    }
    {
        const PrimaryProducerSchedule plan = ReadySchedule(false, true, 16);
        Check(!plan.requestedByUnifiedPt && plan.requestedByCleanDi,
            "Clean-DI production view must preserve its primary request");
        Check(plan.dispatch && !plan.returnBeforeLegacyRoutes,
            "Clean-DI must continue after the shared primary producer");
    }
    {
        const PrimaryProducerSchedule plan = ReadySchedule(true, true, 16);
        Check(plan.requestedByUnifiedPt && plan.requestedByCleanDi,
            "coexisting consumers must both be represented");
        Check(plan.dispatch,
            "coexisting consumers must schedule one shared producer dispatch");
        Check(!plan.returnBeforeLegacyRoutes,
            "Clean-DI consumer must remain reachable when both routes are on");
    }
    {
        const PrimaryProducerSchedule plan = ReadySchedule(false, true, 1);
        Check(!plan.requested && !plan.dispatch,
            "Clean-DI sentinel view must not start the primary producer");
    }
    {
        PrimaryProducerScheduleInput input;
        input.unifiedPtRequested = true;
        input.pipelineReady = false;
        input.resourcesReady = true;
        const PrimaryProducerSchedule plan = BuildPrimaryProducerSchedule(input);
        Check(plan.createPipeline && !plan.dispatch,
            "a missing primary pipeline must be created without same-plan dispatch");
        Check(plan.blockReason == PrimaryProducerBlockReason::PipelineUnavailable,
            "pipeline warmup must be explicit");
    }
    {
        PrimaryProducerScheduleInput input;
        input.unifiedPtRequested = true;
        input.pipelineReady = true;
        input.resourcesReady = false;
        const PrimaryProducerSchedule plan = BuildPrimaryProducerSchedule(input);
        Check(!plan.dispatch &&
                plan.blockReason == PrimaryProducerBlockReason::ResourcesUnavailable,
            "missing primary resources must fail closed");
    }
    {
        PrimaryProducerScheduleInput input;
        input.cleanDiRequested = true;
        input.cleanDiView = 16;
        input.pipelineReady = true;
        input.resourcesReady = true;
        input.isolationActive = true;
        input.isolationAllowsDispatch = false;
        const PrimaryProducerSchedule plan = BuildPrimaryProducerSchedule(input);
        Check(!plan.dispatch &&
                plan.blockReason == PrimaryProducerBlockReason::DispatchDeferred,
            "existing staged isolation must still defer primary dispatch");
    }

    if (failures != 0)
    {
        std::cerr << "UPT-04 primary schedule tests failed: " << failures << '\n';
        return 1;
    }
    std::cout << "UPT-04 primary schedule tests passed; one shared producer, no DI/GI ownership\n";
    return 0;
}
