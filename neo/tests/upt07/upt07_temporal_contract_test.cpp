#include "upt07_temporal_contract.h"

#include <cstdlib>
#include <iostream>

namespace {

using namespace rb::upt02;
using namespace rb::upt07;

int gFailures = 0;

void Check(bool condition, const char* message) {
	if (!condition) {
		std::cerr << "FAIL: " << message << '\n';
		++gFailures;
	}
}

bool Near(double actual, double expected, double tolerance = 1.0e-9) {
	return std::abs(actual - expected) <= tolerance;
}

Candidate Positive(uint32_t identity, uint32_t generation, double target = 2.0) {
	Candidate candidate = {};
	candidate.status = CandidateStatus::ValidPositive;
	candidate.eventKind = EventKind::Analytic;
	candidate.lobeMask = LobeDiffuse;
	candidate.flags = FlagVisibilityKnown | FlagVisibilityPassed;
	candidate.identity0 = identity;
	candidate.generationFingerprint = generation;
	candidate.sampleCoord0 = 0.25;
	candidate.sampleCoord1 = 0.5;
	candidate.contribution = { target, target * 0.5, target * 0.25 };
	candidate.target = target;
	candidate.proposalPdf = 0.5;
	candidate.pathPdf = 0.5;
	candidate.partialJacobian = 1.0;
	candidate.accumulatedRouletteProbability = 1.0;
	return candidate;
}

LogicalReservoir Finalized(uint32_t identity, uint32_t generation, uint32_t m, double meanWeight) {
	LogicalReservoir reservoir = {};
	reservoir.hasSelectedSample = true;
	reservoir.effectiveM = m;
	reservoir.weightSum = meanWeight;
	reservoir.selected = Positive(identity, generation);
	return reservoir;
}

Surface TestSurface() {
	Surface surface = {};
	surface.valid = true;
	surface.worldPosition = { 10.0, 0.0, 0.0 };
	surface.geometricNormal = { -1.0, 0.0, 0.0 };
	surface.roughness = 0.4;
	surface.previousViewDepth = 10.0;
	surface.materialId = 7;
	surface.materialIndex = 3;
	surface.surfaceClass = 2;
	return surface;
}

PreviousCamera TestCamera(uint32_t width = 8, uint32_t height = 8) {
	PreviousCamera camera = {};
	camera.valid = true;
	camera.width = width;
	camera.height = height;
	return camera;
}

void TestUniformPageAdmission() {
	PageMetadata page = { true, 8, 8, 0x1122334455667788ull, 9, 41 };
	Check(AdmitHistoryPage(page, 8, 8, page.contentGeneration, 9, 42),
		"matching page metadata must admit history uniformly");
	PageMetadata stale = page;
	stale.contentGeneration ^= 1u;
	Check(!AdmitHistoryPage(stale, 8, 8, page.contentGeneration, 9, 42),
		"generation mismatch must reject the page before per-pixel reads");
	stale = page;
	stale.historyEpoch = 8;
	Check(!AdmitHistoryPage(stale, 8, 8, page.contentGeneration, 9, 42),
		"history epoch mismatch must reject the whole page");
	stale = page;
	stale.fullyWritten = false;
	Check(!AdmitHistoryPage(stale, 8, 8, page.contentGeneration, 9, 42),
		"partial proof-stage output must never become history");
	stale = page;
	stale.frameSerial = 40;
	Check(!AdmitHistoryPage(stale, 8, 8, page.contentGeneration, 9, 42),
		"an N-2 page must not be paired with N-1 primary surfaces");
	Check(!AdmitHistoryPage(page, 8, 8, page.contentGeneration, 9, 1),
		"the first publication cannot have adjacent-frame history");
}

void TestRoleFlipFollowsCurrentFramePublication() {
	PageRoles roles = {};
	Check(roles.current == 0 && roles.history == 1,
		"two-page schedule must start with page 0 current and page 1 history");
	Check(!roles.CompleteFrame(true, false, false) &&
		roles.current == 0 && roles.history == 1,
		"a frame without a full D0 publication must not flip roles");
	Check(!roles.CompleteFrame(false, true, false) &&
		roles.current == 0 && roles.history == 1,
		"the no-reuse D0/R0 path must remain on its fixed current page");
	Check(roles.CompleteFrame(true, true, false),
		"a temporal frame with a full current-page publication must flip roles");
	Check(roles.current == 1 && roles.history == 0,
		"a completed temporal frame must promote its physical current page");
	Check(roles.CompleteFrame(true, true, false),
		"a fresh D0 fallback must flip even when T0 skipped");
	Check(roles.current == 0 && roles.history == 1,
		"a skipped T0 must not leave N-2 reservoirs paired with N-1 surfaces");
	Check(!roles.CompleteFrame(true, true, true) &&
		roles.current == 0 && roles.history == 1,
		"a spatial publication already resident in the history page must not flip roles");
}

void TestProjectionAndBoundedSearch() {
	const Surface current = TestSurface();
	const PreviousCamera camera = TestCamera();
	const Projection projection = ProjectToPrevious(current, camera);
	Check(projection.valid && projection.pixelFloorX == 4 && projection.pixelFloorY == 4 &&
		Near(projection.linearDepth, 10.0),
		"static receiver must reproject to the previous center pixel");
	Surface offAxis = current;
	offAxis.worldPosition = { 10.0, 5.0, 0.0 };
	offAxis.previousViewDepth = std::sqrt(125.0);
	const Projection offAxisProjection = ProjectToPrevious(offAxis, camera);
	Check(offAxisProjection.valid &&
		Near(offAxisProjection.linearDepth, std::sqrt(125.0)) &&
		SurfacesCompatible(offAxis, offAxis, offAxisProjection.linearDepth),
		"off-axis projection must validate against ray hitT, not forward-axis depth");

	std::vector<HistoryPixel> previous(64);
	previous[4 * 8 + 4].surface = current;
	previous[4 * 8 + 4].surface.previousViewDepth = 20.0;
	previous[4 * 8 + 3].surface = current;
	const HistorySearchResult found = FindCompatibleHistory(current, camera, previous, true);
	Check(!found.found && found.tapsVisited == 1,
		"exact-search mode must reject an incompatible reprojected receiver");
	HistorySearchPattern neighborhood = {};
	neighborhood.probeCount = 2;
	neighborhood.borderMargin = 2;
	neighborhood.offsets[1] = {{ -1, 0 }};
	const HistorySearchResult recoveredNeighbor = FindCompatibleHistory(
		current, camera, previous, true, neighborhood);
	Check(recoveredNeighbor.found &&
		recoveredNeighbor.index == 4u * 8u + 3u &&
		recoveredNeighbor.tapsVisited == 2,
		"surface-only neighborhood search must recover the first compatible receiver");
	previous[4 * 8 + 4].surface = current;
	previous[4 * 8 + 4].surface.materialId = 99;
	previous[4 * 8 + 4].surface.roughness = 0.9;
	previous[4 * 8 + 4].reservoirSelected = false;
	previous[4 * 8 + 3].reservoirSelected = true;
	const HistorySearchResult centerWithoutSample =
		FindCompatibleHistory(current, camera, previous, true, neighborhood);
	Check(centerWithoutSample.found &&
		centerWithoutSample.index == 4u * 8u + 4u &&
		centerWithoutSample.tapsVisited == 1,
		"reservoir contents must not make search skip the first compatible surface");
	const HistorySearchResult rejected = FindCompatibleHistory(current, camera, previous, false);
	Check(!rejected.found && rejected.tapsVisited == 0,
		"uniform page rejection must prevent all per-pixel history reads");
	Surface leadingEdge = current;
	leadingEdge.worldPosition = { 10.0, 12.4, 0.0 };
	const Projection leadingProjection = ProjectToPrevious(leadingEdge, camera, 2);
	previous[4 * 8].surface = leadingEdge;
	previous[4 * 8].surface.previousViewDepth = leadingProjection.linearDepth;
	HistorySearchPattern edgePattern = {};
	edgePattern.probeCount = 2;
	edgePattern.borderMargin = 2;
	edgePattern.offsets[1] = {{ 1, 0 }};
	const HistorySearchResult edgeRecovery = FindCompatibleHistory(
		leadingEdge, camera, previous, true, edgePattern);
	Check(edgeRecovery.found && edgeRecovery.index == 4u * 8u &&
		edgeRecovery.tapsVisited == 1,
		"signed bounds rejection must allow an in-bounds probe to recover an off-screen center");
}

void TestTemporalRecoveryAndRejection() {
	constexpr uint32_t generation = 0x10203040u;
	LogicalReservoir empty = {};
	LogicalReservoir history = Finalized(17, generation, 3, 4.0);
	Candidate shifted = Positive(17, generation, 1.0);
	shifted.contribution = { 3.0, 2.0, 1.0 };
	const TemporalMergeResult recovered = MergeFinalizedTemporalCandidate(
		empty, history, shifted, true, true, true, 0.0, 0.5, generation);
	Check(recovered.historyAccepted && recovered.selectedHistory &&
		recovered.reservoir.hasSelectedSample &&
		recovered.reservoir.selected.identity0 == 17 &&
		recovered.reservoir.selected.contribution[0] == 3.0 &&
		recovered.reservoir.effectiveM == 3 && recovered.reservoir.selected.age == 1,
		"valid shifted history must recover an empty current reservoir in the current domain");
	Check(Near(recovered.reservoir.weightSum, 2.0),
		"history target change must scale finalized mean weight exactly once");

	LogicalReservoir current = Finalized(5, generation, 1, 6.0);
	current.selected.age = 0;
	const TemporalMergeResult stale = MergeFinalizedTemporalCandidate(
		current, history, shifted, false, true, true, 2.0, 0.0, generation);
	Check(!stale.historyAccepted && !stale.selectedHistory &&
		stale.reservoir.effectiveM == current.effectiveM &&
		stale.reservoir.weightSum == current.weightSum &&
		stale.reservoir.selected.identity0 == current.selected.identity0 &&
		stale.reservoir.selected.age == current.selected.age,
		"stale page rejection must leave current M, weight, identity, and age bit-identical");

	const TemporalMergeResult badShift = MergeFinalizedTemporalCandidate(
		current, history, shifted, true, true, false, 2.0, 0.0, generation);
	Check(!badShift.historyAccepted && badShift.reservoir.effectiveM == 1 &&
		badShift.reservoir.selected.age == 0,
		"failed shift/reconnection must not mutate current M or age");
}

void TestZeroTrialAndRescueState() {
	constexpr uint32_t generation = 0x10203040u;
	LogicalReservoir currentZero = {};
	currentZero.effectiveM = 1;
	LogicalReservoir history = Finalized(23, generation, 2, 3.0);
	Candidate shifted = Positive(23, generation);
	const TemporalMergeResult combined = MergeFinalizedTemporalCandidate(
		currentZero, history, shifted, true, true, true, 0.0, 0.75, generation);
	Check(combined.historyAccepted && combined.selectedHistory &&
		combined.reservoir.hasSelectedSample &&
		combined.reservoir.effectiveM == 3 && Near(combined.reservoir.weightSum, 2.0),
		"a defined current zero trial must remain in M when valid history is recovered");

	Candidate occludedShift = shifted;
	occludedShift.status = CandidateStatus::ValidZero;
	occludedShift.target = 0.0;
	occludedShift.contribution = { 0.0, 0.0, 0.0 };
	const TemporalMergeResult occluded = MergeFinalizedTemporalCandidate(
		currentZero, history, occludedShift, true, true, true, 0.0, 0.25, generation);
	Check(occluded.historyAccepted && !occluded.reservoir.hasSelectedSample &&
		occluded.reservoir.needsRescue && occluded.reservoir.effectiveM == 3 &&
		occluded.reservoir.weightSum == 0.0,
		"a valid-zero shifted history trial must add capped M without inventing weight");

	LogicalReservoir selectedCurrent = Finalized(31, generation, 1, 6.0);
	const TemporalMergeResult unsupportedHistory = MergeFinalizedTemporalCandidate(
		selectedCurrent, history, occludedShift, true, true, true, 0.0, 0.25, generation);
	Check(unsupportedHistory.historyAccepted &&
		!unsupportedHistory.selectedHistory &&
		unsupportedHistory.reservoir.hasSelectedSample &&
		unsupportedHistory.reservoir.selected.identity0 == 31 &&
		unsupportedHistory.reservoir.effectiveM == 3 &&
		Near(unsupportedHistory.reservoir.weightSum, 6.0),
		"history M must not attenuate a selected current sample outside its history-domain support");

	const TemporalMergeResult supportedHistory = MergeFinalizedTemporalCandidate(
		selectedCurrent, history, occludedShift, true, true, true, 2.0, 0.25, generation);
	Check(supportedHistory.historyAccepted &&
		supportedHistory.reservoir.hasSelectedSample &&
		supportedHistory.reservoir.effectiveM == 3 &&
		Near(supportedHistory.reservoir.weightSum, 2.0),
		"history M must enter 1/Z only when the selected current sample has history-domain support");

	LogicalReservoir empty = {};
	const TemporalMergeResult noHistory = MergeFinalizedTemporalCandidate(
		empty, LogicalReservoir{}, Candidate{}, true, false, false, 0.0, 0.0, generation);
	Check(!noHistory.reservoir.hasSelectedSample && noHistory.reservoir.needsRescue &&
		noHistory.reservoir.effectiveM == 0,
		"both empty sets must publish a local rescue request without inventing M");
	PackedReservoir packed = {};
	LogicalReservoir decoded = {};
	Check(PackReservoir(noHistory.reservoir, generation, packed) &&
		packed.words[0] == kPackedNeedsRescueBit &&
		UnpackReservoir(packed, generation, decoded) && decoded.needsRescue,
		"rescue state must survive the frozen 64-byte page ABI");
}

void TestStationaryRecurrenceDoesNotAccumulateEnergy() {
	constexpr uint32_t generation = 0x10203040u;
	LogicalReservoir history = Finalized(41, generation, 1, 4.0);
	for (uint32_t frame = 0; frame < 64; ++frame) {
		LogicalReservoir current = Finalized(41, generation, 1, 4.0);
		Candidate shifted = history.selected;
		shifted.target = 2.0;
		shifted.contribution = { 2.0, 1.0, 0.5 };
		const TemporalMergeResult merged = MergeFinalizedTemporalCandidate(
			current,
			history,
			shifted,
			true,
			true,
			true,
			2.0,
			(frame & 1u) != 0u ? 0.25 : 0.75,
			generation);
		Check(merged.historyAccepted && merged.reservoir.hasSelectedSample &&
			Near(merged.reservoir.weightSum, 4.0),
			"stationary temporal recurrence must preserve the D0 estimate instead of stacking energy");
		history = merged.reservoir;
	}
}

void TestSaturatedAgeRemainsReusable() {
	constexpr uint32_t generation = 0x10203040u;
	LogicalReservoir current = Finalized(41, generation, 1, 4.0);
	LogicalReservoir history = Finalized(41, generation, 32, 4.0);
	history.selected.age = 63;
	Candidate shifted = history.selected;
	const TemporalMergeResult merged = MergeFinalizedTemporalCandidate(
		current, history, shifted, true, true, true, 2.0, 0.0, generation);
	Check(merged.historyAccepted && merged.selectedHistory &&
		merged.reservoir.selected.age == 63,
		"six-bit age 63 must saturate rather than expire coherent history");
}

void TestPreviousBestBecomesCurrentProposal() {
	constexpr uint32_t generation = 0x10203040u;
	Candidate shifted = Positive(71, generation, 3.0);
	shifted.age = 27;
	const LogicalReservoir seededEmpty = InjectPreviousBestSeed(
		LogicalReservoir{}, shifted, true, 0.5, generation);
	Check(seededEmpty.hasSelectedSample && seededEmpty.effectiveM == 1u &&
		seededEmpty.selected.identity0 == 71u && seededEmpty.selected.age == 0u &&
		Near(seededEmpty.weightSum, 1.0),
		"previous-best alone must become one age-zero current proposal with finalized W=1");

	LogicalReservoir current = Finalized(72, generation, 1u, 2.0);
	current.selected.target = 2.0;
	const LogicalReservoir choseSeed = InjectPreviousBestSeed(
		current, shifted, true, 0.0, generation);
	Check(choseSeed.hasSelectedSample && choseSeed.effectiveM == 1u &&
		choseSeed.selected.identity0 == 71u && choseSeed.selected.age == 0u &&
		Near(choseSeed.weightSum, 7.0 / 6.0),
		"previous-best combination must finalize two source proposals before collapsing M to one");

	const LogicalReservoir choseCurrent = InjectPreviousBestSeed(
		current, shifted, true, 0.99, generation);
	Check(choseCurrent.selected.identity0 == 72u && choseCurrent.selected.age == 0u &&
		choseCurrent.effectiveM == 1u && Near(choseCurrent.weightSum, 7.0 / 4.0),
		"fresh D0 may remain selected while still absorbing previous-best proposal mass");
}

} // namespace

int main() {
	TestUniformPageAdmission();
	TestRoleFlipFollowsCurrentFramePublication();
	TestProjectionAndBoundedSearch();
	TestTemporalRecoveryAndRejection();
	TestZeroTrialAndRescueState();
	TestStationaryRecurrenceDoesNotAccumulateEnergy();
	TestSaturatedAgeRemainsReusable();
	TestPreviousBestBecomesCurrentProposal();

	if (gFailures != 0) {
		std::cerr << "UPT-07 temporal contract tests failed: " << gFailures << '\n';
		return EXIT_FAILURE;
	}
	std::cout << "UPT-07 temporal contract tests passed; pages=2 taps="
		<< kMaximumHistoryProbeCount << " winnerVisibilityRaysMax=1\n";
	return EXIT_SUCCESS;
}
