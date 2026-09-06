#include "precompiled.h"
#pragma hdrstop

#include "PathTraceSourceOriginCensus.h"
#include "../RenderCommon.h"
#include "../VertexCache.h"

namespace
{
enum class P0HandleState : uint8
{
	Absent,
	Static,
	Current,
	Stale
};

enum class P0CopyOrigin : uint8
{
	FrontendCpuCandidate,
	CurrentFrameCache
};

enum class P0JointOrigin : uint8
{
	FrameOwnedJointSnapshotCandidate,
	StaticModelJointCandidate,
	None,
	InvalidUnknown
};

enum class P0CommandCategory : uint8
{
	Draw3D,
	Gui,
	Nop,
	Other
};

enum class P0SurfaceStatus : uint8
{
	Classifiable,
	NullDrawSurf,
	NullTri
};

struct P0HandleObservation
{
	P0HandleState state = P0HandleState::Absent;
	bool present = false;
	bool isStatic = false;
	bool isCurrent = false;
};

struct P0HandlePopulation
{
	uint64 absent = 0;
	uint64 staticHandle = 0;
	uint64 current = 0;
	uint64 stale = 0;
};

struct P0JointPopulation
{
	uint64 snapshotCandidate = 0;
	uint64 staticModelCandidate = 0;
	uint64 none = 0;
	uint64 invalidUnknown = 0;
};

struct P0FirstDivergence
{
	bool available = false;
	uint64 commandOrdinal = 0;
	uint64 drawSurfOrdinal = 0;
	int entityIndex = -1;
	int modelSurfaceIndex = -1;
	uint32 divergenceMask = 0;
	P0CopyOrigin vertexProbeOrigin = P0CopyOrigin::FrontendCpuCandidate;
	P0CopyOrigin vertexBackendOrigin = P0CopyOrigin::FrontendCpuCandidate;
	P0CopyOrigin indexProbeOrigin = P0CopyOrigin::FrontendCpuCandidate;
	P0CopyOrigin indexBackendOrigin = P0CopyOrigin::FrontendCpuCandidate;
	P0JointOrigin jointProbeOrigin = P0JointOrigin::None;
	P0JointOrigin jointBackendOrigin = P0JointOrigin::None;
};

struct P0Aggregate
{
	bool available = true;
	bool mainThread = false;
	bool noOpportunity = false;
	int entryFrame = -1;
	int exitFrame = -1;
	uint64 commandCount = 0;
	uint64 command3D = 0;
	uint64 commandGui = 0;
	uint64 commandNop = 0;
	uint64 commandOther = 0;
	uint64 viewCount = 0;
	uint64 subviewCount = 0;
	uint64 nullViewDef = 0;
	uint64 invalidDrawSurfArray = 0;
	uint64 drawSurfRows = 0;
	uint64 classifiedRows = 0;
	uint64 nullDrawSurf = 0;
	uint64 nullTri = 0;
	uint64 boundedOverflow = 0;
	uint64 divergenceMask[ 8 ] = {};
	uint64 vertexDivergent = 0;
	uint64 indexDivergent = 0;
	uint64 jointDivergent = 0;
	uint64 vertexDrawSurfSelected = 0;
	uint64 vertexTriSelected = 0;
	uint64 indexDrawSurfSelected = 0;
	uint64 indexTriSelected = 0;
	P0HandlePopulation vertexTri;
	P0HandlePopulation vertexDrawSurf;
	P0HandlePopulation vertexEffective;
	P0HandlePopulation indexTri;
	P0HandlePopulation indexDrawSurf;
	P0HandlePopulation indexEffective;
	P0HandlePopulation jointDrawSurf;
	P0JointPopulation jointProbe;
	P0JointPopulation jointBackend;
	uint64 validationAcceptVertexCpu = 0;
	uint64 validationAcceptVertexCache = 0;
	uint64 validationRejectVertexCpu = 0;
	uint64 validationRejectVertexCache = 0;
	uint64 validationAcceptIndexCpu = 0;
	uint64 validationAcceptIndexCache = 0;
	uint64 validationRejectIndexCpu = 0;
	uint64 validationRejectIndexCache = 0;
	uint64 reconciliationFailures = 0;
	P0FirstDivergence firstDivergence;
};

constexpr P0HandleObservation P0ClassifyHandle(vertCacheHandle_t handle, int frame)
{
	P0HandleObservation result;
	if (handle == 0)
	{
		return result;
	}
	result.present = true;
	result.isStatic = (handle & VERTCACHE_STATIC) != 0;
	result.isCurrent = result.isStatic ||
		(((handle >> VERTCACHE_FRAME_SHIFT) & VERTCACHE_FRAME_MASK) ==
		 (static_cast<uint64>(frame) & VERTCACHE_FRAME_MASK));
	result.state = result.isStatic ? P0HandleState::Static :
		(result.isCurrent ? P0HandleState::Current : P0HandleState::Stale);
	return result;
}

constexpr P0CommandCategory P0ClassifyCommand(renderCommand_t commandId)
{
	return commandId == RC_DRAW_VIEW_3D ? P0CommandCategory::Draw3D :
		(commandId == RC_DRAW_VIEW_GUI ? P0CommandCategory::Gui :
		(commandId == RC_NOP ? P0CommandCategory::Nop : P0CommandCategory::Other));
}

constexpr bool P0DrawSurfArrayIsValid(int numDrawSurfs, bool hasArray)
{
	return numDrawSurfs >= 0 && (numDrawSurfs == 0 || hasArray);
}

constexpr P0SurfaceStatus P0ClassifySurfacePointers(bool hasDrawSurf, bool hasTri)
{
	return !hasDrawSurf ? P0SurfaceStatus::NullDrawSurf :
		(!hasTri ? P0SurfaceStatus::NullTri : P0SurfaceStatus::Classifiable);
}

constexpr vertCacheHandle_t P0SelectEffectiveHandle(vertCacheHandle_t drawSurfHandle, vertCacheHandle_t triHandle)
{
	return drawSurfHandle != 0 ? drawSurfHandle : triHandle;
}

constexpr bool P0WindowAvailable(bool mainThreadAtEntry, int entryFrame, bool mainThreadAtExit, int exitFrame)
{
	return mainThreadAtEntry && mainThreadAtExit && entryFrame == exitFrame;
}

constexpr P0CopyOrigin P0ClassifyCopyOrigin(const P0HandleObservation& observation)
{
	return observation.state == P0HandleState::Current ?
		P0CopyOrigin::CurrentFrameCache : P0CopyOrigin::FrontendCpuCandidate;
}

constexpr P0JointOrigin P0ClassifyJointOrigin(
	P0CopyOrigin vertexOrigin,
	bool hasSnapshot,
	int snapshotCount,
	bool hasStaticModel)
{
	if (vertexOrigin == P0CopyOrigin::CurrentFrameCache)
	{
		return P0JointOrigin::None;
	}
	if (snapshotCount < 0 || (hasSnapshot && snapshotCount <= 0) || (!hasSnapshot && snapshotCount > 0))
	{
		return P0JointOrigin::InvalidUnknown;
	}
	if (hasSnapshot)
	{
		return P0JointOrigin::FrameOwnedJointSnapshotCandidate;
	}
	if (hasStaticModel)
	{
		return P0JointOrigin::StaticModelJointCandidate;
	}
	return P0JointOrigin::None;
}

constexpr bool P0ValidationWouldAccept(
	const P0HandleObservation& triVertex,
	const P0HandleObservation& triIndex)
{
	return (!triVertex.present || triVertex.isCurrent) &&
		(!triIndex.present || triIndex.isCurrent);
}

constexpr uint32 P0DivergenceMask(
	P0CopyOrigin vertexProbe,
	P0CopyOrigin vertexBackend,
	P0CopyOrigin indexProbe,
	P0CopyOrigin indexBackend,
	P0JointOrigin jointProbe,
	P0JointOrigin jointBackend)
{
	return (vertexProbe != vertexBackend ? 1u : 0u) |
		(indexProbe != indexBackend ? 2u : 0u) |
		(jointProbe != jointBackend ? 4u : 0u);
}

void P0AddHandlePopulation(P0HandlePopulation& population, P0HandleState state)
{
	switch (state)
	{
		case P0HandleState::Absent: ++population.absent; break;
		case P0HandleState::Static: ++population.staticHandle; break;
		case P0HandleState::Current: ++population.current; break;
		case P0HandleState::Stale: ++population.stale; break;
	}
}

void P0AddJointPopulation(P0JointPopulation& population, P0JointOrigin origin)
{
	switch (origin)
	{
		case P0JointOrigin::FrameOwnedJointSnapshotCandidate: ++population.snapshotCandidate; break;
		case P0JointOrigin::StaticModelJointCandidate: ++population.staticModelCandidate; break;
		case P0JointOrigin::None: ++population.none; break;
		case P0JointOrigin::InvalidUnknown: ++population.invalidUnknown; break;
	}
}

constexpr uint64 P0HandlePopulationTotal(const P0HandlePopulation& population)
{
	return population.absent + population.staticHandle + population.current + population.stale;
}

constexpr uint64 P0JointPopulationTotal(const P0JointPopulation& population)
{
	return population.snapshotCandidate + population.staticModelCandidate +
		population.none + population.invalidUnknown;
}

constexpr void P0Reconcile(P0Aggregate& aggregate)
{
	const uint64 invalidRows = aggregate.nullDrawSurf + aggregate.nullTri;
	uint64 maskTotal = 0;
	for (int i = 0; i < 8; ++i)
	{
		maskTotal += aggregate.divergenceMask[ i ];
	}
	if (aggregate.commandCount != aggregate.command3D + aggregate.commandGui +
		aggregate.commandNop + aggregate.commandOther)
	{
		++aggregate.reconciliationFailures;
	}
	if (aggregate.drawSurfRows != aggregate.classifiedRows + invalidRows)
	{
		++aggregate.reconciliationFailures;
	}
	if (aggregate.command3D != aggregate.viewCount + aggregate.nullViewDef ||
		aggregate.subviewCount > aggregate.viewCount ||
		aggregate.invalidDrawSurfArray > aggregate.viewCount)
	{
		++aggregate.reconciliationFailures;
	}
	if (aggregate.classifiedRows != maskTotal)
	{
		++aggregate.reconciliationFailures;
	}
	if (aggregate.classifiedRows != aggregate.vertexDrawSurfSelected + aggregate.vertexTriSelected ||
		aggregate.classifiedRows != aggregate.indexDrawSurfSelected + aggregate.indexTriSelected)
	{
		++aggregate.reconciliationFailures;
	}
	if (aggregate.classifiedRows != P0HandlePopulationTotal(aggregate.vertexTri) ||
		aggregate.classifiedRows != P0HandlePopulationTotal(aggregate.vertexDrawSurf) ||
		aggregate.classifiedRows != P0HandlePopulationTotal(aggregate.vertexEffective) ||
		aggregate.classifiedRows != P0HandlePopulationTotal(aggregate.indexTri) ||
		aggregate.classifiedRows != P0HandlePopulationTotal(aggregate.indexDrawSurf) ||
		aggregate.classifiedRows != P0HandlePopulationTotal(aggregate.indexEffective) ||
		aggregate.classifiedRows != P0HandlePopulationTotal(aggregate.jointDrawSurf))
	{
		++aggregate.reconciliationFailures;
	}
	if (aggregate.classifiedRows != P0JointPopulationTotal(aggregate.jointProbe) ||
		aggregate.classifiedRows != P0JointPopulationTotal(aggregate.jointBackend))
	{
		++aggregate.reconciliationFailures;
	}
	if (aggregate.classifiedRows != aggregate.validationAcceptVertexCpu +
		aggregate.validationAcceptVertexCache + aggregate.validationRejectVertexCpu +
		aggregate.validationRejectVertexCache ||
		aggregate.classifiedRows != aggregate.validationAcceptIndexCpu +
		aggregate.validationAcceptIndexCache + aggregate.validationRejectIndexCpu +
		aggregate.validationRejectIndexCache)
	{
		++aggregate.reconciliationFailures;
	}
	if (aggregate.vertexDivergent != aggregate.divergenceMask[ 1 ] + aggregate.divergenceMask[ 3 ] +
		aggregate.divergenceMask[ 5 ] + aggregate.divergenceMask[ 7 ] ||
		aggregate.indexDivergent != aggregate.divergenceMask[ 2 ] + aggregate.divergenceMask[ 3 ] +
		aggregate.divergenceMask[ 6 ] + aggregate.divergenceMask[ 7 ] ||
		aggregate.jointDivergent != aggregate.divergenceMask[ 4 ] + aggregate.divergenceMask[ 5 ] +
		aggregate.divergenceMask[ 6 ] + aggregate.divergenceMask[ 7 ])
	{
		++aggregate.reconciliationFailures;
	}
	if (aggregate.reconciliationFailures != 0)
	{
		aggregate.available = false;
	}
}

constexpr vertCacheHandle_t P0TestFrameHandle(int frame)
{
	return static_cast<vertCacheHandle_t>(static_cast<uint64>(frame & VERTCACHE_FRAME_MASK) << VERTCACHE_FRAME_SHIFT);
}

static_assert(P0ClassifyHandle(0, 7).state == P0HandleState::Absent, "zero must be absent before frame classification");
static_assert(P0ClassifyHandle(VERTCACHE_STATIC, 7).state == P0HandleState::Static, "static handle classification");
static_assert(P0ClassifyHandle(P0TestFrameHandle(7), 7).state == P0HandleState::Current, "current handle classification");
static_assert(P0ClassifyHandle(P0TestFrameHandle(6), 7).state == P0HandleState::Stale, "stale handle classification");
static_assert(P0SelectEffectiveHandle(P0TestFrameHandle(7), P0TestFrameHandle(6)) == P0TestFrameHandle(7),
	"nonzero draw-surface handle must override tri handle");
static_assert(P0SelectEffectiveHandle(0, P0TestFrameHandle(6)) == P0TestFrameHandle(6),
	"zero draw-surface handle must fall back to tri handle");
static_assert(P0ClassifyCopyOrigin(P0ClassifyHandle(P0TestFrameHandle(7), 7)) == P0CopyOrigin::CurrentFrameCache,
	"current frontend handle must select mapped frame cache");
static_assert(P0ClassifyCopyOrigin(P0ClassifyHandle(P0TestFrameHandle(7), 8)) == P0CopyOrigin::FrontendCpuCandidate,
	"current-before-BeginBackEnd handle must predict stale CPU fallback afterward");
static_assert(P0ClassifyJointOrigin(P0CopyOrigin::FrontendCpuCandidate, true, 2, false) ==
	P0JointOrigin::FrameOwnedJointSnapshotCandidate, "snapshot candidate taxonomy");
static_assert(P0ClassifyJointOrigin(P0CopyOrigin::FrontendCpuCandidate, false, 0, true) ==
	P0JointOrigin::StaticModelJointCandidate, "static model candidate taxonomy");
static_assert(P0ClassifyJointOrigin(P0CopyOrigin::FrontendCpuCandidate, false, 0, false) ==
	P0JointOrigin::None, "empty joint taxonomy");
static_assert(P0ClassifyJointOrigin(P0CopyOrigin::FrontendCpuCandidate, true, -1, false) ==
	P0JointOrigin::InvalidUnknown, "invalid joint count taxonomy");
static_assert(P0ClassifyCommand(RC_NOP) == P0CommandCategory::Nop &&
	P0ClassifyCommand(RC_DRAW_VIEW_GUI) == P0CommandCategory::Gui &&
	P0ClassifyCommand(RC_DRAW_VIEW_3D) == P0CommandCategory::Draw3D &&
	P0ClassifyCommand(RC_POST_PROCESS) == P0CommandCategory::Other,
	"only 3D commands may enter the source-origin denominator");
constexpr bool P0CommandMixtureFixture()
{
	const renderCommand_t commands[] = {
		RC_NOP, RC_DRAW_VIEW_GUI, RC_DRAW_VIEW_3D, RC_POST_PROCESS, RC_DRAW_VIEW_3D
	};
	uint32 draw3D = 0;
	uint32 gui = 0;
	uint32 nop = 0;
	uint32 other = 0;
	for (renderCommand_t command : commands)
	{
		switch (P0ClassifyCommand(command))
		{
			case P0CommandCategory::Draw3D: ++draw3D; break;
			case P0CommandCategory::Gui: ++gui; break;
			case P0CommandCategory::Nop: ++nop; break;
			case P0CommandCategory::Other: ++other; break;
		}
	}
	return draw3D == 2 && gui == 1 && nop == 1 && other == 1;
}
static_assert(P0CommandMixtureFixture(), "mixed command fixture must count two independent 3D commands");
static_assert(P0DrawSurfArrayIsValid(0, false) && P0DrawSurfArrayIsValid(2, true) &&
	!P0DrawSurfArrayIsValid(-1, true) && !P0DrawSurfArrayIsValid(2, false),
	"null draw-surface arrays are valid only for an empty view");
static_assert(P0ClassifySurfacePointers(false, false) == P0SurfaceStatus::NullDrawSurf &&
	P0ClassifySurfacePointers(true, false) == P0SurfaceStatus::NullTri &&
	P0ClassifySurfacePointers(true, true) == P0SurfaceStatus::Classifiable,
	"null draw-surface rows and null front-end tris must remain distinct invalid buckets");
static_assert(P0WindowAvailable(true, 9, true, 9), "stable main-thread window must remain available");
static_assert(!P0WindowAvailable(false, 9, true, 9) && !P0WindowAvailable(true, 9, true, 10),
	"non-main entry or cache-frame transition must fail the diagnostic closed");

constexpr P0Aggregate P0MakeReconciliationFixture()
{
	P0Aggregate a;
	a.commandCount = 4;
	a.command3D = 1;
	a.commandGui = 1;
	a.commandNop = 1;
	a.commandOther = 1;
	a.viewCount = 1;
	a.drawSurfRows = 1;
	a.classifiedRows = 1;
	a.divergenceMask[ 3 ] = 1;
	a.vertexDivergent = 1;
	a.indexDivergent = 1;
	a.vertexTriSelected = 1;
	a.indexTriSelected = 1;
	a.vertexTri.current = 1;
	a.vertexDrawSurf.absent = 1;
	a.vertexEffective.current = 1;
	a.indexTri.current = 1;
	a.indexDrawSurf.absent = 1;
	a.indexEffective.current = 1;
	a.jointDrawSurf.absent = 1;
	a.jointProbe.none = 1;
	a.jointBackend.none = 1;
	a.validationAcceptVertexCache = 1;
	a.validationAcceptIndexCache = 1;
	return a;
}

constexpr bool P0ReconciliationAcceptsFixture()
{
	P0Aggregate a = P0MakeReconciliationFixture();
	P0Reconcile(a);
	return a.reconciliationFailures == 0 && a.available;
}

constexpr bool P0ReconciliationRejectsMutations()
{
	P0Aggregate commandMutation = P0MakeReconciliationFixture();
	++commandMutation.commandOther;
	P0Reconcile(commandMutation);
	P0Aggregate rowMutation = P0MakeReconciliationFixture();
	++rowMutation.nullTri;
	P0Reconcile(rowMutation);
	P0Aggregate maskMutation = P0MakeReconciliationFixture();
	++maskMutation.divergenceMask[ 0 ];
	P0Reconcile(maskMutation);
	P0Aggregate populationMutation = P0MakeReconciliationFixture();
	++populationMutation.vertexEffective.stale;
	P0Reconcile(populationMutation);
	P0Aggregate crossTabMutation = P0MakeReconciliationFixture();
	++crossTabMutation.validationAcceptVertexCpu;
	P0Reconcile(crossTabMutation);
	return commandMutation.reconciliationFailures != 0 && rowMutation.reconciliationFailures != 0 &&
		maskMutation.reconciliationFailures != 0 && populationMutation.reconciliationFailures != 0 &&
		crossTabMutation.reconciliationFailures != 0;
}

static_assert(P0ReconciliationAcceptsFixture(), "complete census populations must reconcile");
static_assert(P0ReconciliationRejectsMutations(), "omitted or double-counted census buckets must fail reconciliation");

void P0ObserveSurface(
	P0Aggregate& aggregate,
	uint64 commandOrdinal,
	uint64 drawSurfOrdinal,
	const drawSurf_t* drawSurf,
	int entryFrame)
{
	++aggregate.drawSurfRows;
	const P0SurfaceStatus surfaceStatus = P0ClassifySurfacePointers(
		drawSurf != nullptr, drawSurf != nullptr && drawSurf->frontEndGeo != nullptr);
	if (surfaceStatus == P0SurfaceStatus::NullDrawSurf)
	{
		++aggregate.nullDrawSurf;
		return;
	}
	const srfTriangles_t* tri = drawSurf->frontEndGeo;
	if (surfaceStatus == P0SurfaceStatus::NullTri)
	{
		++aggregate.nullTri;
		return;
	}

	const P0HandleObservation triVertex = P0ClassifyHandle(tri->ambientCache, entryFrame);
	const P0HandleObservation drawVertex = P0ClassifyHandle(drawSurf->ambientCache, entryFrame);
	const bool vertexUsesDrawSurf = drawSurf->ambientCache != 0;
	const vertCacheHandle_t effectiveVertexHandle = P0SelectEffectiveHandle(drawSurf->ambientCache, tri->ambientCache);
	const P0HandleObservation effectiveVertexProbe = P0ClassifyHandle(effectiveVertexHandle, entryFrame);
	const P0HandleObservation effectiveVertexBackend = P0ClassifyHandle(effectiveVertexHandle, entryFrame + 1);

	const P0HandleObservation triIndex = P0ClassifyHandle(tri->indexCache, entryFrame);
	const P0HandleObservation drawIndex = P0ClassifyHandle(drawSurf->indexCache, entryFrame);
	const bool indexUsesDrawSurf = drawSurf->indexCache != 0;
	const vertCacheHandle_t effectiveIndexHandle = P0SelectEffectiveHandle(drawSurf->indexCache, tri->indexCache);
	const P0HandleObservation effectiveIndexProbe = P0ClassifyHandle(effectiveIndexHandle, entryFrame);
	const P0HandleObservation effectiveIndexBackend = P0ClassifyHandle(effectiveIndexHandle, entryFrame + 1);

	const P0CopyOrigin vertexProbeOrigin = P0ClassifyCopyOrigin(effectiveVertexProbe);
	const P0CopyOrigin vertexBackendOrigin = P0ClassifyCopyOrigin(effectiveVertexBackend);
	const P0CopyOrigin indexProbeOrigin = P0ClassifyCopyOrigin(effectiveIndexProbe);
	const P0CopyOrigin indexBackendOrigin = P0ClassifyCopyOrigin(effectiveIndexBackend);
	// Snapshot/static-model payloads may be poison in this census. Presence and
	// count are sufficient for the conservative candidate taxonomy; never follow them.
	const P0HandleObservation jointHandle = P0ClassifyHandle(drawSurf->jointCache, entryFrame);
	const bool hasSnapshot = drawSurf->jointCacheCpuSnapshot != nullptr;
	const int snapshotCount = drawSurf->jointCacheCpuSnapshotCount;
	const bool hasStaticModel = tri->staticModelWithJoints != nullptr;
	const P0JointOrigin jointProbeOrigin = P0ClassifyJointOrigin(
		vertexProbeOrigin, hasSnapshot, snapshotCount, hasStaticModel);
	const P0JointOrigin jointBackendOrigin = P0ClassifyJointOrigin(
		vertexBackendOrigin, hasSnapshot, snapshotCount, hasStaticModel);
	const uint32 divergenceMask = P0DivergenceMask(
		vertexProbeOrigin, vertexBackendOrigin,
		indexProbeOrigin, indexBackendOrigin,
		jointProbeOrigin, jointBackendOrigin);

	++aggregate.classifiedRows;
	++aggregate.divergenceMask[ divergenceMask ];
	aggregate.vertexDivergent += (divergenceMask & 1u) != 0;
	aggregate.indexDivergent += (divergenceMask & 2u) != 0;
	aggregate.jointDivergent += (divergenceMask & 4u) != 0;
	aggregate.vertexDrawSurfSelected += vertexUsesDrawSurf;
	aggregate.vertexTriSelected += !vertexUsesDrawSurf;
	aggregate.indexDrawSurfSelected += indexUsesDrawSurf;
	aggregate.indexTriSelected += !indexUsesDrawSurf;
	P0AddHandlePopulation(aggregate.vertexTri, triVertex.state);
	P0AddHandlePopulation(aggregate.vertexDrawSurf, drawVertex.state);
	P0AddHandlePopulation(aggregate.vertexEffective, effectiveVertexProbe.state);
	P0AddHandlePopulation(aggregate.indexTri, triIndex.state);
	P0AddHandlePopulation(aggregate.indexDrawSurf, drawIndex.state);
	P0AddHandlePopulation(aggregate.indexEffective, effectiveIndexProbe.state);
	P0AddHandlePopulation(aggregate.jointDrawSurf, jointHandle.state);
	P0AddJointPopulation(aggregate.jointProbe, jointProbeOrigin);
	P0AddJointPopulation(aggregate.jointBackend, jointBackendOrigin);

	const bool validationWouldAccept = P0ValidationWouldAccept(triVertex, triIndex);
	if (validationWouldAccept)
	{
		vertexProbeOrigin == P0CopyOrigin::CurrentFrameCache ?
			++aggregate.validationAcceptVertexCache : ++aggregate.validationAcceptVertexCpu;
		indexProbeOrigin == P0CopyOrigin::CurrentFrameCache ?
			++aggregate.validationAcceptIndexCache : ++aggregate.validationAcceptIndexCpu;
	}
	else
	{
		vertexProbeOrigin == P0CopyOrigin::CurrentFrameCache ?
			++aggregate.validationRejectVertexCache : ++aggregate.validationRejectVertexCpu;
		indexProbeOrigin == P0CopyOrigin::CurrentFrameCache ?
			++aggregate.validationRejectIndexCache : ++aggregate.validationRejectIndexCpu;
	}

	if (divergenceMask != 0 && !aggregate.firstDivergence.available)
	{
		P0FirstDivergence& first = aggregate.firstDivergence;
		first.available = true;
		first.commandOrdinal = commandOrdinal;
		first.drawSurfOrdinal = drawSurfOrdinal;
		first.entityIndex = drawSurf->space != nullptr && drawSurf->space->entityDef != nullptr ?
			drawSurf->space->entityDef->index : -1;
		first.modelSurfaceIndex = drawSurf->modelSurfaceIndex;
		first.divergenceMask = divergenceMask;
		first.vertexProbeOrigin = vertexProbeOrigin;
		first.vertexBackendOrigin = vertexBackendOrigin;
		first.indexProbeOrigin = indexProbeOrigin;
		first.indexBackendOrigin = indexBackendOrigin;
		first.jointProbeOrigin = jointProbeOrigin;
		first.jointBackendOrigin = jointBackendOrigin;
	}
}

void P0TagAggregate(const P0Aggregate& a)
{
	OPTICK_TAG("available", a.available ? 1 : 0);
	OPTICK_TAG("noOpportunity", a.noOpportunity ? 1 : 0);
	OPTICK_TAG("mainThread", a.mainThread ? 1 : 0);
	OPTICK_TAG("entryFrame", a.entryFrame);
	OPTICK_TAG("exitFrame", a.exitFrame);
	OPTICK_TAG("commands", a.commandCount);
	OPTICK_TAG("command3D", a.command3D);
	OPTICK_TAG("commandGui", a.commandGui);
	OPTICK_TAG("commandNop", a.commandNop);
	OPTICK_TAG("commandOther", a.commandOther);
	OPTICK_TAG("views", a.viewCount);
	OPTICK_TAG("subviews", a.subviewCount);
	OPTICK_TAG("nullViewDef", a.nullViewDef);
	OPTICK_TAG("invalidDrawSurfArray", a.invalidDrawSurfArray);
	OPTICK_TAG("drawSurfRows", a.drawSurfRows);
	OPTICK_TAG("classified", a.classifiedRows);
	OPTICK_TAG("nullDrawSurf", a.nullDrawSurf);
	OPTICK_TAG("nullTri", a.nullTri);
	OPTICK_TAG("boundedOverflow", a.boundedOverflow);
	OPTICK_TAG("divergenceMask0", a.divergenceMask[ 0 ]);
	OPTICK_TAG("divergenceMask1", a.divergenceMask[ 1 ]);
	OPTICK_TAG("divergenceMask2", a.divergenceMask[ 2 ]);
	OPTICK_TAG("divergenceMask3", a.divergenceMask[ 3 ]);
	OPTICK_TAG("divergenceMask4", a.divergenceMask[ 4 ]);
	OPTICK_TAG("divergenceMask5", a.divergenceMask[ 5 ]);
	OPTICK_TAG("divergenceMask6", a.divergenceMask[ 6 ]);
	OPTICK_TAG("divergenceMask7", a.divergenceMask[ 7 ]);
	OPTICK_TAG("vertexD", a.vertexDivergent);
	OPTICK_TAG("indexD", a.indexDivergent);
	OPTICK_TAG("jointCandidateD", a.jointDivergent);
	OPTICK_TAG("vertexDrawSurfSelected", a.vertexDrawSurfSelected);
	OPTICK_TAG("vertexTriSelected", a.vertexTriSelected);
	OPTICK_TAG("indexDrawSurfSelected", a.indexDrawSurfSelected);
	OPTICK_TAG("indexTriSelected", a.indexTriSelected);
	OPTICK_TAG("vertexTriAbsent", a.vertexTri.absent);
	OPTICK_TAG("vertexTriStatic", a.vertexTri.staticHandle);
	OPTICK_TAG("vertexTriCurrent", a.vertexTri.current);
	OPTICK_TAG("vertexTriStale", a.vertexTri.stale);
	OPTICK_TAG("vertexDrawAbsent", a.vertexDrawSurf.absent);
	OPTICK_TAG("vertexDrawStatic", a.vertexDrawSurf.staticHandle);
	OPTICK_TAG("vertexDrawCurrent", a.vertexDrawSurf.current);
	OPTICK_TAG("vertexDrawStale", a.vertexDrawSurf.stale);
	OPTICK_TAG("vertexEffectiveAbsent", a.vertexEffective.absent);
	OPTICK_TAG("vertexEffectiveStatic", a.vertexEffective.staticHandle);
	OPTICK_TAG("vertexEffectiveCurrent", a.vertexEffective.current);
	OPTICK_TAG("vertexEffectiveStale", a.vertexEffective.stale);
	OPTICK_TAG("indexTriAbsent", a.indexTri.absent);
	OPTICK_TAG("indexTriStatic", a.indexTri.staticHandle);
	OPTICK_TAG("indexTriCurrent", a.indexTri.current);
	OPTICK_TAG("indexTriStale", a.indexTri.stale);
	OPTICK_TAG("indexDrawAbsent", a.indexDrawSurf.absent);
	OPTICK_TAG("indexDrawStatic", a.indexDrawSurf.staticHandle);
	OPTICK_TAG("indexDrawCurrent", a.indexDrawSurf.current);
	OPTICK_TAG("indexDrawStale", a.indexDrawSurf.stale);
	OPTICK_TAG("indexEffectiveAbsent", a.indexEffective.absent);
	OPTICK_TAG("indexEffectiveStatic", a.indexEffective.staticHandle);
	OPTICK_TAG("indexEffectiveCurrent", a.indexEffective.current);
	OPTICK_TAG("indexEffectiveStale", a.indexEffective.stale);
	OPTICK_TAG("jointHandleAbsent", a.jointDrawSurf.absent);
	OPTICK_TAG("jointHandleStatic", a.jointDrawSurf.staticHandle);
	OPTICK_TAG("jointHandleCurrent", a.jointDrawSurf.current);
	OPTICK_TAG("jointHandleStale", a.jointDrawSurf.stale);
	OPTICK_TAG("jointProbeSnapshot", a.jointProbe.snapshotCandidate);
	OPTICK_TAG("jointProbeStaticModel", a.jointProbe.staticModelCandidate);
	OPTICK_TAG("jointProbeNone", a.jointProbe.none);
	OPTICK_TAG("jointProbeInvalid", a.jointProbe.invalidUnknown);
	OPTICK_TAG("jointBackendSnapshot", a.jointBackend.snapshotCandidate);
	OPTICK_TAG("jointBackendStaticModel", a.jointBackend.staticModelCandidate);
	OPTICK_TAG("jointBackendNone", a.jointBackend.none);
	OPTICK_TAG("jointBackendInvalid", a.jointBackend.invalidUnknown);
	OPTICK_TAG("validationAcceptVertexCpu", a.validationAcceptVertexCpu);
	OPTICK_TAG("validationAcceptVertexCache", a.validationAcceptVertexCache);
	OPTICK_TAG("validationRejectVertexCpu", a.validationRejectVertexCpu);
	OPTICK_TAG("validationRejectVertexCache", a.validationRejectVertexCache);
	OPTICK_TAG("validationAcceptIndexCpu", a.validationAcceptIndexCpu);
	OPTICK_TAG("validationAcceptIndexCache", a.validationAcceptIndexCache);
	OPTICK_TAG("validationRejectIndexCpu", a.validationRejectIndexCpu);
	OPTICK_TAG("validationRejectIndexCache", a.validationRejectIndexCache);
	OPTICK_TAG("reconciliationFailures", a.reconciliationFailures);
	OPTICK_TAG("firstDAvailable", a.firstDivergence.available ? 1 : 0);
	OPTICK_TAG("firstDCommandOrdinal", a.firstDivergence.commandOrdinal);
	OPTICK_TAG("firstDDrawSurfOrdinal", a.firstDivergence.drawSurfOrdinal);
	OPTICK_TAG("firstDEntityIndex", a.firstDivergence.entityIndex);
	OPTICK_TAG("firstDModelSurfaceIndex", a.firstDivergence.modelSurfaceIndex);
	OPTICK_TAG("firstDMask", a.firstDivergence.divergenceMask);
	OPTICK_TAG("firstDVertexProbeOrigin", static_cast<uint32>(a.firstDivergence.vertexProbeOrigin));
	OPTICK_TAG("firstDVertexBackendOrigin", static_cast<uint32>(a.firstDivergence.vertexBackendOrigin));
	OPTICK_TAG("firstDIndexProbeOrigin", static_cast<uint32>(a.firstDivergence.indexProbeOrigin));
	OPTICK_TAG("firstDIndexBackendOrigin", static_cast<uint32>(a.firstDivergence.indexBackendOrigin));
	OPTICK_TAG("firstDJointProbeOrigin", static_cast<uint32>(a.firstDivergence.jointProbeOrigin));
	OPTICK_TAG("firstDJointBackendOrigin", static_cast<uint32>(a.firstDivergence.jointBackendOrigin));
}
}

void RunPathTraceSourceOriginCensusP0()
{
	OPTICK_EVENT("PT Capture Source Origin Census P0");
	P0Aggregate aggregate;
	aggregate.mainThread = idLib::IsMainThread();
	if (!aggregate.mainThread || frameData == nullptr)
	{
		aggregate.available = false;
		P0TagAggregate(aggregate);
		return;
	}

	aggregate.entryFrame = vertexCache.currentFrame;
	const emptyCommand_t* command = frameData->cmdHead;
	constexpr uint64 kMaxCommands = 65536;
	constexpr uint64 kMaxDrawSurfs = 1048576;
	for (uint64 commandOrdinal = 0; command != nullptr; ++commandOrdinal)
	{
		if (commandOrdinal >= kMaxCommands)
		{
			aggregate.available = false;
			++aggregate.boundedOverflow;
			break;
		}
		++aggregate.commandCount;
		const P0CommandCategory commandCategory = P0ClassifyCommand(command->commandId);
		if (commandCategory == P0CommandCategory::Draw3D)
		{
			++aggregate.command3D;
			// Do not deduplicate viewDef values: command/draw-surface ordinals are the
			// census identity, including repeated views in the closed command chain.
			const drawSurfsCommand_t* drawCommand = reinterpret_cast<const drawSurfsCommand_t*>(command);
			const viewDef_t* viewDef = drawCommand->viewDef;
			if (viewDef == nullptr)
			{
				++aggregate.nullViewDef;
			}
			else
			{
				++aggregate.viewCount;
				aggregate.subviewCount += viewDef->isSubview;
				if (!P0DrawSurfArrayIsValid(viewDef->numDrawSurfs, viewDef->drawSurfs != nullptr))
				{
					++aggregate.invalidDrawSurfArray;
				}
				else if (static_cast<uint64>(viewDef->numDrawSurfs) > kMaxDrawSurfs - aggregate.drawSurfRows)
				{
					aggregate.available = false;
					++aggregate.boundedOverflow;
				}
				else
				{
					for (int drawSurfOrdinal = 0; drawSurfOrdinal < viewDef->numDrawSurfs; ++drawSurfOrdinal)
					{
						P0ObserveSurface(aggregate, commandOrdinal, static_cast<uint64>(drawSurfOrdinal),
							viewDef->drawSurfs[ drawSurfOrdinal ], aggregate.entryFrame);
					}
				}
			}
		}
		else if (commandCategory == P0CommandCategory::Gui)
		{
			++aggregate.commandGui;
		}
		else if (commandCategory == P0CommandCategory::Nop)
		{
			++aggregate.commandNop;
		}
		else
		{
			++aggregate.commandOther;
		}
		command = reinterpret_cast<const emptyCommand_t*>(command->next);
	}

	aggregate.exitFrame = vertexCache.currentFrame;
	if (!P0WindowAvailable(aggregate.mainThread, aggregate.entryFrame, idLib::IsMainThread(), aggregate.exitFrame))
	{
		aggregate.available = false;
	}
	P0Reconcile(aggregate);
	aggregate.noOpportunity = aggregate.available && aggregate.drawSurfRows == 0 &&
		aggregate.nullViewDef == 0 && aggregate.invalidDrawSurfArray == 0;
	P0TagAggregate(aggregate);
}
