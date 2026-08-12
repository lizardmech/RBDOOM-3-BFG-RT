#pragma once

#include "../upt02/upt02_unified_reservoir_codec.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

namespace rb::upt07 {

using upt02::Candidate;
using upt02::IsFinitePositive;
using upt02::IsStructurallyValid;
using upt02::LogicalReservoir;

struct PageMetadata {
	bool fullyWritten = false;
	uint32_t width = 0;
	uint32_t height = 0;
	uint64_t contentGeneration = 0;
	uint64_t historyEpoch = 0;
	uint64_t frameSerial = 0;
};

inline bool AdmitHistoryPage(
	const PageMetadata& page,
	uint32_t width,
	uint32_t height,
	uint64_t contentGeneration,
	uint64_t historyEpoch,
	uint64_t currentFrameSerial) {
	return page.fullyWritten && width != 0 && height != 0 &&
		page.width == width && page.height == height &&
		page.contentGeneration == contentGeneration &&
		page.historyEpoch == historyEpoch && historyEpoch != 0 &&
		currentFrameSerial > 1 &&
		page.frameSerial == currentFrameSerial - 1;
}

struct PageRoles {
	uint32_t current = 0;
	uint32_t history = 1;

	bool CompleteFrame(
		bool temporalModeActive,
		bool initialPublished,
		bool spatialPublished) {
		const bool promoteCurrent = temporalModeActive &&
			initialPublished && !spatialPublished;
		if (promoteCurrent) {
			std::swap(current, history);
		}
		return promoteCurrent;
	}
};

struct Vec3 {
	double x = 0.0;
	double y = 0.0;
	double z = 0.0;
};

inline Vec3 operator-(Vec3 a, Vec3 b) {
	return { a.x - b.x, a.y - b.y, a.z - b.z };
}

inline double Dot(Vec3 a, Vec3 b) {
	return a.x * b.x + a.y * b.y + a.z * b.z;
}

inline Vec3 Normalize(Vec3 value) {
	const double lengthSquared = Dot(value, value);
	if (!std::isfinite(lengthSquared) || lengthSquared <= 1.0e-20) {
		return {};
	}
	const double inverseLength = 1.0 / std::sqrt(lengthSquared);
	return { value.x * inverseLength, value.y * inverseLength, value.z * inverseLength };
}

struct Surface {
	bool valid = false;
	Vec3 worldPosition = {};
	Vec3 previousWorldPosition = {};
	bool hasPreviousWorldPosition = false;
	Vec3 geometricNormal = { 1.0, 0.0, 0.0 };
	double roughness = 1.0;
	double previousViewDepth = 0.0;
	uint32_t materialId = 0;
	uint32_t materialIndex = 0;
	uint32_t surfaceClass = 0;
};

struct PreviousCamera {
	bool valid = false;
	uint32_t width = 0;
	uint32_t height = 0;
	Vec3 origin = {};
	Vec3 forward = { 1.0, 0.0, 0.0 };
	Vec3 left = { 0.0, 1.0, 0.0 };
	Vec3 up = { 0.0, 0.0, 1.0 };
	double tanX = 1.0;
	double tanY = 1.0;
	double projectionJitterX = 0.0;
	double projectionJitterY = 0.0;
};

struct Projection {
	bool valid = false;
	double pixelX = -1.0;
	double pixelY = -1.0;
	double linearDepth = 0.0;
	int pixelFloorX = -1;
	int pixelFloorY = -1;
};

inline Projection ProjectToPrevious(
	const Surface& current,
	const PreviousCamera& camera,
	uint32_t borderMargin = 0) {
	Projection result = {};
	if (!current.valid || !camera.valid || camera.width == 0 || camera.height == 0 ||
		!std::isfinite(camera.tanX) || !std::isfinite(camera.tanY) ||
		camera.tanX <= 0.0 || camera.tanY <= 0.0) {
		return result;
	}
	const Vec3 position = current.hasPreviousWorldPosition ?
		current.previousWorldPosition : current.worldPosition;
	const Vec3 delta = position - camera.origin;
	const double forwardDistance = Dot(delta, camera.forward);
	const double linearDepth = std::sqrt(Dot(delta, delta));
	if (!std::isfinite(forwardDistance) || forwardDistance <= 0.05 ||
		!std::isfinite(linearDepth)) {
		return result;
	}
	const double ndcX = -Dot(delta, camera.left) / (forwardDistance * camera.tanX);
	const double ndcY = -Dot(delta, camera.up) / (forwardDistance * camera.tanY);
	if (!std::isfinite(ndcX) || !std::isfinite(ndcY)) {
		return result;
	}
	result.pixelX = (ndcX * 0.5 + 0.5) * static_cast<double>(camera.width) +
		camera.projectionJitterX;
	result.pixelY = (ndcY * 0.5 + 0.5) * static_cast<double>(camera.height) +
		camera.projectionJitterY;
	result.linearDepth = linearDepth;
	result.pixelFloorX = static_cast<int>(std::floor(result.pixelX));
	result.pixelFloorY = static_cast<int>(std::floor(result.pixelY));
	result.valid = result.pixelFloorX >= -static_cast<int>(borderMargin) &&
		result.pixelFloorY >= -static_cast<int>(borderMargin) &&
		result.pixelFloorX < static_cast<int>(camera.width + borderMargin) &&
		result.pixelFloorY < static_cast<int>(camera.height + borderMargin);
	return result;
}

inline bool SurfacesCompatible(
	const Surface& current,
	const Surface& previous,
	double projectedPreviousLinearDepth) {
	if (!current.valid || !previous.valid) {
		return false;
	}
	const Vec3 currentNormal = Normalize(current.geometricNormal);
	const Vec3 previousNormal = Normalize(previous.geometricNormal);
	if (Dot(currentNormal, previousNormal) < 0.35) {
		return false;
	}
	const double depthTolerance = 0.10 * std::max(
		previous.previousViewDepth, projectedPreviousLinearDepth);
	return std::isfinite(previous.previousViewDepth) &&
		std::isfinite(depthTolerance) && depthTolerance >= 0.0 &&
		std::abs(previous.previousViewDepth - projectedPreviousLinearDepth) <= depthTolerance;
}

struct HistoryPixel {
	Surface surface = {};
	bool reservoirSelected = false;
};

struct HistorySearchResult {
	bool found = false;
	uint32_t index = 0;
	uint32_t tapsVisited = 0;
};

inline constexpr uint32_t kMaximumHistoryProbeCount = 9;

struct HistorySearchPattern {
	double jitterX = 0.0;
	double jitterY = 0.0;
	uint32_t probeCount = 1;
	uint32_t borderMargin = 0;
	std::array<std::array<int, 2>, kMaximumHistoryProbeCount> offsets = {};
};

inline HistorySearchResult FindCompatibleHistory(
	const Surface& current,
	const PreviousCamera& camera,
	const std::vector<HistoryPixel>& previous,
	bool historyPageAdmitted,
	const HistorySearchPattern& pattern = {}) {
	HistorySearchResult result = {};
	if (!historyPageAdmitted) {
		return result;
	}
	const Projection projection = ProjectToPrevious(
		current, camera, pattern.borderMargin);
	if (!projection.valid || previous.size() <
		static_cast<size_t>(camera.width) * static_cast<size_t>(camera.height)) {
		return result;
	}
	const int baseX = static_cast<int>(std::floor(
		projection.pixelX + pattern.jitterX));
	const int baseY = static_cast<int>(std::floor(
		projection.pixelY + pattern.jitterY));
	const uint32_t probeCount = std::clamp(
		pattern.probeCount, 1u, kMaximumHistoryProbeCount);
	for (uint32_t probe = 0; probe < probeCount; ++probe) {
		const auto& offset = pattern.offsets[probe];
		const int x = baseX + offset[0];
		const int y = baseY + offset[1];
		if (x < 0 || y < 0 || static_cast<uint32_t>(x) >= camera.width ||
			static_cast<uint32_t>(y) >= camera.height) {
			continue;
		}
		++result.tapsVisited;
		const uint32_t index = static_cast<uint32_t>(y) * camera.width +
			static_cast<uint32_t>(x);
		const HistoryPixel& candidate = previous[index];
		if (SurfacesCompatible(current, candidate.surface, projection.linearDepth)) {
			result.found = true;
			result.index = index;
			return result;
		}
	}
	return result;
}

struct TemporalMergeResult {
	LogicalReservoir reservoir = {};
	bool historyAccepted = false;
	bool selectedHistory = false;
	uint32_t historyM = 0;
};

inline LogicalReservoir InjectPreviousBestSeed(
	const LogicalReservoir& current,
	const Candidate& shiftedHistory,
	bool seedValid,
	double selectionRandom,
	uint32_t expectedGeneration) {
	if (!seedValid || shiftedHistory.status != upt02::CandidateStatus::ValidPositive ||
		!IsStructurallyValid(shiftedHistory, expectedGeneration) ||
		!IsFinitePositive(shiftedHistory.target) ||
		!std::isfinite(selectionRandom) || selectionRandom < 0.0 || selectionRandom >= 1.0) {
		return current;
	}
	const bool currentSelected = current.hasSelectedSample && current.effectiveM != 0u &&
		IsFinitePositive(current.weightSum) &&
		IsStructurallyValid(current.selected, expectedGeneration);
	const double currentMass = currentSelected ?
		current.selected.target * current.weightSum * static_cast<double>(current.effectiveM) : 0.0;
	const double seedMass = shiftedHistory.target;
	const double totalMass = currentMass + seedMass;
	const uint32_t sourceM = currentSelected ? 2u : 1u;
	if (!std::isfinite(currentMass) || currentMass < 0.0 || !IsFinitePositive(totalMass)) {
		return current;
	}
	const bool chooseSeed = !currentSelected || selectionRandom * totalMass < seedMass;
	const double selectedTarget = chooseSeed ? shiftedHistory.target : current.selected.target;
	const double finalizedWeight = totalMass /
		(selectedTarget * static_cast<double>(sourceM));
	if (!IsFinitePositive(finalizedWeight)) {
		return current;
	}
	LogicalReservoir seeded = current;
	seeded.hasSelectedSample = true;
	seeded.needsRescue = false;
	seeded.effectiveM = 1u;
	seeded.weightSum = finalizedWeight;
	if (chooseSeed) {
		seeded.selected = shiftedHistory;
	}
	seeded.selected.age = 0u;
	return seeded;
}

inline TemporalMergeResult MergeFinalizedTemporalCandidate(
	const LogicalReservoir& current,
	const LogicalReservoir& history,
	const Candidate& shiftedHistory,
	bool historyPageAdmitted,
	bool compatibleSurfaceFound,
	bool shiftedHistoryValid,
	double currentTargetAtHistory,
	double selectionRandom,
	uint32_t expectedGeneration,
	uint32_t maximumHistoryM = 32u,
	uint8_t maximumHistoryAge = 63u,
	double shiftJacobian = 1.0) {
	TemporalMergeResult result = {};
	result.reservoir = current;
	const bool currentSelected = current.hasSelectedSample && current.effectiveM != 0u &&
		IsFinitePositive(current.weightSum) &&
		IsStructurallyValid(current.selected, expectedGeneration);
	const bool historySelected = history.hasSelectedSample && history.effectiveM != 0u &&
		IsFinitePositive(history.weightSum) &&
		IsStructurallyValid(history.selected, expectedGeneration);
	const bool shiftedStructurallyValid =
		IsStructurallyValid(shiftedHistory, expectedGeneration);
	const bool historyAgeUsable = maximumHistoryAge >= 63u ||
		history.selected.age < maximumHistoryAge;
	const bool historyUsable = historyPageAdmitted && compatibleSurfaceFound &&
		shiftedHistoryValid && historySelected &&
		historyAgeUsable &&
		shiftedStructurallyValid &&
		IsFinitePositive(shiftJacobian) &&
		std::isfinite(selectionRandom) && selectionRandom >= 0.0 && selectionRandom < 1.0;
	if (!historyUsable) {
		if (!currentSelected) {
			result.reservoir.hasSelectedSample = false;
			result.reservoir.weightSum = 0.0;
			result.reservoir.needsRescue = true;
		}
		return result;
	}

	const uint32_t historyM = std::min(history.effectiveM, maximumHistoryM);
	if (historyM == 0u || current.effectiveM >
		std::numeric_limits<uint32_t>::max() - historyM) {
		if (!currentSelected) {
			result.reservoir.needsRescue = true;
		}
		return result;
	}
	const double currentMass = currentSelected ?
		current.weightSum * static_cast<double>(current.effectiveM) : 0.0;
	const bool shiftedPositive =
		shiftedHistory.status == upt02::CandidateStatus::ValidPositive;
	const double historyMass = shiftedPositive
		? history.weightSum * static_cast<double>(historyM) *
			(shiftedHistory.target / history.selected.target) * shiftJacobian
		: 0.0;
	const uint32_t outputM = current.effectiveM + historyM;
	const double totalMass = currentMass + historyMass;
	if ((shiftedPositive && !IsFinitePositive(historyMass)) ||
		(!shiftedPositive && historyMass != 0.0) || outputM == 0u ||
		(totalMass != 0.0 && !IsFinitePositive(totalMass))) {
		if (!currentSelected) {
			result.reservoir.needsRescue = true;
		}
		return result;
	}

	const bool outputSelected = IsFinitePositive(totalMass);
	const bool chooseHistory = shiftedPositive &&
		(!currentSelected || selectionRandom * totalMass < historyMass);
	const double selectedTargetAtCurrent = chooseHistory ?
		shiftedHistory.target : (currentSelected ? current.selected.target : 0.0);
	const double selectedTargetAtHistory = chooseHistory ?
		history.selected.target : std::max(currentTargetAtHistory, 0.0);
	const double selectedSourceTarget = chooseHistory ?
		selectedTargetAtHistory : selectedTargetAtCurrent;
	const double normalizationDenominator =
		selectedTargetAtCurrent * static_cast<double>(current.effectiveM) +
		selectedTargetAtHistory * static_cast<double>(historyM);
	const double finalizedMeanWeight = outputSelected &&
		IsFinitePositive(selectedSourceTarget) &&
		IsFinitePositive(normalizationDenominator)
		? totalMass * selectedSourceTarget / normalizationDenominator
		: 0.0;
	result.reservoir.hasSelectedSample = outputSelected;
	result.reservoir.needsRescue = !outputSelected;
	result.reservoir.effectiveM = outputM;
	result.reservoir.weightSum = finalizedMeanWeight;
	if (outputSelected && !IsFinitePositive(finalizedMeanWeight)) {
		result.reservoir.hasSelectedSample = false;
		result.reservoir.needsRescue = true;
		return result;
	}
	if (chooseHistory) {
		result.reservoir.selected = shiftedHistory;
		result.reservoir.selected.age = static_cast<uint8_t>(
			std::min<uint32_t>(63u, static_cast<uint32_t>(history.selected.age) + 1u));
		result.selectedHistory = true;
	}
	result.historyAccepted = true;
	result.historyM = historyM;
	return result;
}

} // namespace rb::upt07
