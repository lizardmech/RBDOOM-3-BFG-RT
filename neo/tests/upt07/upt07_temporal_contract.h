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
};

inline bool AdmitHistoryPage(
	const PageMetadata& page,
	uint32_t width,
	uint32_t height,
	uint64_t contentGeneration,
	uint64_t historyEpoch) {
	return page.fullyWritten && width != 0 && height != 0 &&
		page.width == width && page.height == height &&
		page.contentGeneration == contentGeneration &&
		page.historyEpoch == historyEpoch && historyEpoch != 0;
}

struct PageRoles {
	uint32_t current = 0;
	uint32_t history = 1;

	void CommitCompletedTemporalFrame() {
		std::swap(current, history);
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
};

struct Projection {
	bool valid = false;
	double pixelX = -1.0;
	double pixelY = -1.0;
	double viewDepth = 0.0;
	int pixelFloorX = -1;
	int pixelFloorY = -1;
};

inline Projection ProjectToPrevious(const Surface& current, const PreviousCamera& camera) {
	Projection result = {};
	if (!current.valid || !camera.valid || camera.width == 0 || camera.height == 0 ||
		!std::isfinite(camera.tanX) || !std::isfinite(camera.tanY) ||
		camera.tanX <= 0.0 || camera.tanY <= 0.0) {
		return result;
	}
	const Vec3 position = current.hasPreviousWorldPosition ?
		current.previousWorldPosition : current.worldPosition;
	const Vec3 delta = position - camera.origin;
	const double depth = Dot(delta, camera.forward);
	if (!std::isfinite(depth) || depth <= 0.05) {
		return result;
	}
	const double ndcX = -Dot(delta, camera.left) / (depth * camera.tanX);
	const double ndcY = -Dot(delta, camera.up) / (depth * camera.tanY);
	if (!std::isfinite(ndcX) || !std::isfinite(ndcY) ||
		std::abs(ndcX) > 1.0 || std::abs(ndcY) > 1.0) {
		return result;
	}
	result.pixelX = (ndcX * 0.5 + 0.5) * static_cast<double>(camera.width);
	result.pixelY = (ndcY * 0.5 + 0.5) * static_cast<double>(camera.height);
	result.viewDepth = depth;
	result.pixelFloorX = static_cast<int>(std::floor(result.pixelX));
	result.pixelFloorY = static_cast<int>(std::floor(result.pixelY));
	result.valid = result.pixelFloorX >= 0 && result.pixelFloorY >= 0 &&
		static_cast<uint32_t>(result.pixelFloorX) < camera.width &&
		static_cast<uint32_t>(result.pixelFloorY) < camera.height;
	return result;
}

inline bool SurfacesCompatible(
	const Surface& current,
	const Surface& previous,
	double projectedPreviousDepth) {
	if (!current.valid || !previous.valid ||
		current.materialId != previous.materialId ||
		current.materialIndex != previous.materialIndex ||
		current.surfaceClass != previous.surfaceClass) {
		return false;
	}
	const Vec3 currentNormal = Normalize(current.geometricNormal);
	const Vec3 previousNormal = Normalize(previous.geometricNormal);
	if (Dot(currentNormal, previousNormal) < 0.85 ||
		std::abs(current.roughness - previous.roughness) > 0.20) {
		return false;
	}
	const double depthTolerance = std::max(0.10, std::abs(projectedPreviousDepth) * 0.10);
	return std::isfinite(previous.previousViewDepth) &&
		std::abs(previous.previousViewDepth - projectedPreviousDepth) <= depthTolerance;
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

inline constexpr std::array<std::array<int, 2>, 5> kHistoryTapOffsets = {{
	{{ 0, 0 }}, {{ -1, 0 }}, {{ 1, 0 }}, {{ 0, -1 }}, {{ 0, 1 }}
}};

inline HistorySearchResult FindCompatibleHistory(
	const Surface& current,
	const PreviousCamera& camera,
	const std::vector<HistoryPixel>& previous,
	bool historyPageAdmitted) {
	HistorySearchResult result = {};
	if (!historyPageAdmitted) {
		return result;
	}
	const Projection projection = ProjectToPrevious(current, camera);
	if (!projection.valid || previous.size() <
		static_cast<size_t>(camera.width) * static_cast<size_t>(camera.height)) {
		return result;
	}
	for (const auto& offset : kHistoryTapOffsets) {
		const int x = projection.pixelFloorX + offset[0];
		const int y = projection.pixelFloorY + offset[1];
		if (x < 0 || y < 0 || static_cast<uint32_t>(x) >= camera.width ||
			static_cast<uint32_t>(y) >= camera.height) {
			continue;
		}
		++result.tapsVisited;
		const uint32_t index = static_cast<uint32_t>(y) * camera.width +
			static_cast<uint32_t>(x);
		const HistoryPixel& candidate = previous[index];
		if (candidate.reservoirSelected &&
			SurfacesCompatible(current, candidate.surface, projection.viewDepth)) {
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
	uint32_t historyM = 0;
};

inline TemporalMergeResult MergeFinalizedTemporalCandidate(
	const LogicalReservoir& current,
	const LogicalReservoir& history,
	const Candidate& shiftedHistory,
	bool historyPageAdmitted,
	bool compatibleSurfaceFound,
	bool shiftedHistoryValid,
	double selectionRandom,
	uint32_t expectedGeneration,
	uint32_t maximumHistoryM = 32u,
	uint8_t maximumHistoryAge = 20u) {
	TemporalMergeResult result = {};
	result.reservoir = current;
	const bool currentSelected = current.hasSelectedSample && current.effectiveM != 0u &&
		IsFinitePositive(current.weightSum) &&
		IsStructurallyValid(current.selected, expectedGeneration);
	const bool historySelected = history.hasSelectedSample && history.effectiveM != 0u &&
		IsFinitePositive(history.weightSum) &&
		IsStructurallyValid(history.selected, expectedGeneration);
	const bool historyUsable = historyPageAdmitted && compatibleSurfaceFound &&
		shiftedHistoryValid && historySelected &&
		history.selected.age < maximumHistoryAge &&
		IsStructurallyValid(shiftedHistory, expectedGeneration) &&
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
	const double historyMass = history.weightSum * static_cast<double>(historyM) *
		(shiftedHistory.target / history.selected.target);
	const uint32_t outputM = current.effectiveM + historyM;
	const double totalMass = currentMass + historyMass;
	if (!IsFinitePositive(historyMass) || !IsFinitePositive(totalMass) || outputM == 0u) {
		if (!currentSelected) {
			result.reservoir.needsRescue = true;
		}
		return result;
	}

	const bool chooseHistory = !currentSelected || selectionRandom * totalMass < historyMass;
	result.reservoir.hasSelectedSample = true;
	result.reservoir.needsRescue = false;
	result.reservoir.effectiveM = outputM;
	result.reservoir.weightSum = totalMass / static_cast<double>(outputM);
	if (chooseHistory) {
		result.reservoir.selected = shiftedHistory;
		result.reservoir.selected.age = static_cast<uint8_t>(
			std::min<uint32_t>(63u, static_cast<uint32_t>(history.selected.age) + 1u));
	}
	result.historyAccepted = true;
	result.historyM = historyM;
	return result;
}

} // namespace rb::upt07
