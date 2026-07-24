#pragma once

#include "PathTraceGeometryLifecycle.h"
#include "../RenderCommon.h"

#include <vector>

class RtPathTraceInstanceUniverse;
class RtSmokeGeometryUniverse;
struct RtSmokeMaterialStats;

struct RtPathTraceEntityFeedFrameEntity
{
    // A value clone constructed in frame memory. Pointers inside parms are
    // limited to stable renderer assets; frontend-owned callback/joint/gui/
    // remote-view/sound pointers are cleared by the producer.
    idRenderEntityLocal entity;
    const idRenderEntityLocal* sourceIdentity = nullptr;
    PtRenderDefKey renderDefKey;
    uint32_t modelEpoch = 0;
    int currentArea = -1;
    int areaDepth = -1;
    bool onScreen = false;
    areaReference_t areaRef = {};
};

struct RtPathTraceEntityFeedFrameSnapshot
{
    uint64 mapLoadSerial = 0;
    int numAreas = 0;
    int entityCount = 0;
    bool* reachableAreas = nullptr;
    int* areaDepth = nullptr;
    areaReference_t* areaHeads = nullptr;
    RtPathTraceEntityFeedFrameEntity* entities = nullptr;
};

struct RtPathTraceEntityFeedStats
{
    int frameIndex = 0;
    int reachableAreas = 0;
    int candidatesS0 = 0;
    int candidatesS1 = 0;
    int candidatesS2 = 0;
    int candidatesS3 = 0;
    int candidatesS4 = 0;
    int admitted = 0;
    int droppedBudget = 0;
    int residencyVisited = 0;
    int residencyDerived = 0;
    int residencyCacheHits = 0;
    int residencyCacheMisses = 0;
    int residencyResidentRecords = 0;
    int residencyEvictedRecords = 0;
    int residencyPlayerAreaAreas = 0;
    int residencyBeyondViewAreas = 0;
    int residencyPlayerAreaRefs = 0;
    int residencyBeyondViewRefs = 0;
    int residencyPlayerAreaVisited = 0;
    int residencyBeyondViewVisited = 0;
    int residencyPlayerAreaDerived = 0;
    int residencyBeyondViewDerived = 0;
    int residencyPlayerAreaVisitMs = 0;
    int residencyBeyondViewVisitMs = 0;
};

void DumpEntityFeedStats(const RtPathTraceEntityFeedStats& s);
void CapturePathTraceEntityFeedFrameSnapshot(viewDef_t* viewDef);
std::vector<bool> BuildEntityFeedReachableAreas(const viewDef_t* viewDef, int maxDepth, float maxDistance);
void DumpEntityFeedSingleBoneDiagnostics(const viewDef_t* viewDef);
void DumpEntityFeedJointAdvanceProbe(const viewDef_t* viewDef);
void DumpEntityFeedReachableCandidateStats(const viewDef_t* viewDef);
void ProduceEntityFeedRigidEntities(const viewDef_t* viewDef, RtSmokeGeometryUniverse& geometryUniverse, RtPathTraceInstanceUniverse& instanceUniverse, RtSmokeMaterialStats& materialStats);
