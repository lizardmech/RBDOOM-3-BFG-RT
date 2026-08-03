#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <type_traits>

namespace rb::upt02 {

enum class CandidateStatus : uint8_t {
	Invalid = 0,
	ValidZero = 1,
	ValidPositive = 2,
};

enum class EventKind : uint8_t {
	None = 0,
	Analytic = 1,
	Emissive = 2,
	Environment = 3,
	Indirect = 4,
};

enum LobeBits : uint8_t {
	LobeDiffuse = 1u << 0u,
	LobeSpecular = 1u << 1u,
	LobeTransmission = 1u << 2u,
	LobeDelta = 1u << 3u,
	LobeEmission = 1u << 4u,
};

enum ReservoirFlags : uint8_t {
	FlagSelected = 1u << 0u,
	FlagGlobal = 1u << 1u,
	FlagVisibilityKnown = 1u << 2u,
	FlagVisibilityPassed = 1u << 3u,
	FlagReplayable = 1u << 4u,
	FlagNeedsShift = 1u << 5u,
};

struct RandomKey {
	uint32_t pixelX = 0;
	uint32_t pixelY = 0;
	uint32_t frameSampleIndex = 0;
	uint32_t passNamespace = 0;
	uint32_t pathVertex = 0;
	uint32_t streamNamespace = 0;
	uint32_t dimension = 0;
	uint32_t sampleOrdinal = 0;
	uint32_t replayEpoch = 0;
};

static_assert(sizeof(RandomKey) == 36, "The UPT logical random key is nine uint32 words");

struct RandomSlot {
	const char* name;
	uint32_t passNamespace;
	uint32_t streamNamespace;
	uint32_t dimension;
};

inline constexpr RandomSlot kRandomSlots[] = {
#define UPT_RANDOM_SLOT(name, passId, streamId, dimensionId) \
	{ #name, passId, streamId, dimensionId },
#include "upt02_random_dimensions.def"
#undef UPT_RANDOM_SLOT
};

struct Candidate {
	CandidateStatus status = CandidateStatus::Invalid;
	EventKind eventKind = EventKind::None;
	uint8_t lobeMask = 0;
	uint8_t flags = 0;
	uint8_t age = 0;
	uint8_t pathLength = 0;
	uint8_t reconnectionVertexLength = 0;
	uint32_t identity0 = 0;
	uint32_t identity1 = 0;
	uint32_t replayKey = 0;
	uint32_t replayIndex = 0;
	uint32_t generationFingerprint = 0;
	double sampleCoord0 = 0.0;
	double sampleCoord1 = 0.0;
	std::array<double, 3> contribution = { 0.0, 0.0, 0.0 };
	double target = 0.0;
	double proposalPdf = 0.0;
	double pathPdf = 0.0;
	double partialJacobian = 1.0;
	double accumulatedRouletteProbability = 1.0;
};

struct LogicalReservoir {
	bool hasSelectedSample = false;
	bool needsRescue = false;
	uint32_t effectiveM = 0;
	double weightSum = 0.0;
	Candidate selected = {};
};

struct PackedReservoir {
	std::array<uint32_t, 16> words = {};
};

static_assert(sizeof(PackedReservoir) == 64, "UPT reservoir stride must remain 64 bytes");
static_assert(std::is_trivially_copyable_v<PackedReservoir>, "Packed reservoir must be byte-copyable");

enum class UpdateResult : uint8_t {
	SkippedInvalid,
	CountedValidZero,
	SelectedPositive,
	KeptPositive,
	RejectedOverflow,
};

struct RouletteState {
	double compensatedThroughput = 1.0;
	double pathPdf = 1.0;
	double accumulatedSurvival = 1.0;
};

inline bool IsFiniteNonNegative(double value) {
	return std::isfinite(value) && value >= 0.0;
}

inline bool IsFinitePositive(double value) {
	return std::isfinite(value) && value > 0.0;
}

inline bool IsSupportedEvent(EventKind kind) {
	return kind >= EventKind::Analytic && kind <= EventKind::Indirect;
}

inline bool IsValidIdentity(const Candidate& candidate) {
	if (candidate.identity0 == 0) {
		return false;
	}
	if (candidate.eventKind == EventKind::Indirect && candidate.replayKey == 0) {
		return false;
	}
	return true;
}

inline bool AreSampleCoordinatesValid(const Candidate& candidate) {
	if (!std::isfinite(candidate.sampleCoord0) || !std::isfinite(candidate.sampleCoord1)) {
		return false;
	}
	if (candidate.eventKind == EventKind::Environment) {
		return candidate.sampleCoord0 >= -1.0 && candidate.sampleCoord0 <= 1.0 &&
			candidate.sampleCoord1 >= -1.0 && candidate.sampleCoord1 <= 1.0;
	}
	const bool inUnitSquare = candidate.sampleCoord0 >= 0.0 && candidate.sampleCoord0 <= 1.0 &&
		candidate.sampleCoord1 >= 0.0 && candidate.sampleCoord1 <= 1.0;
	if (!inUnitSquare) {
		return false;
	}
	if ((candidate.eventKind == EventKind::Emissive || candidate.eventKind == EventKind::Indirect) &&
		candidate.sampleCoord0 + candidate.sampleCoord1 > 1.0) {
		return false;
	}
	return true;
}

inline bool IsStructurallyValid(const Candidate& candidate, uint32_t expectedGeneration) {
	if (candidate.status == CandidateStatus::Invalid || !IsSupportedEvent(candidate.eventKind) ||
		candidate.generationFingerprint != expectedGeneration || expectedGeneration == 0 ||
		(candidate.lobeMask & 0x1fu) == 0 || (candidate.lobeMask & ~0x1fu) != 0 ||
		(candidate.flags & ~0x3fu) != 0 ||
		((candidate.flags & FlagVisibilityPassed) != 0 &&
			(candidate.flags & FlagVisibilityKnown) == 0) ||
		!IsValidIdentity(candidate) || !AreSampleCoordinatesValid(candidate) ||
		candidate.age > 63 || candidate.pathLength > 15 ||
		candidate.reconnectionVertexLength > candidate.pathLength ||
		!IsFinitePositive(candidate.proposalPdf) || !IsFinitePositive(candidate.pathPdf) ||
		!IsFinitePositive(candidate.partialJacobian) ||
		!IsFinitePositive(candidate.accumulatedRouletteProbability) ||
		candidate.accumulatedRouletteProbability > 1.0) {
		return false;
	}
	for (double channel : candidate.contribution) {
		if (!IsFiniteNonNegative(channel)) {
			return false;
		}
	}

	if (candidate.status == CandidateStatus::ValidZero) {
		return candidate.target == 0.0 && candidate.contribution[0] == 0.0 &&
			candidate.contribution[1] == 0.0 && candidate.contribution[2] == 0.0;
	}
	if (candidate.status != CandidateStatus::ValidPositive || !IsFinitePositive(candidate.target)) {
		return false;
	}
	return candidate.contribution[0] > 0.0 || candidate.contribution[1] > 0.0 ||
		candidate.contribution[2] > 0.0;
}

inline UpdateResult StreamCandidate(
	LogicalReservoir& reservoir,
	const Candidate& candidate,
	double selectionRandom,
	uint32_t expectedGeneration) {
	if (!IsStructurallyValid(candidate, expectedGeneration)) {
		return UpdateResult::SkippedInvalid;
	}
	if (reservoir.effectiveM == std::numeric_limits<uint32_t>::max()) {
		return UpdateResult::RejectedOverflow;
	}
	if (candidate.status == CandidateStatus::ValidZero) {
		++reservoir.effectiveM;
		return UpdateResult::CountedValidZero;
	}
	if (!std::isfinite(selectionRandom) || selectionRandom < 0.0 || selectionRandom >= 1.0) {
		return UpdateResult::SkippedInvalid;
	}

	const double candidateWeight = candidate.target / candidate.proposalPdf;
	const double newWeightSum = reservoir.weightSum + candidateWeight;
	if (!IsFinitePositive(candidateWeight) || !IsFinitePositive(newWeightSum) ||
		newWeightSum > static_cast<double>(std::numeric_limits<float>::max())) {
		return UpdateResult::RejectedOverflow;
	}

	const bool select = !reservoir.hasSelectedSample || selectionRandom * newWeightSum < candidateWeight;
	++reservoir.effectiveM;
	reservoir.weightSum = newWeightSum;
	if (select) {
		reservoir.hasSelectedSample = true;
		reservoir.selected = candidate;
		return UpdateResult::SelectedPositive;
	}
	return UpdateResult::KeptPositive;
}

inline bool ApplyRouletteSurvival(RouletteState& state, double survivalProbability) {
	if (!IsFinitePositive(survivalProbability) || survivalProbability > 1.0) {
		return false;
	}
	const RouletteState next = {
		state.compensatedThroughput / survivalProbability,
		state.pathPdf * survivalProbability,
		state.accumulatedSurvival * survivalProbability,
	};
	if (!IsFinitePositive(next.compensatedThroughput) || !IsFinitePositive(next.pathPdf) ||
		!IsFinitePositive(next.accumulatedSurvival)) {
		return false;
	}
	state = next;
	return true;
}

inline uint16_t PackUnorm16(double value) {
	return static_cast<uint16_t>(std::llround(value * 65535.0));
}

inline double UnpackUnorm16(uint16_t value) {
	return static_cast<double>(value) / 65535.0;
}

inline uint16_t PackSnorm16(double value) {
	const int32_t quantized = static_cast<int32_t>(std::llround(value * 32767.0));
	return static_cast<uint16_t>(static_cast<int16_t>(quantized));
}

inline double UnpackSnorm16(uint16_t value) {
	const int16_t signedValue = static_cast<int16_t>(value);
	return std::max(-1.0, static_cast<double>(signedValue) / 32767.0);
}

inline uint32_t PackCoordinatePair(const Candidate& candidate) {
	const uint16_t low = candidate.eventKind == EventKind::Environment ?
		PackSnorm16(candidate.sampleCoord0) : PackUnorm16(candidate.sampleCoord0);
	const uint16_t high = candidate.eventKind == EventKind::Environment ?
		PackSnorm16(candidate.sampleCoord1) : PackUnorm16(candidate.sampleCoord1);
	return static_cast<uint32_t>(low) | (static_cast<uint32_t>(high) << 16u);
}

inline uint32_t PackHeader(const Candidate& candidate) {
	constexpr uint32_t kAbiVersion = 1;
	return kAbiVersion |
		(static_cast<uint32_t>(candidate.eventKind) << 4u) |
		(static_cast<uint32_t>(candidate.lobeMask & 0x1fu) << 7u) |
		(static_cast<uint32_t>((candidate.flags | FlagSelected) & 0x3fu) << 12u) |
		(static_cast<uint32_t>(candidate.age & 0x3fu) << 18u) |
		(static_cast<uint32_t>(candidate.pathLength & 0x0fu) << 24u) |
		(static_cast<uint32_t>(candidate.reconnectionVertexLength & 0x0fu) << 28u);
}

inline constexpr uint32_t kPackedSelectedTag = 1u;
inline constexpr uint32_t kPackedCountOnlyTag = 2u;
// Bit 2 is a reservoir-local request for the later bounded spatial rescue
// policy.  It is meaningful only when no selected sample exists.  This keeps
// tag 0 as the canonical all-zero empty record and tag 2 as count-only while
// allowing UPT-07 to distinguish an ordinary empty write from an empty result
// that exhausted both current and compatible temporal candidates.
inline constexpr uint32_t kPackedNeedsRescueBit = 4u;

inline bool NarrowFiniteFloat(double value, float& result, bool requirePositive) {
	if (!std::isfinite(value) || value > static_cast<double>(std::numeric_limits<float>::max()) ||
		value < -static_cast<double>(std::numeric_limits<float>::max())) {
		return false;
	}
	result = static_cast<float>(value);
	return std::isfinite(result) && (!requirePositive || result > 0.0f);
}

inline bool PackReservoir(
	const LogicalReservoir& reservoir,
	uint32_t expectedGeneration,
	PackedReservoir& packed) {
	packed = {};
	if (!reservoir.hasSelectedSample) {
		if (reservoir.weightSum != 0.0) {
			return false;
		}
		if (reservoir.effectiveM != 0) {
			packed.words[0] = kPackedCountOnlyTag;
			packed.words[1] = reservoir.effectiveM;
		}
		if (reservoir.needsRescue) {
			packed.words[0] |= kPackedNeedsRescueBit;
		}
		return true;
	}
	if (reservoir.effectiveM == 0 || !IsFinitePositive(reservoir.weightSum) ||
		reservoir.selected.status != CandidateStatus::ValidPositive ||
		!IsStructurallyValid(reservoir.selected, expectedGeneration)) {
		return false;
	}

	float contribution[3] = {};
	float target = 0.0f;
	float weightSum = 0.0f;
	float proposalPdf = 0.0f;
	float pathPdf = 0.0f;
	float partialJacobian = 0.0f;
	float rouletteProbability = 0.0f;
	for (int i = 0; i < 3; ++i) {
		if (!NarrowFiniteFloat(reservoir.selected.contribution[i], contribution[i], false) ||
			contribution[i] < 0.0f ||
			(reservoir.selected.contribution[i] > 0.0 && contribution[i] == 0.0f)) {
			return false;
		}
	}
	if (!NarrowFiniteFloat(reservoir.selected.target, target, true) ||
		!NarrowFiniteFloat(reservoir.weightSum, weightSum, true) ||
		!NarrowFiniteFloat(reservoir.selected.proposalPdf, proposalPdf, true) ||
		!NarrowFiniteFloat(reservoir.selected.pathPdf, pathPdf, true) ||
		!NarrowFiniteFloat(reservoir.selected.partialJacobian, partialJacobian, true) ||
		!NarrowFiniteFloat(reservoir.selected.accumulatedRouletteProbability, rouletteProbability, true)) {
		return false;
	}

	auto floatBits = [](float value) {
		uint32_t bits = 0;
		std::memcpy(&bits, &value, sizeof(bits));
		return bits;
	};
	packed.words[0] = PackHeader(reservoir.selected);
	packed.words[1] = reservoir.effectiveM;
	packed.words[2] = reservoir.selected.identity0;
	packed.words[3] = reservoir.selected.identity1;
	packed.words[4] = reservoir.selected.replayKey;
	packed.words[5] = reservoir.selected.replayIndex;
	packed.words[6] = PackCoordinatePair(reservoir.selected);
	packed.words[7] = floatBits(contribution[0]);
	packed.words[8] = floatBits(contribution[1]);
	packed.words[9] = floatBits(contribution[2]);
	packed.words[10] = floatBits(target);
	packed.words[11] = floatBits(weightSum);
	packed.words[12] = floatBits(proposalPdf);
	packed.words[13] = floatBits(pathPdf);
	packed.words[14] = floatBits(partialJacobian);
	packed.words[15] = floatBits(rouletteProbability);
	return true;
}

inline bool IsCanonicalEmpty(const PackedReservoir& packed) {
	for (uint32_t word : packed.words) {
		if (word != 0) {
			return false;
		}
	}
	return true;
}

inline bool UnpackReservoir(
	const PackedReservoir& packed,
	uint32_t currentGeneration,
	LogicalReservoir& reservoir) {
	reservoir = {};
	if (IsCanonicalEmpty(packed)) {
		return true;
	}
	const uint32_t header = packed.words[0];
	const uint32_t lowTag = header & 0x03u;
	const bool needsRescue = (header & kPackedNeedsRescueBit) != 0u;
	if ((header & 0x08u) != 0u) {
		return false;
	}
	if (lowTag == 0u && needsRescue) {
		if (header != kPackedNeedsRescueBit || packed.words[1] != 0u) {
			return false;
		}
		for (size_t word = 2; word < packed.words.size(); ++word) {
			if (packed.words[word] != 0) {
				return false;
			}
		}
		reservoir.needsRescue = true;
		return true;
	}
	if (lowTag == kPackedCountOnlyTag) {
		if (header != (kPackedCountOnlyTag |
			(needsRescue ? kPackedNeedsRescueBit : 0u)) || packed.words[1] == 0) {
			return false;
		}
		for (size_t word = 2; word < packed.words.size(); ++word) {
			if (packed.words[word] != 0) {
				return false;
			}
		}
		reservoir.effectiveM = packed.words[1];
		reservoir.needsRescue = needsRescue;
		return true;
	}
	if ((header & 0x0fu) != kPackedSelectedTag) {
		return false;
	}

	auto bitsFloat = [](uint32_t bits) {
		float value = 0.0f;
		std::memcpy(&value, &bits, sizeof(value));
		return value;
	};
	Candidate candidate = {};
	candidate.status = CandidateStatus::ValidPositive;
	candidate.eventKind = static_cast<EventKind>((header >> 4u) & 0x07u);
	candidate.lobeMask = static_cast<uint8_t>((header >> 7u) & 0x1fu);
	candidate.flags = static_cast<uint8_t>((header >> 12u) & 0x3fu);
	candidate.age = static_cast<uint8_t>((header >> 18u) & 0x3fu);
	candidate.pathLength = static_cast<uint8_t>((header >> 24u) & 0x0fu);
	candidate.reconnectionVertexLength = static_cast<uint8_t>((header >> 28u) & 0x0fu);
	candidate.identity0 = packed.words[2];
	candidate.identity1 = packed.words[3];
	candidate.replayKey = packed.words[4];
	candidate.replayIndex = packed.words[5];
	candidate.generationFingerprint = currentGeneration;
	const uint16_t coord0 = static_cast<uint16_t>(packed.words[6] & 0xffffu);
	const uint16_t coord1 = static_cast<uint16_t>(packed.words[6] >> 16u);
	if (candidate.eventKind == EventKind::Environment) {
		candidate.sampleCoord0 = UnpackSnorm16(coord0);
		candidate.sampleCoord1 = UnpackSnorm16(coord1);
	} else {
		candidate.sampleCoord0 = UnpackUnorm16(coord0);
		candidate.sampleCoord1 = UnpackUnorm16(coord1);
	}
	candidate.contribution = {
		static_cast<double>(bitsFloat(packed.words[7])),
		static_cast<double>(bitsFloat(packed.words[8])),
		static_cast<double>(bitsFloat(packed.words[9])),
	};
	candidate.target = bitsFloat(packed.words[10]);
	candidate.proposalPdf = bitsFloat(packed.words[12]);
	candidate.pathPdf = bitsFloat(packed.words[13]);
	candidate.partialJacobian = bitsFloat(packed.words[14]);
	candidate.accumulatedRouletteProbability = bitsFloat(packed.words[15]);

	reservoir.hasSelectedSample = true;
	reservoir.effectiveM = packed.words[1];
	reservoir.weightSum = bitsFloat(packed.words[11]);
	reservoir.selected = candidate;
	if ((candidate.flags & FlagSelected) == 0 || reservoir.effectiveM == 0 ||
		!IsFinitePositive(reservoir.weightSum) || !IsStructurallyValid(candidate, currentGeneration)) {
		reservoir = {};
		return false;
	}
	return true;
}

inline double FinalReservoirWeight(const LogicalReservoir& reservoir) {
	if (!reservoir.hasSelectedSample || reservoir.effectiveM == 0 ||
		!IsFinitePositive(reservoir.selected.target)) {
		return 0.0;
	}
	return reservoir.weightSum /
		(static_cast<double>(reservoir.effectiveM) * reservoir.selected.target);
}

} // namespace rb::upt02
