#pragma once

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace rb::upt10 {

constexpr uint32_t Invalid = 0u;
constexpr uint32_t ValidZero = 1u;
constexpr uint32_t ValidPositive = 2u;
constexpr uint32_t NoSelection = 0xffffffffu;
constexpr uint32_t PairCount = 8u;

struct Proposal {
	uint32_t status = Invalid;
	uint32_t routeLabel = 0u;
	uint32_t sourceConfidence = 0u;
	uint32_t reserved0 = 0u;
	float sourceScaledWeight = 0.0f;
	float sourceTarget = 0.0f;
	float targetTarget = 0.0f;
	float shiftJacobian = 0.0f;
	float pairwiseMis = 0.0f;
	float reserved1 = 0.0f;
	float reserved2 = 0.0f;
	float reserved3 = 0.0f;
};

struct Control {
	float winnerRandom = 0.0f;
	uint32_t reserved0 = 0u;
	uint32_t reserved1 = 0u;
	uint32_t reserved2 = 0u;
};

struct Result {
	float totalMappedMass = 0.0f;
	uint32_t selectedOrdinal = NoSelection;
	uint32_t outputConfidence = 0u;
	uint32_t validMask = 0u;
};

static_assert(sizeof(Proposal) == 48, "UPT-30 proposal ABI drifted");
static_assert(sizeof(Control) == 16, "UPT-30 control ABI drifted");
static_assert(sizeof(Result) == 16, "UPT-30 result ABI drifted");

inline bool FiniteNonNegative(float value) {
	return std::isfinite(value) && value >= 0.0f;
}

inline bool FinitePositive(float value) {
	return std::isfinite(value) && value > 0.0f;
}

struct Mapped {
	float mass = 0.0f;
	uint32_t confidence = 0u;
	uint32_t valid = 0u;
};

inline Mapped Map(const Proposal& input) {
	Mapped result;
	if (input.status != ValidZero && input.status != ValidPositive) return result;
	if (!FiniteNonNegative(input.sourceScaledWeight) ||
		!FiniteNonNegative(input.sourceTarget) ||
		!FiniteNonNegative(input.targetTarget) ||
		!FinitePositive(input.shiftJacobian) ||
		!FiniteNonNegative(input.pairwiseMis)) return result;
	if (input.status == ValidZero) {
		if (input.targetTarget != 0.0f) return result;
		result.confidence = input.sourceConfidence;
		result.valid = 1u;
		return result;
	}
	if (!FinitePositive(input.sourceTarget) ||
		!FinitePositive(input.targetTarget)) return result;
	const float sourceUcw = input.sourceScaledWeight / input.sourceTarget;
	const float mass = input.pairwiseMis * input.targetTarget * sourceUcw * input.shiftJacobian;
	if (!FiniteNonNegative(sourceUcw) || !FiniteNonNegative(mass)) return result;
	result.mass = mass;
	result.confidence = input.sourceConfidence;
	result.valid = 1u;
	return result;
}

inline uint32_t SaturatingAdd(uint32_t a, uint32_t b) {
	return a > std::numeric_limits<uint32_t>::max() - b
		? std::numeric_limits<uint32_t>::max() : a + b;
}

inline Result Merge(const Proposal& a, const Proposal& b, const Control& control) {
	const Mapped mappedA = Map(a);
	const Mapped mappedB = Map(b);
	Result result;
	result.totalMappedMass = mappedA.mass + mappedB.mass;
	result.outputConfidence = SaturatingAdd(mappedA.confidence, mappedB.confidence);
	result.validMask = mappedA.valid | (mappedB.valid << 1u);
	if (!FinitePositive(result.totalMappedMass)) return result;
	const float random01 = control.winnerRandom < 0.0f ? 0.0f
		: (control.winnerRandom > 0.99999994f ? 0.99999994f : control.winnerRandom);
	result.selectedOrdinal = random01 * result.totalMappedMass < mappedA.mass ? 0u : 1u;
	return result;
}

inline Proposal Positive(uint32_t route, uint32_t confidence, float scaledWeight,
	float sourceTarget, float targetTarget, float jacobian, float mis) {
	Proposal result;
	result.status = ValidPositive;
	result.routeLabel = route;
	result.sourceConfidence = confidence;
	result.sourceScaledWeight = scaledWeight;
	result.sourceTarget = sourceTarget;
	result.targetTarget = targetTarget;
	result.shiftJacobian = jacobian;
	result.pairwiseMis = mis;
	return result;
}

struct Corpus {
	std::array<Proposal, PairCount * 2u> proposals = {};
	std::array<Control, PairCount> controls = {};
};

inline Corpus MakeCorpus() {
	Corpus corpus;
	const Proposal direct = Positive(1u, 3u, 8.0f, 4.0f, 6.0f, 0.5f, 0.25f);
	const Proposal global = Positive(2u, 7u, 2.0f, 2.0f, 5.0f, 2.0f, 0.5f);
	auto set = [&](uint32_t pair, Proposal a, Proposal b, float random) {
		corpus.proposals[pair * 2u] = a;
		corpus.proposals[pair * 2u + 1u] = b;
		corpus.controls[pair].winnerRandom = random;
	};

	set(0u, direct, global, 0.2f);
	Proposal relabeledDirect = direct;
	Proposal relabeledGlobal = global;
	relabeledDirect.routeLabel = 2u;
	relabeledGlobal.routeLabel = 1u;
	set(1u, relabeledDirect, relabeledGlobal, 0.2f);

	Proposal invalid = direct;
	invalid.status = Invalid;
	set(2u, invalid, global, 0.0f);

	Proposal zero = direct;
	zero.status = ValidZero;
	zero.sourceScaledWeight = 0.0f;
	zero.sourceTarget = 0.0f;
	zero.targetTarget = 0.0f;
	set(3u, zero, invalid, 0.5f);

	Proposal overflow = direct;
	overflow.sourceScaledWeight = std::numeric_limits<float>::max();
	overflow.targetTarget = std::numeric_limits<float>::max();
	set(4u, overflow, invalid, 0.5f);

	Proposal highConfidence = direct;
	highConfidence.sourceConfidence = std::numeric_limits<uint32_t>::max() - 1u;
	set(5u, highConfidence, global, 0.99f);
	set(6u, direct, global, 0.95f);

	Proposal malformedZero = zero;
	malformedZero.targetTarget = 1.0f;
	Proposal nanSource = global;
	nanSource.sourceTarget = std::numeric_limits<float>::quiet_NaN();
	set(7u, malformedZero, nanSource, 0.5f);
	return corpus;
}

inline std::array<Result, PairCount> MakeExpected(const Corpus& corpus) {
	std::array<Result, PairCount> result = {};
	for (uint32_t pair = 0u; pair < PairCount; ++pair) {
		result[pair] = Merge(corpus.proposals[pair * 2u],
			corpus.proposals[pair * 2u + 1u], corpus.controls[pair]);
	}
	return result;
}

inline void Require(bool condition, const char* message) {
	if (!condition) throw std::runtime_error(message);
}

inline void ValidateExpected(const std::array<Result, PairCount>& values) {
	Require(values[0].totalMappedMass == 6.5f, "Jacobian/UCW mass conversion failed");
	Require(values[0].selectedOrdinal == 0u && values[0].outputConfidence == 10u &&
		values[0].validMask == 3u, "positive pair merge failed");
	Require(std::memcmp(&values[0], &values[1], sizeof(Result)) == 0,
		"route label changed estimator output");
	Require(values[2].totalMappedMass == 5.0f && values[2].selectedOrdinal == 1u &&
		values[2].outputConfidence == 7u && values[2].validMask == 2u,
		"invalid shift contributed mass or confidence");
	Require(values[3].totalMappedMass == 0.0f && values[3].selectedOrdinal == NoSelection &&
		values[3].outputConfidence == 3u && values[3].validMask == 1u,
		"valid-zero policy failed");
	Require(values[4].totalMappedMass == 0.0f && values[4].outputConfidence == 0u,
		"non-finite mapped mass was admitted");
	Require(values[5].outputConfidence == std::numeric_limits<uint32_t>::max(),
		"confidence addition did not saturate");
	Require(values[6].selectedOrdinal == 1u, "second proposal selection failed");
	Require(values[7].validMask == 0u && values[7].outputConfidence == 0u,
		"malformed mapping input was admitted");
}

} // namespace rb::upt10
