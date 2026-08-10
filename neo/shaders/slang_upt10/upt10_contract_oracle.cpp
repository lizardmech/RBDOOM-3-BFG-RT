#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {

constexpr uint32_t kInvalid = 0u;
constexpr uint32_t kValidZero = 1u;
constexpr uint32_t kValidPositive = 2u;
constexpr uint32_t kNoSelection = 0xffffffffu;

struct Proposal {
	uint32_t status;
	uint32_t routeLabel;
	uint32_t sourceConfidence;
	uint32_t reserved0;
	float sourceScaledWeight;
	float sourceTarget;
	float targetTarget;
	float shiftJacobian;
	float pairwiseMis;
	float reserved1;
	float reserved2;
	float reserved3;
};

struct Result {
	float totalMappedMass;
	uint32_t selectedOrdinal;
	uint32_t outputConfidence;
	uint32_t validMask;
};

static_assert(sizeof(Proposal) == 48, "UPT-30 proposal ABI drifted");
static_assert(sizeof(Result) == 16, "UPT-30 result ABI drifted");

bool FiniteNonNegative(float value) {
	return std::isfinite(value) && value >= 0.0f;
}

bool FinitePositive(float value) {
	return std::isfinite(value) && value > 0.0f;
}

struct Mapped {
	float mass = 0.0f;
	uint32_t confidence = 0u;
	uint32_t valid = 0u;
};

Mapped Map(const Proposal& input) {
	Mapped result;
	if (input.status != kValidZero && input.status != kValidPositive) return result;
	if (!FiniteNonNegative(input.sourceScaledWeight) ||
		!FinitePositive(input.sourceTarget) ||
		!FiniteNonNegative(input.targetTarget) ||
		!FinitePositive(input.shiftJacobian) ||
		!FiniteNonNegative(input.pairwiseMis)) return result;
	if ((input.status == kValidZero && input.targetTarget != 0.0f) ||
		(input.status == kValidPositive && input.targetTarget <= 0.0f)) return result;
	const float sourceUcw = input.sourceScaledWeight / input.sourceTarget;
	const float mass = input.pairwiseMis * input.targetTarget * sourceUcw * input.shiftJacobian;
	if (!FiniteNonNegative(sourceUcw) || !FiniteNonNegative(mass)) return result;
	result.mass = mass;
	result.confidence = input.sourceConfidence;
	result.valid = 1u;
	return result;
}

uint32_t SaturatingAdd(uint32_t a, uint32_t b) {
	return a > std::numeric_limits<uint32_t>::max() - b
		? std::numeric_limits<uint32_t>::max() : a + b;
}

Result Merge(const Proposal& a, const Proposal& b, float winnerRandom) {
	const Mapped mappedA = Map(a);
	const Mapped mappedB = Map(b);
	Result result = {};
	result.totalMappedMass = mappedA.mass + mappedB.mass;
	result.selectedOrdinal = kNoSelection;
	result.outputConfidence = SaturatingAdd(mappedA.confidence, mappedB.confidence);
	result.validMask = mappedA.valid | (mappedB.valid << 1u);
	if (!FinitePositive(result.totalMappedMass)) return result;
	const float random01 = std::fmin(std::fmax(winnerRandom, 0.0f), 0.99999994f);
	result.selectedOrdinal = random01 * result.totalMappedMass < mappedA.mass ? 0u : 1u;
	return result;
}

Proposal Positive(uint32_t route, uint32_t confidence, float scaledWeight,
	float sourceTarget, float targetTarget, float jacobian, float mis) {
	return { kValidPositive, route, confidence, 0u, scaledWeight, sourceTarget,
		targetTarget, jacobian, mis, 0.0f, 0.0f, 0.0f };
}

void Require(bool condition, const char* message) {
	if (!condition) throw std::runtime_error(message);
}

void RequireSame(const Result& a, const Result& b, const char* message) {
	Require(std::memcmp(&a, &b, sizeof(Result)) == 0, message);
}

} // namespace

int main(int argc, char** argv) try {
	const Proposal direct = Positive(1u, 3u, 8.0f, 4.0f, 6.0f, 0.5f, 0.25f);
	const Proposal global = Positive(2u, 7u, 2.0f, 2.0f, 5.0f, 2.0f, 0.5f);
	const Result base = Merge(direct, global, 0.2f);
	Require(base.totalMappedMass == 6.5f, "Jacobian/UCW mass conversion failed");
	Require(base.selectedOrdinal == 0u && base.outputConfidence == 10u && base.validMask == 3u,
		"positive pair merge failed");

	Proposal relabeledDirect = direct;
	Proposal relabeledGlobal = global;
	relabeledDirect.routeLabel = 2u;
	relabeledGlobal.routeLabel = 1u;
	RequireSame(base, Merge(relabeledDirect, relabeledGlobal, 0.2f),
		"route label changed estimator output");

	Proposal invalid = direct;
	invalid.status = kInvalid;
	const Result invalidResult = Merge(invalid, global, 0.0f);
	Require(invalidResult.totalMappedMass == 5.0f && invalidResult.selectedOrdinal == 1u &&
		invalidResult.outputConfidence == 7u && invalidResult.validMask == 2u,
		"invalid shift contributed mass or confidence");

	Proposal zero = direct;
	zero.status = kValidZero;
	zero.targetTarget = 0.0f;
	const Result zeroResult = Merge(zero, invalid, 0.5f);
	Require(zeroResult.totalMappedMass == 0.0f && zeroResult.selectedOrdinal == kNoSelection &&
		zeroResult.outputConfidence == 3u && zeroResult.validMask == 1u,
		"valid-zero policy failed");

	Proposal overflow = direct;
	overflow.sourceScaledWeight = std::numeric_limits<float>::max();
	overflow.targetTarget = std::numeric_limits<float>::max();
	const Result overflowResult = Merge(overflow, invalid, 0.5f);
	Require(overflowResult.totalMappedMass == 0.0f && overflowResult.outputConfidence == 0u,
		"non-finite mapped mass was admitted");

	Proposal highConfidence = direct;
	highConfidence.sourceConfidence = std::numeric_limits<uint32_t>::max() - 1u;
	const Result saturated = Merge(highConfidence, global, 0.99f);
	Require(saturated.outputConfidence == std::numeric_limits<uint32_t>::max(),
		"confidence addition did not saturate");

	std::cout << "UPT-30 CPU oracle passed: label-invariant, Jacobian-once, invalid/zero/extreme cases\n";
	if (argc == 2) {
		std::ofstream stamp(argv[1], std::ios::binary | std::ios::trunc);
		if (!stamp) throw std::runtime_error("failed to create oracle stamp");
		stamp << "UPT-30 CPU oracle passed\n";
	}
	return 0;
} catch (const std::exception& error) {
	std::cerr << "UPT-30 CPU oracle FAILED: " << error.what() << '\n';
	return 1;
}
