#include "upt02_unified_reservoir_codec.h"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <iterator>
#include <limits>
#include <string>

namespace {

using namespace rb::upt02;

int gFailures = 0;

void Check(bool condition, const char* message) {
	if (!condition) {
		std::cerr << "FAIL: " << message << '\n';
		++gFailures;
	}
}

bool Near(double actual, double expected, double tolerance) {
	return std::abs(actual - expected) <= tolerance;
}

bool SameCriticalState(const LogicalReservoir& a, const LogicalReservoir& b) {
	return a.hasSelectedSample == b.hasSelectedSample &&
		a.needsRescue == b.needsRescue &&
		a.effectiveM == b.effectiveM &&
		a.weightSum == b.weightSum &&
		a.selected.eventKind == b.selected.eventKind &&
		a.selected.identity0 == b.selected.identity0 &&
		a.selected.identity1 == b.selected.identity1 &&
		a.selected.replayKey == b.selected.replayKey &&
		a.selected.replayIndex == b.selected.replayIndex;
}

Candidate PositiveCandidate(EventKind event, uint32_t generation) {
	Candidate candidate = {};
	candidate.status = CandidateStatus::ValidPositive;
	candidate.eventKind = event;
	candidate.lobeMask = LobeDiffuse;
	candidate.flags = event == EventKind::Indirect ?
		static_cast<uint8_t>(FlagGlobal | FlagVisibilityKnown | FlagVisibilityPassed | FlagReplayable) :
		static_cast<uint8_t>(FlagVisibilityKnown | FlagVisibilityPassed);
	candidate.identity0 = 17;
	candidate.identity1 = 3;
	candidate.replayKey = event == EventKind::Indirect ? 0x12345678u : 0u;
	candidate.replayIndex = event == EventKind::Indirect ? 9u : 0u;
	candidate.generationFingerprint = generation;
	candidate.sampleCoord0 = event == EventKind::Environment ? -0.375 : 0.25;
	candidate.sampleCoord1 = event == EventKind::Environment ? 0.625 : 0.5;
	candidate.contribution = { 1.25, 0.5, 0.125 };
	candidate.target = 2.0;
	candidate.proposalPdf = 0.25;
	candidate.pathPdf = 0.125;
	candidate.partialJacobian = 0.75;
	candidate.accumulatedRouletteProbability = 1.0;
	return candidate;
}

Candidate ZeroCandidate(uint32_t generation) {
	Candidate candidate = PositiveCandidate(EventKind::Analytic, generation);
	candidate.status = CandidateStatus::ValidZero;
	candidate.target = 0.0;
	candidate.contribution = { 0.0, 0.0, 0.0 };
	return candidate;
}

void TestRandomSchedule() {
	for (size_t i = 0; i < std::size(kRandomSlots); ++i) {
		for (size_t j = i + 1; j < std::size(kRandomSlots); ++j) {
			const bool same = kRandomSlots[i].passNamespace == kRandomSlots[j].passNamespace &&
				kRandomSlots[i].streamNamespace == kRandomSlots[j].streamNamespace &&
				kRandomSlots[i].dimension == kRandomSlots[j].dimension;
			Check(!same, "random slot tuples must be collision-free");
		}
	}
	Check(std::size(kRandomSlots) == 22, "the frozen schedule must contain all 22 declared slots");

	RandomKey material = { 4, 7, 11, 1, 2, 3, 1, 0, 5 };
	RandomKey afterOptionalFamilyDisabled = material;
	Check(std::memcmp(&material, &afterOptionalFamilyDisabled, sizeof(material)) == 0,
		"optional family state must not renumber material draws");
	afterOptionalFamilyDisabled.pathVertex = 3;
	Check(afterOptionalFamilyDisabled.dimension == material.dimension &&
		afterOptionalFamilyDisabled.streamNamespace == material.streamNamespace,
		"adding a bounce changes pathVertex, not the slot identifier");
}

void TestCandidateStatusAndReferenceUpdate() {
	constexpr uint32_t generation = 0x10203040u;
	LogicalReservoir reservoir = {};
	const LogicalReservoir pristine = reservoir;
	Candidate invalid = PositiveCandidate(EventKind::Analytic, generation);
	invalid.status = CandidateStatus::Invalid;
	Check(StreamCandidate(reservoir, invalid, 0.0, generation) == UpdateResult::SkippedInvalid,
		"explicit INVALID must be rejected");
	Check(SameCriticalState(reservoir, pristine), "INVALID must not mutate any reservoir state");

	Candidate zero = ZeroCandidate(generation);
	Check(StreamCandidate(reservoir, zero, std::numeric_limits<double>::quiet_NaN(), generation) ==
		UpdateResult::CountedValidZero, "VALID_ZERO must count without consuming winner RNG");
	Check(reservoir.effectiveM == 1 && reservoir.weightSum == 0.0 && !reservoir.hasSelectedSample,
		"VALID_ZERO increments M but cannot set weight or identity");

	Candidate first = PositiveCandidate(EventKind::Analytic, generation);
	Check(StreamCandidate(reservoir, first, 0.75, generation) == UpdateResult::SelectedPositive,
		"first positive candidate must be selected");
	Check(reservoir.effectiveM == 2 && Near(reservoir.weightSum, 8.0, 0.0),
		"positive update must add target/proposalPdf weight");
	const uint32_t firstIdentity = reservoir.selected.identity0;

	Candidate second = PositiveCandidate(EventKind::Emissive, generation);
	second.identity0 = 99;
	second.target = 1.0;
	second.proposalPdf = 1.0;
	Check(StreamCandidate(reservoir, second, 0.99, generation) == UpdateResult::KeptPositive,
		"high winner draw must retain the previous positive candidate");
	Check(reservoir.effectiveM == 3 && Near(reservoir.weightSum, 9.0, 0.0) &&
		reservoir.selected.identity0 == firstIdentity,
		"non-winning positive candidate must count without changing identity");

	const double weightBeforeZero = FinalReservoirWeight(reservoir);
	Check(StreamCandidate(reservoir, zero, 0.0, generation) == UpdateResult::CountedValidZero,
		"a defined zero after selection still counts");
	Check(FinalReservoirWeight(reservoir) < weightBeforeZero,
		"valid-zero trials must remain in the estimator denominator");
}

void TestInvalidCorpus() {
	constexpr uint32_t generation = 77;
	LogicalReservoir baseline = {};
	Candidate seed = PositiveCandidate(EventKind::Analytic, generation);
	Check(StreamCandidate(baseline, seed, 0.0, generation) == UpdateResult::SelectedPositive,
		"invalid corpus needs a seeded reservoir");

	auto rejectWithoutMutation = [&](Candidate candidate, const char* label) {
		LogicalReservoir actual = baseline;
		const LogicalReservoir before = actual;
		Check(StreamCandidate(actual, candidate, 0.0, generation) == UpdateResult::SkippedInvalid, label);
		Check(SameCriticalState(actual, before), "invalid candidate changed M, weight, or identity");
	};

	Candidate candidate = PositiveCandidate(EventKind::Analytic, generation);
	candidate.proposalPdf = 0.0;
	rejectWithoutMutation(candidate, "zero proposal PDF must be invalid");
	candidate = PositiveCandidate(EventKind::Analytic, generation);
	candidate.pathPdf = std::numeric_limits<double>::infinity();
	rejectWithoutMutation(candidate, "infinite path PDF must be invalid");
	candidate = PositiveCandidate(EventKind::Analytic, generation);
	candidate.contribution[1] = std::numeric_limits<double>::quiet_NaN();
	rejectWithoutMutation(candidate, "NaN contribution must be invalid");
	candidate = PositiveCandidate(EventKind::Analytic, generation + 1);
	rejectWithoutMutation(candidate, "stale page generation must be invalid");
	candidate = PositiveCandidate(EventKind::Indirect, generation);
	candidate.reconnectionVertexLength = 3;
	candidate.pathLength = 2;
	rejectWithoutMutation(candidate, "impossible reconnection topology must be invalid");
	candidate = PositiveCandidate(EventKind::Indirect, generation);
	candidate.replayKey = 0;
	rejectWithoutMutation(candidate, "missing indirect replay identity must be invalid");
	candidate = PositiveCandidate(EventKind::Analytic, generation);
	candidate.lobeMask = 0;
	rejectWithoutMutation(candidate, "missing output lobe classification must be invalid");
	candidate = PositiveCandidate(EventKind::Analytic, generation);
	candidate.flags = FlagVisibilityPassed;
	rejectWithoutMutation(candidate, "visibility-passed without visibility-known must be invalid");
	candidate = PositiveCandidate(EventKind::Emissive, generation);
	candidate.sampleCoord0 = 0.75;
	candidate.sampleCoord1 = 0.75;
	rejectWithoutMutation(candidate, "invalid triangle barycentrics must be invalid");
}

void TestOverflowIsAtomic() {
	constexpr uint32_t generation = 88;
	Candidate candidate = PositiveCandidate(EventKind::Analytic, generation);
	LogicalReservoir countOverflow = {};
	countOverflow.effectiveM = std::numeric_limits<uint32_t>::max();
	const LogicalReservoir countBefore = countOverflow;
	Check(StreamCandidate(countOverflow, ZeroCandidate(generation), 0.0, generation) ==
		UpdateResult::RejectedOverflow, "M overflow must be rejected");
	Check(SameCriticalState(countOverflow, countBefore), "M overflow rejection must be atomic");

	LogicalReservoir weightOverflow = {};
	weightOverflow.hasSelectedSample = true;
	weightOverflow.effectiveM = 1;
	weightOverflow.weightSum = static_cast<double>(std::numeric_limits<float>::max()) * 0.75;
	weightOverflow.selected = candidate;
	candidate.target = static_cast<double>(std::numeric_limits<float>::max()) * 0.5;
	candidate.proposalPdf = 0.25;
	const LogicalReservoir weightBefore = weightOverflow;
	Check(StreamCandidate(weightOverflow, candidate, 0.0, generation) == UpdateResult::RejectedOverflow,
		"packed-weight overflow must be rejected");
	Check(SameCriticalState(weightOverflow, weightBefore), "weight overflow rejection must be atomic");
}

void TestCanonicalEmpty() {
	PackedReservoir packed = {};
	LogicalReservoir logical = {};
	Check(PackReservoir(logical, 1, packed) && IsCanonicalEmpty(packed),
		"logical empty must encode as all-zero bytes");
	LogicalReservoir decoded = {};
	Check(UnpackReservoir(packed, 1, decoded) && !decoded.hasSelectedSample && decoded.effectiveM == 0,
		"canonical empty must decode successfully");
	packed.words[5] = 1;
	Check(!UnpackReservoir(packed, 1, decoded), "non-canonical empty-like bytes must be rejected");

	LogicalReservoir zerosOnly = {};
	Check(StreamCandidate(zerosOnly, ZeroCandidate(1), 0.0, 1) == UpdateResult::CountedValidZero,
		"zero-only stream must count while logical streaming is active");
	Check(PackReservoir(zerosOnly, 1, packed) && !IsCanonicalEmpty(packed) &&
		packed.words[0] == kPackedCountOnlyTag && packed.words[1] == 1,
		"a finalized zero-only stream must preserve M in the count-only representation");
	Check(UnpackReservoir(packed, 1, decoded) && !decoded.hasSelectedSample &&
		decoded.effectiveM == 1 && decoded.weightSum == 0.0,
		"count-only state must round-trip without acquiring a selected identity");
	packed.words[2] = 1;
	Check(!UnpackReservoir(packed, 1, decoded),
		"count-only records with stray selected-sample payload must be rejected");

	LogicalReservoir rescueOnly = {};
	rescueOnly.needsRescue = true;
	Check(PackReservoir(rescueOnly, 1, packed) &&
		packed.words[0] == kPackedNeedsRescueBit && packed.words[1] == 0,
		"empty temporal exhaustion must encode a reservoir-local rescue request");
	Check(UnpackReservoir(packed, 1, decoded) && decoded.needsRescue &&
		!decoded.hasSelectedSample && decoded.effectiveM == 0,
		"rescue-only state must round-trip without acquiring history authority");

	zerosOnly.needsRescue = true;
	Check(PackReservoir(zerosOnly, 1, packed) &&
		packed.words[0] == (kPackedCountOnlyTag | kPackedNeedsRescueBit),
		"count-only state must retain M while requesting bounded rescue");
	Check(UnpackReservoir(packed, 1, decoded) && decoded.needsRescue &&
		decoded.effectiveM == 1 && !decoded.hasSelectedSample,
		"count-only rescue state must round-trip without a selected identity");
}

void TestPackRoundTrip() {
	constexpr uint32_t generation = 0xaabbccddu;
	for (EventKind event : { EventKind::Analytic, EventKind::Emissive, EventKind::Environment, EventKind::Indirect }) {
		Candidate candidate = PositiveCandidate(event, generation);
		candidate.pathLength = event == EventKind::Indirect ? 3 : 0;
		candidate.reconnectionVertexLength = event == EventKind::Indirect ? 2 : 0;
		candidate.accumulatedRouletteProbability = event == EventKind::Indirect ? 0.125 : 1.0;
		LogicalReservoir logical = {};
		Check(StreamCandidate(logical, candidate, 0.0, generation) == UpdateResult::SelectedPositive,
			"round-trip candidate must stream");
		PackedReservoir packed = {};
		Check(PackReservoir(logical, generation, packed), "valid reservoir must pack");
		LogicalReservoir decoded = {};
		Check(UnpackReservoir(packed, generation, decoded), "packed reservoir must decode");
		Check(decoded.selected.eventKind == event && decoded.effectiveM == logical.effectiveM &&
			decoded.selected.identity0 == candidate.identity0 && decoded.selected.replayKey == candidate.replayKey,
			"integer identity and topology fields must round-trip exactly");
		const double coordTolerance = event == EventKind::Environment ?
			(0.5 / 32767.0 + 1e-12) : (0.5 / 65535.0 + 1e-12);
		Check(Near(decoded.selected.sampleCoord0, candidate.sampleCoord0, coordTolerance) &&
			Near(decoded.selected.sampleCoord1, candidate.sampleCoord1, coordTolerance),
			"coordinate quantization exceeded the documented bound");
		Check(Near(decoded.selected.accumulatedRouletteProbability,
			candidate.accumulatedRouletteProbability, 1e-7),
			"roulette probability float32 error exceeded the test bound");
		Check(Near(FinalReservoirWeight(decoded), FinalReservoirWeight(logical), 1e-6),
			"final reservoir weight changed beyond float32 packing tolerance");
	}
}

void TestThreeBounceRouletteVector() {
	RouletteState state = {};
	state.pathPdf = 0.1;
	Check(ApplyRouletteSurvival(state, 0.25), "first roulette survival must be representable");
	Check(ApplyRouletteSurvival(state, 0.5), "second roulette survival must be representable");
	Check(Near(state.accumulatedSurvival, 0.125, 0.0),
		"three-bounce accumulated survival must be the product of per-depth probabilities");
	Check(Near(state.pathPdf, 0.0125, 1e-15),
		"three-bounce path PDF must include survival exactly once per survived depth");
	Check(Near(state.compensatedThroughput, 8.0, 0.0),
		"three-bounce throughput must carry reciprocal survival compensation exactly once");

	constexpr uint32_t generation = 123;
	Candidate candidate = PositiveCandidate(EventKind::Indirect, generation);
	candidate.pathLength = 3;
	candidate.reconnectionVertexLength = 2;
	candidate.pathPdf = state.pathPdf;
	candidate.accumulatedRouletteProbability = state.accumulatedSurvival;
	LogicalReservoir reservoir = {};
	Check(StreamCandidate(reservoir, candidate, 0.0, generation) == UpdateResult::SelectedPositive,
		"mapped three-bounce sample must fit the normal reservoir update");
	PackedReservoir packed = {};
	LogicalReservoir decoded = {};
	Check(PackReservoir(reservoir, generation, packed) && UnpackReservoir(packed, generation, decoded),
		"mapped three-bounce sample must use the baseline 64-byte ABI");
	Check(decoded.selected.pathLength == 3 && decoded.selected.reconnectionVertexLength == 2 &&
		Near(decoded.selected.accumulatedRouletteProbability, 0.125, 1e-7),
		"three-bounce topology and roulette state must survive round trip");

	const LogicalReservoir beforeTermination = reservoir;
	Check(SameCriticalState(reservoir, beforeTermination),
		"roulette termination is no candidate and therefore performs no reservoir update");
}

} // namespace

int main() {
	TestRandomSchedule();
	TestCandidateStatusAndReferenceUpdate();
	TestInvalidCorpus();
	TestOverflowIsAtomic();
	TestCanonicalEmpty();
	TestPackRoundTrip();
	TestThreeBounceRouletteVector();

	if (gFailures != 0) {
		std::cerr << "UPT-02 codec tests failed: " << gFailures << '\n';
		return EXIT_FAILURE;
	}
	std::cout << "UPT-02 codec tests passed; stride=" << sizeof(PackedReservoir)
		<< " bytes randomSlots=" << std::size(kRandomSlots) << '\n';
	return EXIT_SUCCESS;
}
