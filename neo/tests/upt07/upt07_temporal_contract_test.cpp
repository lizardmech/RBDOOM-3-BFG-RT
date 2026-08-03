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
	PageMetadata page = { true, 8, 8, 0x1122334455667788ull, 9 };
	Check(AdmitHistoryPage(page, 8, 8, page.contentGeneration, 9),
		"matching page metadata must admit history uniformly");
	PageMetadata stale = page;
	stale.contentGeneration ^= 1u;
	Check(!AdmitHistoryPage(stale, 8, 8, page.contentGeneration, 9),
		"generation mismatch must reject the page before per-pixel reads");
	stale = page;
	stale.historyEpoch = 8;
	Check(!AdmitHistoryPage(stale, 8, 8, page.contentGeneration, 9),
		"history epoch mismatch must reject the whole page");
	stale = page;
	stale.fullyWritten = false;
	Check(!AdmitHistoryPage(stale, 8, 8, page.contentGeneration, 9),
		"partial proof-stage output must never become history");
}

void TestRoleFlipOnlyAfterCompletion() {
	PageRoles roles = {};
	Check(roles.current == 0 && roles.history == 1,
		"two-page schedule must start with page 0 current and page 1 history");
	const PageRoles aborted = roles;
	Check(aborted.current == 0 && aborted.history == 1,
		"an aborted frame has no implicit role flip");
	roles.CommitCompletedTemporalFrame();
	Check(roles.current == 1 && roles.history == 0,
		"a completed temporal frame must flip physical roles exactly once");
	roles.CommitCompletedTemporalFrame();
	Check(roles.current == 0 && roles.history == 1,
		"two completed frames must return to the original physical roles");
}

void TestProjectionAndBoundedSearch() {
	const Surface current = TestSurface();
	const PreviousCamera camera = TestCamera();
	const Projection projection = ProjectToPrevious(current, camera);
	Check(projection.valid && projection.pixelFloorX == 4 && projection.pixelFloorY == 4 &&
		Near(projection.viewDepth, 10.0),
		"static receiver must reproject to the previous center pixel");

	std::vector<HistoryPixel> previous(64);
	previous[4 * 8 + 4].surface = current;
	previous[4 * 8 + 4].surface.materialId = 99;
	previous[4 * 8 + 4].reservoirSelected = true;
	previous[4 * 8 + 3].surface = current;
	previous[4 * 8 + 3].reservoirSelected = true;
	const HistorySearchResult found = FindCompatibleHistory(current, camera, previous, true);
	Check(found.found && found.index == 4u * 8u + 3u && found.tapsVisited == 2,
		"bounded search must skip an incompatible center and accept a compatible fallback tap");
	const HistorySearchResult rejected = FindCompatibleHistory(current, camera, previous, false);
	Check(!rejected.found && rejected.tapsVisited == 0,
		"uniform page rejection must prevent all per-pixel history reads");
	Check(kHistoryTapOffsets.size() == 5,
		"the first temporal neighborhood must remain compile-time bounded to five taps");
}

void TestTemporalRecoveryAndRejection() {
	constexpr uint32_t generation = 0x10203040u;
	LogicalReservoir empty = {};
	LogicalReservoir history = Finalized(17, generation, 3, 4.0);
	Candidate shifted = Positive(17, generation, 1.0);
	shifted.contribution = { 3.0, 2.0, 1.0 };
	const TemporalMergeResult recovered = MergeFinalizedTemporalCandidate(
		empty, history, shifted, true, true, true, 0.5, generation);
	Check(recovered.historyAccepted && recovered.reservoir.hasSelectedSample &&
		recovered.reservoir.selected.identity0 == 17 &&
		recovered.reservoir.selected.contribution[0] == 3.0 &&
		recovered.reservoir.effectiveM == 3 && recovered.reservoir.selected.age == 1,
		"valid shifted history must recover an empty current reservoir in the current domain");
	Check(Near(recovered.reservoir.weightSum, 2.0),
		"history target change must scale finalized mean weight exactly once");

	LogicalReservoir current = Finalized(5, generation, 1, 6.0);
	current.selected.age = 0;
	const TemporalMergeResult stale = MergeFinalizedTemporalCandidate(
		current, history, shifted, false, true, true, 0.0, generation);
	Check(!stale.historyAccepted && stale.reservoir.effectiveM == current.effectiveM &&
		stale.reservoir.weightSum == current.weightSum &&
		stale.reservoir.selected.identity0 == current.selected.identity0 &&
		stale.reservoir.selected.age == current.selected.age,
		"stale page rejection must leave current M, weight, identity, and age bit-identical");

	const TemporalMergeResult badShift = MergeFinalizedTemporalCandidate(
		current, history, shifted, true, true, false, 0.0, generation);
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
		currentZero, history, shifted, true, true, true, 0.75, generation);
	Check(combined.historyAccepted && combined.reservoir.hasSelectedSample &&
		combined.reservoir.effectiveM == 3 && Near(combined.reservoir.weightSum, 2.0),
		"a defined current zero trial must remain in M when valid history is recovered");

	Candidate occludedShift = shifted;
	occludedShift.status = CandidateStatus::ValidZero;
	occludedShift.target = 0.0;
	occludedShift.contribution = { 0.0, 0.0, 0.0 };
	const TemporalMergeResult occluded = MergeFinalizedTemporalCandidate(
		currentZero, history, occludedShift, true, true, true, 0.25, generation);
	Check(occluded.historyAccepted && !occluded.reservoir.hasSelectedSample &&
		occluded.reservoir.needsRescue && occluded.reservoir.effectiveM == 3 &&
		occluded.reservoir.weightSum == 0.0,
		"a valid-zero shifted history trial must add capped M without inventing weight");

	LogicalReservoir selectedCurrent = Finalized(31, generation, 1, 6.0);
	const TemporalMergeResult attenuated = MergeFinalizedTemporalCandidate(
		selectedCurrent, history, occludedShift, true, true, true, 0.25, generation);
	Check(attenuated.historyAccepted && attenuated.reservoir.hasSelectedSample &&
		attenuated.reservoir.selected.identity0 == 31 &&
		attenuated.reservoir.effectiveM == 3 && Near(attenuated.reservoir.weightSum, 2.0),
		"valid-zero shifted history must stay in the denominator of a positive current result");

	LogicalReservoir empty = {};
	const TemporalMergeResult noHistory = MergeFinalizedTemporalCandidate(
		empty, LogicalReservoir{}, Candidate{}, true, false, false, 0.0, generation);
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

} // namespace

int main() {
	TestUniformPageAdmission();
	TestRoleFlipOnlyAfterCompletion();
	TestProjectionAndBoundedSearch();
	TestTemporalRecoveryAndRejection();
	TestZeroTrialAndRescueState();

	if (gFailures != 0) {
		std::cerr << "UPT-07 temporal contract tests failed: " << gFailures << '\n';
		return EXIT_FAILURE;
	}
	std::cout << "UPT-07 temporal contract tests passed; pages=2 taps="
		<< kHistoryTapOffsets.size() << " historyRays=shift-provider-only\n";
	return EXIT_SUCCESS;
}
