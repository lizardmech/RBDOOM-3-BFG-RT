// Phase A-D CPU producer harness: pack I/O, apply(), --dir table.
// + --dir table. Console exe. No renderer link.

#include "PathTraceCpuProducerPackFormat.h"
#include "PathTraceCpuProducerApply.h"
#include "PathTraceCpuProducerPublish.h"
#include "PathTraceCaptureDeriveRing.h"
#include "PathTraceCommittedCapture.h"
#include "PathTraceDoomMaterialClassifierKernel.h"
#include "PathTraceGeometryIdentityTransport.h"
#include "PathTraceGeometryIdentityReplay.h"
#include "PathTraceUniversePlanningSnapshot.h"
#include "PathTraceProducerLanes.h"

#include <cstdio>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <iostream>
#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace fs = std::filesystem;
using namespace cpu_producer_pack;

namespace
{

int g_failures = 0;

void Check(bool condition, const char* name)
{
	if (condition)
	{
		std::cout << "[PASS] " << name << "\n";
		return;
	}
	std::cout << "[FAIL] " << name << "\n";
	++g_failures;
}

enum Deform : uint32_t
{
	kDeformStaticWorld = 0,
	kDeformRigid = 1,
	kDeformSkinnedSource = 2,
};

class ProducerTables
{
public:
	void PresentMesh(uint64_t meshId, uint32_t deform)
	{
		MeshRecord r{};
		r.meshId = meshId;
		r.modelEpoch = 1;
		r.deformationClass = deform;
		r.generation = 1;
		meshes_[meshId] = r;
	}
	void UpdateMesh(uint64_t meshId)
	{
		auto it = meshes_.find(meshId);
		if (it == meshes_.end())
		{
			Check(false, "UpdateMesh missing");
			return;
		}
		++it->second.generation;
	}
	void FreeMesh(uint64_t meshId)
	{
		if (MeshReferenced(meshId))
		{
			Check(false, "FreeMesh still referenced");
			return;
		}
		meshes_.erase(meshId);
	}
	void PresentMaterial(uint64_t materialId)
	{
		MaterialRecord r{};
		r.materialId = materialId;
		r.generation = 1;
		materials_[materialId] = r;
	}
	void UpdateMaterial(uint64_t materialId)
	{
		auto it = materials_.find(materialId);
		if (it == materials_.end())
		{
			Check(false, "UpdateMaterial missing");
			return;
		}
		++it->second.overlayGeneration;
		++it->second.generation;
	}
	void FreeMaterial(uint64_t materialId)
	{
		materials_.erase(materialId);
	}
	void PresentInstance(uint64_t instanceId, uint64_t meshId)
	{
		InstanceRecord r{};
		r.instanceId = instanceId;
		r.meshId = meshId;
		r.dirty = kDirtyMesh;
		r.generation = 1;
		r.live = 1;
		instances_[instanceId] = r;
	}
	void UpdateInstance(uint64_t instanceId, uint32_t dirty)
	{
		auto it = instances_.find(instanceId);
		if (it == instances_.end() || !it->second.live)
		{
			Check(false, "UpdateInstance missing");
			return;
		}
		it->second.dirty |= dirty;
		++it->second.generation;
	}
	void FreeInstance(uint64_t instanceId)
	{
		instances_.erase(instanceId);
	}
	void PresentLight(uint64_t lightId)
	{
		LightRecord r{};
		r.lightId = lightId;
		r.generation = 1;
		r.live = 1;
		lights_[lightId] = r;
	}
	void UpdateLight(uint64_t lightId, uint32_t dirty)
	{
		auto it = lights_.find(lightId);
		if (it == lights_.end() || !it->second.live)
		{
			Check(false, "UpdateLight missing");
			return;
		}
		it->second.dirty |= dirty;
		++it->second.generation;
	}
	void FreeLight(uint64_t lightId)
	{
		lights_.erase(lightId);
	}

	bool HasMesh(uint64_t id) const { return meshes_.count(id) != 0; }
	bool HasMaterial(uint64_t id) const { return materials_.count(id) != 0; }
	bool HasLiveInstance(uint64_t id) const
	{
		auto it = instances_.find(id);
		return it != instances_.end() && it->second.live;
	}
	bool HasLiveLight(uint64_t id) const
	{
		auto it = lights_.find(id);
		return it != lights_.end() && it->second.live;
	}
	uint32_t InstanceDirty(uint64_t id) const
	{
		auto it = instances_.find(id);
		return it == instances_.end() ? 0 : it->second.dirty;
	}
	uint32_t LightDirty(uint64_t id) const
	{
		auto it = lights_.find(id);
		return it == lights_.end() ? 0 : it->second.dirty;
	}
	uint32_t MaterialOverlay(uint64_t id) const
	{
		auto it = materials_.find(id);
		return it == materials_.end() ? 0 : it->second.overlayGeneration;
	}

	template <typename T>
	static std::vector<T> Values(const std::unordered_map<uint64_t, T>& m)
	{
		std::vector<T> out;
		out.reserve(m.size());
		for (const auto& kv : m)
		{
			out.push_back(kv.second);
		}
		return out;
	}
	std::vector<MeshRecord> MeshVec() const { return Values(meshes_); }
	std::vector<MaterialRecord> MaterialVec() const { return Values(materials_); }
	std::vector<InstanceRecord> InstanceVec() const { return Values(instances_); }
	std::vector<LightRecord> LightVec() const { return Values(lights_); }

private:
	bool MeshReferenced(uint64_t meshId) const
	{
		for (const auto& kv : instances_)
		{
			if (kv.second.live && kv.second.meshId == meshId)
			{
				return true;
			}
		}
		return false;
	}
	std::unordered_map<uint64_t, MeshRecord> meshes_;
	std::unordered_map<uint64_t, MaterialRecord> materials_;
	std::unordered_map<uint64_t, InstanceRecord> instances_;
	std::unordered_map<uint64_t, LightRecord> lights_;
};

fs::path MakeTempDir(const char* tag)
{
	const fs::path root = fs::temp_directory_path() / "cpu_producer_harness";
	fs::create_directories(root);
	static uint32_t seq = 0;
	const fs::path dir = root / (std::string(tag) + "-" + std::to_string(++seq));
	std::error_code ec;
	fs::remove_all(dir, ec);
	fs::create_directories(dir);
	return dir;
}

void PrintTableHeader()
{
	std::printf("%-16s %7s %10s %6s %6s %10s %10s %9s %7s %9s\n",
		"pack", "frames", "dirtyInst", "omit", "admit", "gather_us", "oracle_us",
		"skipCand", "gpuRes", "wouldDrop");
}

void PrintPackRow(const std::string& folderName, const PackTables& pack,
	uint64_t gatherUsValue, const CompletenessStats& complete)
{
	const char* dirtyInst = "n/a";
	const char* omit = "n/a";
	const char* admit = "n/a";
	const char* oracleUs = "n/a";
	char dirtyBuf[32];
	char omitBuf[32];
	char admitBuf[32];
	char oracleBuf[32];
	char gatherBuf[32];
	if (!pack.frames.empty())
	{
		std::snprintf(dirtyBuf, sizeof(dirtyBuf), "%u", pack.frames.back().dirtyInst);
		dirtyInst = dirtyBuf;
		std::snprintf(omitBuf, sizeof(omitBuf), "%u", pack.frames.back().omit);
		omit = omitBuf;
		std::snprintf(admitBuf, sizeof(admitBuf), "%u", pack.frames.back().admit);
		admit = admitBuf;
	}
	if (!pack.oracles.empty())
	{
		std::snprintf(oracleBuf, sizeof(oracleBuf), "%llu",
			static_cast<unsigned long long>(pack.oracles.back().oracleUs));
		oracleUs = oracleBuf;
	}
	std::snprintf(gatherBuf, sizeof(gatherBuf), "%llu",
		static_cast<unsigned long long>(gatherUsValue));
	std::printf("%-16s %7u %10s %6s %6s %10s %10s %9u %7u %9s\n",
		folderName.c_str(),
		pack.manifest.frameCount,
		dirtyInst,
		omit,
		admit,
		gatherBuf,
		oracleUs,
		complete.skipCandidates,
		complete.gpuResident,
		complete.wouldDrop ? "yes" : "no");
}

int CheckApplyAcceptance(
	const std::unordered_map<std::string, ApplyResult>& gathers,
	const std::unordered_map<std::string, PackTables>& packs)
{
	int fails = 0;
	auto has = [&](const char* name) { return packs.find(name) != packs.end(); };
	for (const auto& kv : packs)
	{
		const CompletenessStats complete = MeasureCompleteness(kv.second);
		if (complete.skipSet > complete.omitSkin)
		{
			std::cout << "[FAIL] " << kv.first << " skip-set > omit (skip-all-clean)\n";
			++fails;
		}
		if (complete.skipSet == complete.skipCandidates &&
			complete.skipCandidates > complete.omitSkin)
		{
			std::cout << "[FAIL] " << kv.first << " skip-set is all clean instances\n";
			++fails;
		}
	}
	if (!has("doom3_2") || !has("doom3_3"))
	{
		return fails;
	}
	const ApplyResult& a32 = gathers.at("doom3_2");
	const ApplyResult& a33 = gathers.at("doom3_3");
	const uint64_t o32 = packs.at("doom3_2").oracles.empty() ? 0 : packs.at("doom3_2").oracles.back().oracleUs;
	const uint64_t o33 = packs.at("doom3_3").oracles.empty() ? 0 : packs.at("doom3_3").oracles.back().oracleUs;
	if (a32.gatherUs == o32 && o32 != 0)
	{
		std::cout << "[FAIL] gather_us copied oracle_us on doom3_2\n";
		++fails;
	}
	if (a32.gatherUs > a33.gatherUs)
	{
		std::cout << "[FAIL] gather_us(doom3_2) > gather_us(doom3_3); apply tracked pixels\n";
		++fails;
	}
	if (GatherTracksOracleRatio(a32.gatherUs, a33.gatherUs, o32, o33))
	{
		std::cout << "[FAIL] gather_us ratio still within 20% of oracle_us ratio\n";
		++fails;
	}
	if (has("walkway") && a33.gatherUs > 0)
	{
		const uint64_t walk = gathers.at("walkway").gatherUs;
		const uint64_t naive = a33.gatherUs * 20ull;
		if (walk >= naive)
		{
			std::cout << "[FAIL] walkway gather_us rebuilt the 1400-instance table\n";
			++fails;
		}
	}
	const CompletenessStats c32 = MeasureCompleteness(packs.at("doom3_2"));
	if (c32.skipSet > 17)
	{
		std::cout << "[FAIL] doom3_2 skip-set > omit 17\n";
		++fails;
	}
	return fails;
}


int CheckPublishAcceptance(const std::unordered_map<std::string, PackTables>& packs)
{
	using namespace cpu_producer_publish;
	int fails = 0;
	uint64_t emit32 = 0;
	uint64_t emit33 = 0;
	bool have32 = false;
	bool have33 = false;
	for (const auto& kv : packs)
	{
		const PublishFrameResult off = SimulatePublishFrame(kv.second, 0);
		const PublishFrameResult on = SimulatePublishFrame(kv.second, 1);
		if (on.descriptorKeys.size() < off.descriptorKeys.size())
		{
			std::cout << "[FAIL] A2 " << kv.first << " emitCount(1) < emitCount(0)\n";
			++fails;
		}
		std::unordered_set<uint64_t> uniq(on.descriptorKeys.begin(), on.descriptorKeys.end());
		if (uniq.size() != on.descriptorKeys.size())
		{
			std::cout << "[FAIL] A3 " << kv.first << " duplicate instanceKey\n";
			++fails;
		}
		const uint32_t expected = ExpectedOmittedSkinCandidates(kv.first);
		if (expected != UINT32_MAX && on.counters.publishCandidates != expected)
		{
			std::cout << "[FAIL] A6 " << kv.first << " publishCandidates "
				<< on.counters.publishCandidates << " expected " << expected << "\n";
			++fails;
		}
		if (on.counters.publishCandidates ==
				static_cast<uint32_t>(kv.second.instances.size()) &&
			kv.second.instances.size() > 32)
		{
			std::cout << "[FAIL] A6 " << kv.first << " candidates == full instance table\n";
			++fails;
		}
		if (!CountersBalance(on.counters))
		{
			std::cout << "[FAIL] A6 " << kv.first << " counter identity broken\n";
			++fails;
		}
		if (on.counters.omittedThisFrameWithoutFinalDescriptor != 0)
		{
			std::cout << "[FAIL] A10 " << kv.first
				<< " omittedThisFrameWithoutFinalDescriptor="
				<< on.counters.omittedThisFrameWithoutFinalDescriptor << "\n";
			++fails;
		}
		if (on.rigid.rigidOmittedThisFrameWithoutFinalDescriptor != 0)
		{
			std::cout << "[FAIL] A11 " << kv.first
				<< " rigidOmittedThisFrameWithoutFinalDescriptor="
				<< on.rigid.rigidOmittedThisFrameWithoutFinalDescriptor << "\n";
			++fails;
		}
		if (on.rigid.rigidPublishEmitted != 0)
		{
			std::cout << "[FAIL] A13 " << kv.first
				<< " rigidPublishEmitted="
				<< on.rigid.rigidPublishEmitted << "\n";
			++fails;
		}
		if (!on.rigid.Balance())
		{
			std::cout << "[FAIL] A14 " << kv.first << " rigid counter identity broken\n";
			++fails;
		}
		if (kv.first == "doom3_2")
		{
			have32 = true;
			emit32 = on.descriptorKeys.size();
		}
		if (kv.first == "doom3_3")
		{
			have33 = true;
			emit33 = on.descriptorKeys.size();
			if (kv.second.instances.size() >= 1066 &&
				on.descriptorKeys.size() < 1066)
			{
				std::cout << "[FAIL] A4 doom3_3 emitCount(1) < 1066\n";
				++fails;
			}
		}
		const CompareFrameInput cmpIn = CompareInputFromPack(kv.second, 1);
		const CompareFrameResult cmp = CompareSubmitSets(cmpIn);
		if (!cmp.eligible || !cmp.partitionOk)
		{
			std::cout << "[FAIL] C1 " << kv.first << " layer-1 partition\n";
			++fails;
		}
		if (!cmp.m1.empty() || !cmp.m2.empty() || !cmp.m3.empty() ||
			!cmp.m4.empty() || !cmp.m5.empty() || !cmp.m6.empty() || !cmp.m7.empty() ||
			cmp.m8 || cmp.m9 || cmp.registryEvaluated ||
			cmp.registryVerdict != CompareVerdict::NotEvaluated ||
			cmp.verdict != CompareVerdict::NotEvaluated)
		{
			std::cout << "[FAIL] C2 " << kv.first
				<< " legacy empty-diff / registry no-live not_evaluated baseline\n";
			++fails;
		}
		std::cout << "publish " << kv.first
			<< " mode1 cand=" << on.counters.publishCandidates
			<< " emit=" << on.counters.publishEmitted
			<< " supp=" << on.counters.publishSuppressed
			<< " rej=" << on.counters.RejectedSum()
			<< " omittedWithout=" << on.counters.omittedThisFrameWithoutFinalDescriptor
			<< " desc=" << on.descriptorKeys.size()
			<< " rigidEmit=" << on.rigid.rigidPublishEmitted
			<< " rigidOmit=" << on.rigid.rigidOmittedThisFrameWithoutFinalDescriptor
			<< " " << FormatCompareSizeLine(cmp)
			<< "\n";
		std::vector<WalkedSurface> packMerged;
		std::vector<WalkedSurface> packStatic;
		std::unordered_set<uint64_t> packOmit;
		std::unordered_set<uint64_t> packSkip;
		std::unordered_set<uint64_t> packSubmitted;
		std::unordered_set<uint64_t> packRestore;
		for (const auto& inst : kv.second.instances)
		{
			if (!inst.live)
			{
				continue;
			}
			if (inst.omittedSkin)
			{
				packOmit.insert(inst.instanceId);
				continue;
			}
			WalkedSurface surf;
			surf.id = inst.instanceId;
			surf.triangles = 0;
			packMerged.push_back(surf);
		}
		uint32_t packStaticOracle = 0;
		if (!kv.second.oracles.empty())
		{
			const OracleRecord& o = kv.second.oracles.back();
			packStaticOracle = static_cast<uint32_t>(
				std::max(0, o.staticCachedSurfaces) +
				std::max(0, o.staticNewSurfaces));
			for (uint32_t n = 0; n < packStaticOracle; ++n)
			{
				WalkedSurface surf;
				surf.id = (1ull << 62) + n;
				surf.triangles = 0;
				surf.domain = CaseCIdentityDomain::StaticSurface;
				packStatic.push_back(surf);
			}
		}
		StaticSurfaceProductSet packStaticProduct;
		const CaseCWalkSet packC = CountCaseCWalkSet(
			packStatic, packMerged, packSkip, packOmit, packSubmitted, packRestore, false,
			packStaticProduct);
		std::cout << "caseCWalk " << kv.first
			<< " remove=? packUpperBound static=" << packC.staticBakeOnly()
			<< " merged=" << packC.mergedDynamicOnly()
			<< " note=no-descriptor-list omit-excluded"
			<< "\n";
		std::cout << "mergedWalk " << kv.first
			<< " packCannotJoin note=no-range-blas-tlas"
			<< "\n";
	}
	if (have32 && have33 && emit32 < emit33 &&
		packs.at("doom3_2").instances.size() >= 1066 &&
		packs.at("doom3_3").instances.size() >= 1066)
	{
		std::cout << "[FAIL] A5 emit(1,doom3_2) < emit(1,doom3_3)\n";
		++fails;
	}
	return fails;
}

int LoadAndPrintDir(const fs::path& root)
{
	PrintTableHeader();
	if (!fs::exists(root))
	{
		return 1;
	}
	std::vector<fs::path> packDirs;
	if (fs::exists(root / kManifestName))
	{
		packDirs.push_back(root);
	}
	else if (fs::is_directory(root))
	{
		for (const auto& entry : fs::directory_iterator(root))
		{
			if (entry.is_directory() && fs::exists(entry.path() / kManifestName))
			{
				packDirs.push_back(entry.path());
			}
		}
	}
	std::sort(packDirs.begin(), packDirs.end());
	int localFails = 0;
	std::unordered_map<std::string, ApplyResult> gathers;
	std::unordered_map<std::string, PackTables> loaded;
	for (const fs::path& packDir : packDirs)
	{
		std::string err;
		PackTables pack;
		if (!ReadPack(packDir, pack, err))
		{
			std::cout << "[FAIL] " << packDir.filename().string() << ": " << err << "\n";
			++localFails;
			continue;
		}
		const ApplyResult applied = ApplyCpuProducerPack(pack);
		const CompletenessStats complete = MeasureCompleteness(pack);
		const std::string folder = packDir.filename().string();
		gathers[folder] = applied;
		loaded[folder] = pack;
		PrintPackRow(folder, pack, applied.gatherUs, complete);
	}
	localFails += CheckApplyAcceptance(gathers, loaded);
	localFails += CheckPublishAcceptance(loaded);
	return localFails;
}

void TestLifecycle()
{
	ProducerTables t;
	t.PresentMesh(10, kDeformSkinnedSource);
	t.PresentMaterial(20);
	t.PresentInstance(30, 10);
	t.PresentLight(40);
	Check(t.HasMesh(10) && t.HasMaterial(20) && t.HasLiveInstance(30) &&
			t.HasLiveLight(40),
		"Present four record types");

	t.UpdateInstance(30, kDirtyXform | kDirtyJoints);
	t.UpdateMaterial(20);
	t.UpdateLight(40, kDirtyIntensity);
	t.UpdateMesh(10);
	Check(t.InstanceDirty(30) == (kDirtyMesh | kDirtyXform | kDirtyJoints),
		"Update instance dirty tokens");
	Check(t.MaterialOverlay(20) == 1, "Update material overlay");
	Check(t.LightDirty(40) == kDirtyIntensity, "Update light intensity");

	t.FreeInstance(30);
	t.FreeLight(40);
	Check(!t.HasLiveInstance(30), "Free instance");
	Check(!t.HasLiveLight(40), "Free light");
	Check(t.HasMesh(10) && t.HasMaterial(20),
		"Mesh/Material persist after instance Free");
	t.FreeMesh(10);
	t.FreeMaterial(20);
	Check(!t.HasMesh(10) && !t.HasMaterial(20), "Free unreferenced mesh/material");
}

void TestEmptyPackRoundTrip()
{
	const fs::path dir = MakeTempDir("empty-pack");
	PackTables written;
	written.manifest.schema = kSchemaName;
	written.manifest.schemaVersion = kSchemaVersion;
	written.manifest.saveName = "empty";
	written.manifest.note = "phase-c";
	std::string err;
	Check(WritePack(dir, written, err), "write empty pack");
	PackTables read;
	Check(ReadPack(dir, read, err), "read empty pack");
	Check(read.manifest.schemaVersion == kSchemaVersion &&
			read.manifest.saveName == "empty" && read.instances.empty() &&
			read.frames.empty(),
		"empty pack round-trip fields");
	fs::remove_all(dir);
}

void TestThreeFrameDirLoad()
{
	const fs::path dir = MakeTempDir("three-frame");
	PackTables written;
	written.manifest.schema = kSchemaName;
	written.manifest.schemaVersion = kSchemaVersion;
	written.manifest.saveName = "synthetic";
	written.manifest.note = "phase-c";
	written.manifest.frameCount = 3;
	written.manifest.width = 640;
	written.manifest.height = 360;
	written.manifest.gi = 0;

	InstanceRecord inst{};
	inst.instanceId = 30;
	inst.meshId = 10;
	inst.generation = 2;
	inst.live = 1;
	inst.dirty = kDirtyXform | kDirtyJoints;
	written.instances.push_back(inst);
	LightRecord light{};
	light.lightId = 40;
	light.generation = 1;
	light.live = 1;
	written.lights.push_back(light);

	for (uint32_t i = 0; i < 3; ++i)
	{
		FrameRecord frame{};
		frame.frameIndex = i;
		frame.dirtyInst = (i == 0) ? 1u : 25u;
		frame.dirtyXform = frame.dirtyInst;
		frame.dirtyJoints = frame.dirtyInst;
		frame.omit = 0;
		frame.admit = 0;
		written.frames.push_back(frame);
		OracleRecord oracle{};
		oracle.frameIndex = i;
		oracle.entityUpdates = 25;
		oracle.buildSceneUs = 18000 + i * 1000;
		oracle.oracleUs = 31782;
		written.oracles.push_back(oracle);
	}

	std::string err;
	Check(WritePack(dir, written, err), "write 3-frame synthetic pack");
	PackTables read;
	Check(ReadPack(dir, read, err), "read 3-frame synthetic pack");
	Check(read.frames.size() == 3 && read.oracles.size() == 3 &&
			read.frames.back().dirtyInst == 25 &&
			read.oracles.back().oracleUs == 31782,
		"3-frame dirtyInst/oracle_us present");

	const int dirFails = LoadAndPrintDir(dir);
	Check(dirFails == 0, "--dir load 3-frame pack");
	Check(read.frames.back().dirtyInst != 0 && read.oracles.back().oracleUs != 0,
		"--dir prints dirtyInst and oracle_us not n/a");
	fs::remove_all(dir);
}


// Ranking key is oracle_us and omit, never dirtyInst alone.
// Real doom3_2: dirtyInst=0 omit=17 oracle=11496. doom3_3: dirty=43 omit=0 oracle=5165.
// dirtyInst alone ranks 3_2 cheaper — that is the wrong answer (see 17).
uint64_t RankOracleUs(const PackTables& pack)
{
	return pack.oracles.empty() ? 0 : pack.oracles.back().oracleUs;
}
uint32_t RankOmit(const PackTables& pack)
{
	return pack.frames.empty() ? 0 : pack.frames.back().omit;
}
uint32_t RankDirtyInst(const PackTables& pack)
{
	return pack.frames.empty() ? 0 : pack.frames.back().dirtyInst;
}
bool RankedMoreExpensive(const PackTables& a, const PackTables& b)
{
	if (RankOracleUs(a) != RankOracleUs(b))
	{
		return RankOracleUs(a) > RankOracleUs(b);
	}
	return RankOmit(a) > RankOmit(b);
}

PackTables MakeViewPack(const char* folderHint, uint32_t dirtyInst, uint32_t omit, uint64_t oracleUs)
{
	(void)folderHint;
	PackTables pack;
	pack.manifest.schema = kSchemaName;
	pack.manifest.schemaVersion = kSchemaVersion;
	pack.manifest.saveName = folderHint;
	pack.manifest.note = "phase-c";
	pack.manifest.frameCount = 3;
	for (uint32_t i = 0; i < omit; ++i)
	{
		InstanceRecord inst{};
		inst.instanceId = 1000 + i;
		inst.meshId = 500 + i;
		inst.live = 1;
		inst.omittedSkin = 1;
		inst.vertexCount = 64;
		pack.instances.push_back(inst);
		MeshRecord mesh{};
		mesh.meshId = inst.meshId;
		mesh.contentChecksum = inst.meshId;
		mesh.deformationClass = 2;
		mesh.vertexCount = 64;
		mesh.indexCount = 96;
		pack.meshes.push_back(mesh);
	}
	for (uint32_t i = 0; i < dirtyInst; ++i)
	{
		InstanceRecord inst{};
		inst.instanceId = 2000 + i;
		inst.meshId = 800 + i;
		inst.live = 1;
		inst.dirty = kDirtyXform | kDirtyMaterial;
		inst.vertexCount = 32;
		pack.instances.push_back(inst);
	}
	for (uint32_t i = 0; i < 3; ++i)
	{
		FrameRecord frame{};
		frame.frameIndex = i;
		frame.dirtyInst = dirtyInst;
		frame.omit = omit;
		frame.admit = 0;
		pack.frames.push_back(frame);
		OracleRecord oracle{};
		oracle.frameIndex = i;
		oracle.skinnedCaptureOmittedSurfaces = static_cast<int32_t>(omit);
		oracle.skinnedCaptureOmittedVerts = static_cast<int32_t>(omit * 64);
		oracle.skinnedCaptureOmittedIndexes = static_cast<int32_t>(omit * 96);
		oracle.oracleUs = oracleUs;
		oracle.buildSceneUs = oracleUs;
		pack.oracles.push_back(oracle);
	}
	return pack;
}

void TestDoom32VsDoom33Ranking()
{
	const fs::path root = MakeTempDir("rank-root");
	const fs::path d32 = root / "doom3_2";
	const fs::path d33 = root / "doom3_3";
	PackTables p32 = MakeViewPack("doom3_2", 0, 17, 11496);
	PackTables p33 = MakeViewPack("doom3_3", 43, 0, 5165);
	std::string err;
	Check(WritePack(d32, p32, err), "write 3_2-like pack");
	Check(WritePack(d33, p33, err), "write 3_3-like pack");
	PackTables r32;
	PackTables r33;
	Check(ReadPack(d32, r32, err) && ReadPack(d33, r33, err), "read 3_2/3_3-like packs");
	Check(RankDirtyInst(r32) < RankDirtyInst(r33),
		"3_2 dirtyInst < 3_3 (the trap dirtyInst ranking would take)");
	Check(RankOmit(r32) == 17 && RankOracleUs(r32) > RankOracleUs(r33),
		"3_2 omit=17 and oracle_us > 3_3");
	Check(RankedMoreExpensive(r32, r33),
		"rank by oracle_us+omit: 3_2 more expensive than 3_3");
	const ApplyResult apply32 = ApplyCpuProducerPack(r32);
	const ApplyResult apply33 = ApplyCpuProducerPack(r33);
	Check(apply32.omittedSkinResident == 17, "apply keeps 17 omitted-skin resident");
	Check(apply32.gatherUs <= apply33.gatherUs,
		"apply gather_us 3_2 <= 3_3 (standing, not dirty)");
	const CompletenessStats c32 = MeasureCompleteness(r32);
	Check(c32.skipSet <= 17 && c32.skipSet <= c32.omitSkin,
		"3_2-like skip-set <= omit, never the full instance table");
	Check(apply32.gatherUs != RankOracleUs(r32) && apply33.gatherUs != RankOracleUs(r33),
		"apply gather_us is not oracle_us");
	if (RankDirtyInst(r32) < RankDirtyInst(r33) && !RankedMoreExpensive(r32, r33))
	{
		Check(false, "3_2 ranked cheaper than 3_3 using dirtyInst alone");
	}
	const int dirFails = LoadAndPrintDir(root);
	Check(dirFails == 0, "--dir 3_2/3_3-like prints omit and dirtyInst");
	fs::remove_all(root);
}
void TestRejectForbidden()
{
	const fs::path dir = MakeTempDir("forbidden-pack");
	PackTables written;
	written.manifest.schema = kSchemaName;
	written.manifest.schemaVersion = kSchemaVersion;
	written.manifest.saveName = "bad";
	std::string err;
	WritePack(dir, written, err);
	WriteText(dir / "crash.dmp", "minidump");
	PackTables read;
	const bool ok = ReadPack(dir, read, err);
	Check(!ok && err.find("forbidden") != std::string::npos,
		"reject minidump in pack dir");
	fs::remove(dir / "crash.dmp");
	WriteText(dir / "srfTriangles_t.bin", "nope");
	err.clear();
	const bool ok2 = ReadPack(dir, read, err);
	Check(!ok2 && err.find("forbidden") != std::string::npos,
		"reject srfTriangles filename");
	fs::remove_all(dir);
}

void TestSchemaMismatch()
{
	const fs::path dir = MakeTempDir("bad-schema");
	WriteText(dir / kManifestName,
		"{\n  \"schema\": \"cpu-producer-pack\",\n  \"schemaVersion\": 99,\n"
		"  \"saveName\": \"x\",\n  \"frameCount\": 0\n}\n");
	std::vector<uint8_t> empty;
	for (const char* name : kPackBins)
	{
		WriteBinTable(dir / name, empty);
	}
	WriteText(dir / kTexturesName, "{}\n");
	PackTables read;
	std::string err;
	const bool ok = ReadPack(dir, read, err);
	Check(!ok && err.find("schema version mismatch") != std::string::npos,
		"reject schema version mismatch");
	fs::remove_all(dir);
}


void TestSkipAllCleanTrap()
{
	PackTables pack;
	pack.manifest.schema = kSchemaName;
	pack.manifest.schemaVersion = kSchemaVersion;
	pack.manifest.saveName = "clean-trap";
	pack.manifest.note = "phase-c";
	pack.manifest.frameCount = 1;
	for (uint32_t i = 0; i < 10; ++i)
	{
		InstanceRecord inst{};
		inst.instanceId = 10 + i;
		inst.live = 1;
		inst.dirty = 0;
		pack.instances.push_back(inst);
	}
	const CompletenessStats complete = MeasureCompleteness(pack);
	Check(complete.skipCandidates == 10 && complete.gpuResident == 0,
		"10 clean instances, gpuResident defaults to 0");
	Check(complete.wouldDrop, "skipCandidates > gpuResident would drop GPU-missing geometry");
	Check(SkipAllCleanWouldStarveGpu(complete),
		"refuse skip-all-clean: 10 clean / 0 gpuResident");
	Check(complete.skipSet == 0 && complete.omitSkin == 0,
		"H2 skip-set is omitted-skin only, not the 10 clean");
}


void TestRegistryGap()
{
	using namespace cpu_producer_publish;
	const uint64_t worldTok = 1;
	const uint64_t worldGen = 42;
	const RegistryGapKey liveKey = MakeRegistryGapKey(worldTok, worldGen, 10, 1);
	const RegistryGapKey worldKey = MakeRegistryGapKey(worldTok, worldGen, 11, 1);
	const RegistryGapKey deformKey = MakeRegistryGapKey(worldTok, worldGen, 12, 1);
	const RegistryGapKey deadKey = MakeRegistryGapKey(worldTok, worldGen, 13, 1);
	RegistryGapLiveRecord liveRigid;
	liveRigid.key = liveKey;
	liveRigid.geometryClass = RegistryGapClass::RigidAtRest;
	liveRigid.alive = true;
	RegistryGapLiveRecord world;
	world.key = worldKey;
	world.geometryClass = RegistryGapClass::World;
	world.alive = true;
	RegistryGapLiveRecord deform;
	deform.key = deformKey;
	deform.geometryClass = RegistryGapClass::Deforming;
	deform.alive = true;
	RegistryGapLiveRecord dead;
	dead.key = deadKey;
	dead.geometryClass = RegistryGapClass::RigidAtRest;
	dead.alive = false;
	const std::vector<RegistryGapLiveRecord> presented = {
		liveRigid, world, deform, dead
	};

	const uint64_t liveId = PackRegistryGapIdentity(liveKey);
	const RegistryGapResult noProduct = CountRegistryGap(presented, {});
	Check(noProduct.live() == 1 && noProduct.liveIds[0] == liveId,
		"registry gap live is the Presented live rigid");
	Check(noProduct.product() == 0 && noProduct.gap() == 1 &&
			noProduct.gapIds[0] == liveId,
		"registry gap: Presented live rigid with no extraTlas is a gap");
	Check(std::find(noProduct.gapIds.begin(), noProduct.gapIds.end(),
			PackRegistryGapIdentity(worldKey)) == noProduct.gapIds.end(),
		"registry gap: World-class is not a gap");
	Check(std::find(noProduct.gapIds.begin(), noProduct.gapIds.end(),
			PackRegistryGapIdentity(deformKey)) == noProduct.gapIds.end(),
		"registry gap: Deforming is not a gap");
	Check(std::find(noProduct.gapIds.begin(), noProduct.gapIds.end(),
			PackRegistryGapIdentity(deadKey)) == noProduct.gapIds.end(),
		"registry gap: dead slot is not a gap");

	RegistryGapProduct exactJoin;
	exactJoin.key = liveKey;
	exactJoin.instanceMask = 0x2u;
	exactJoin.submittedBlasToken = 0xAull;
	exactJoin.selectedMeshBlasToken = 0xAull;
	const RegistryGapResult joined = CountRegistryGap({ liveRigid }, { exactJoin });
	Check(joined.live() == 1 && joined.product() == 1 && joined.gap() == 0,
		"registry gap: matching exact join is not a gap");

	RegistryGapProduct maskZero = exactJoin;
	maskZero.instanceMask = 0;
	Check(CountRegistryGap({ liveRigid }, { maskZero }).gap() == 1,
		"registry gap: mask 0 fails the section-5 join");

	RegistryGapProduct blasMismatch = exactJoin;
	blasMismatch.selectedMeshBlasToken = 0xBull;
	Check(CountRegistryGap({ liveRigid }, { blasMismatch }).gap() == 1,
		"registry gap: BLAS mismatch fails the section-5 join");

	const RegistryGapKey g1 = MakeRegistryGapKey(worldTok, worldGen, 7, 1);
	const RegistryGapKey g2 = MakeRegistryGapKey(worldTok, worldGen, 7, 2);
	Check(PackRegistryGapIdentity(g1) != PackRegistryGapIdentity(g2),
		"registry gap: G1/G2 same world+index pack to different identities");
	Check(PackRegistryGapIdentity(g1) != PackDefId(worldGen, 7) &&
			PackRegistryGapIdentity(g2) != PackDefId(worldGen, 7),
		"registry gap: PackRegistryGapIdentity is not PackDefId(worldGeneration,index)");
	RegistryGapLiveRecord liveG2;
	liveG2.key = g2;
	liveG2.geometryClass = RegistryGapClass::RigidAtRest;
	liveG2.alive = true;
	RegistryGapProduct productG1;
	productG1.key = g1;
	productG1.instanceMask = 0x2u;
	productG1.submittedBlasToken = 0xAull;
	productG1.selectedMeshBlasToken = 0xAull;
	const RegistryGapResult reuse = CountRegistryGap({ liveG2 }, { productG1 });
	Check(reuse.live() == 1 && reuse.product() == 1 && reuse.gap() == 1 &&
			reuse.gapIds[0] == PackRegistryGapIdentity(g2),
		"registry gap: G1 product does not clear G2 live at same world+index");

	const fs::path sceneFromHarness =
		fs::path(__FILE__).parent_path() / ".." / "renderer" / "NVRHI" /
		"PathTraceSmokeSceneBuild.cpp";
	const fs::path sceneAbs(
		"E:/prog/rbdoom-3-bfg-rt-producer/neo/renderer/NVRHI/PathTraceSmokeSceneBuild.cpp");
	const fs::path lifeFromHarness =
		fs::path(__FILE__).parent_path() / ".." / "renderer" / "NVRHI" /
		"PathTraceGeometryLifecycle.cpp";
	const fs::path lifeAbs(
		"E:/prog/rbdoom-3-bfg-rt-producer/neo/renderer/NVRHI/PathTraceGeometryLifecycle.cpp");
	std::string sceneText;
	for (const fs::path& candidate : { sceneFromHarness, sceneAbs })
	{
		std::ifstream in(candidate);
		if (!in)
		{
			continue;
		}
		sceneText.assign(std::istreambuf_iterator<char>(in),
			std::istreambuf_iterator<char>());
		break;
	}
	std::string lifeText;
	for (const fs::path& candidate : { lifeFromHarness, lifeAbs })
	{
		std::ifstream in(candidate);
		if (!in)
		{
			continue;
		}
		lifeText.assign(std::istreambuf_iterator<char>(in),
			std::istreambuf_iterator<char>());
		break;
	}
	std::string bridge;
	const auto bridgePos = sceneText.find("BuildRegistryGapAtSubmitBoundary");
	if (bridgePos != std::string::npos)
	{
		const auto bridgeEnd = sceneText.find(
			"void PathTracePrimaryPass::BuildRayTracingSmokeTestScene", bridgePos);
		if (bridgeEnd != std::string::npos)
		{
			bridge = sceneText.substr(bridgePos, bridgeEnd - bridgePos);
		}
	}
	std::string snapshot;
	const auto snapPos = lifeText.find("void SnapshotPresentedEntities");
	if (snapPos != std::string::npos)
	{
		snapshot = lifeText.substr(snapPos, 900);
	}
	Check(!bridge.empty() &&
		bridge.find("MakeRegistryGapKey") != std::string::npos &&
		bridge.find("rec.key.generation") != std::string::npos &&
		bridge.find("submitBoundary") != std::string::npos &&
		bridge.find("RegistryGapProductsFromSubmitBoundary") != std::string::npos &&
		bridge.find("PackDefId") == std::string::npos &&
		bridge.find("PackPresentedEntityId") == std::string::npos,
		"registry gap: production bridge carries PtRenderDefKey generation");
	Check(!snapshot.empty() &&
		snapshot.find("slot.generation") != std::string::npos &&
		snapshot.find("rec.key.generation") != std::string::npos &&
		snapshot.find("PackDefId") == std::string::npos,
		"registry gap: SnapshotPresentedEntities stores slot generation");
}


void TestRegistryContract()
{
	using namespace cpu_producer_publish;
	bool rejected = false;
	std::string log;
	Check(DecodeRegistryModeRaw(0, &rejected, &log) == RegistryMode::Off && !rejected,
		"registry decode 0");
	Check(DecodeRegistryModeRaw(1, &rejected, &log) == RegistryMode::DualWrite && !rejected,
		"registry decode 1");
	Check(DecodeRegistryModeRaw(2, &rejected, &log) == RegistryMode::Off && rejected &&
			log.find("33_phase_a_rigid_registry.txt") != std::string::npos,
		"registry raw 2 is mode 0 and logs 33");
	Check(DecodeRegistryModeRaw(-1, &rejected, &log) == RegistryMode::Off && rejected &&
			log.find("33_phase_a_rigid_registry.txt") != std::string::npos,
		"registry raw -1 is mode 0 and logs 33");

	const fs::path cvarsFromHarness =
		fs::path(__FILE__).parent_path() / ".." / "renderer" / "NVRHI" /
		"PathTraceCVars.cpp";
	const fs::path cvarsAbs(
		"E:/prog/rbdoom-3-bfg-rt-producer/neo/renderer/NVRHI/PathTraceCVars.cpp");
	std::string cvarsText;
	for (const fs::path& candidate : { cvarsFromHarness, cvarsAbs })
	{
		std::ifstream cvarsIn(candidate);
		if (!cvarsIn)
		{
			continue;
		}
		cvarsText.assign(std::istreambuf_iterator<char>(cvarsIn),
			std::istreambuf_iterator<char>());
		break;
	}

	const char* declStartTok = "idCVar r_pathTracingCpuProducerRegistry(";
	const auto declPos = cvarsText.find(declStartTok);
	std::string decl;
	if (declPos != std::string::npos)
	{
		const auto declEnd = cvarsText.find(");", declPos);
		if (declEnd != std::string::npos)
		{
			decl = cvarsText.substr(declPos, declEnd + 2 - declPos);
		}
	}
	Check(!cvarsText.empty() && !decl.empty(),
		"registry CVar declaration found in PathTraceCVars.cpp");
	Check(decl.find("CVAR_ARCHIVE") == std::string::npos,
		"registry CVar has no CVAR_ARCHIVE");
	Check(decl.find("\"0\"") != std::string::npos,
		"registry CVar default string is 0");
	Check(decl.find("CVAR_RENDERER") != std::string::npos &&
			decl.find("CVAR_INTEGER") != std::string::npos,
		"registry CVar flags are CVAR_RENDERER | CVAR_INTEGER");

	// Extra ctor args after the help string are valueMin/valueMax (CVarSystem.cpp:258-277).
	const auto lastQuote = decl.rfind('"');
	bool extraCtorArgs = true;
	if (lastQuote != std::string::npos)
	{
		size_t i = lastQuote + 1;
		while (i < decl.size() &&
			(decl[i] == ' ' || decl[i] == '\t' || decl[i] == '\r' || decl[i] == '\n'))
		{
			++i;
		}
		extraCtorArgs = i >= decl.size() || decl[i] != ')';
	}
	Check(!extraCtorArgs,
		"registry CVar has no valueMin/valueMax constructor args");
}


void TestPublishContract()
{
	using namespace cpu_producer_publish;
	PackTables emptyPack;
	emptyPack.manifest.schema = kSchemaName;
	emptyPack.manifest.schemaVersion = kSchemaVersion;
	PackTables p32 = MakeViewPack("doom3_2", 0, 17, 11496);
	PackTables p33 = MakeViewPack("doom3_3", 43, 0, 5165);

	// A9 exact-value decoder
	bool rejected = false;
	std::string log;
	Check(DecodePublishModeRaw(0, &rejected, &log) == PublishMode::Off && !rejected,
		"A9 decode 0");
	Check(DecodePublishModeRaw(1, &rejected, &log) == PublishMode::DualWrite && !rejected,
		"A9 decode 1");
	Check(DecodePublishModeRaw(2, &rejected, &log) == PublishMode::Off && rejected &&
			log.find("22_publish_spec.txt") != std::string::npos,
		"A9 raw 2 is mode 0 and logs 22");
	Check(DecodePublishModeRaw(-1, &rejected, &log) == PublishMode::Off && rejected,
		"A9 raw -1 is mode 0");
	Check(DecodePublishModeRaw(7, &rejected, &log) == PublishMode::Off && rejected,
		"A9 raw 7 is mode 0");

	const PublishFrameResult clean0 = SimulatePublishFrame(p32, 0);
	const PublishFrameResult mode2 = SimulatePublishFrame(p32, 2);
	Check(mode2.effectiveMode == PublishMode::Off && mode2.counters.AllZero(),
		"A9 raw 2 publisher output zero");
	Check(mode2.descriptorKeys == clean0.descriptorKeys,
		"A9 raw 2 capture output matches clean mode 0");

	// A1 live 0 -> 1 -> 0
	const PublishFrameResult a0 = SimulatePublishFrame(p32, 0);
	const PublishFrameResult a1 = SimulatePublishFrame(p32, 1);
	const PublishFrameResult a0b = SimulatePublishFrame(p32, 0);
	Check(a0b.descriptorKeys == a0.descriptorKeys && a0b.counters.AllZero(),
		"A1 final 0 bit-identical to clean 0, counters cleared");
	Check(a1.counters.publishCandidates == 17 && CountersBalance(a1.counters),
		"A1 mode 1 candidates 17 balance");

	// A2 monotonic
	Check(a1.descriptorKeys.size() >= a0.descriptorKeys.size(),
		"A2 emitCount(1) >= emitCount(0)");

	// A3 dedupe
	std::unordered_set<uint64_t> uniq(a1.descriptorKeys.begin(), a1.descriptorKeys.end());
	Check(uniq.size() == a1.descriptorKeys.size(), "A3 no duplicate instanceKey");

	PackTables wall;
	wall.manifest = p33.manifest;
	PackTables zombies = p32;
	for (uint32_t i = 0; i < 1066; ++i)
	{
		InstanceRecord live{};
		live.instanceId = 50000 + i;
		live.live = 1;
		wall.instances.push_back(live);
		zombies.instances.push_back(live);
	}
	const PublishFrameResult w0 = SimulatePublishFrame(wall, 0);
	const PublishFrameResult w1 = SimulatePublishFrame(wall, 1);
	Check(w1.descriptorKeys.size() >= w0.descriptorKeys.size() &&
			w1.descriptorKeys.size() >= 1066,
		"A4 emitCount(1, 3_3-like) >= 1066");
	Check(w1.counters.publishCandidates == 0 && w1.counters.publishEmitted == 0,
		"A4 wall has nothing for a skinned publisher to add");

	const PublishFrameResult z1 = SimulatePublishFrame(zombies, 1);
	Check(z1.descriptorKeys.size() >= w1.descriptorKeys.size(),
		"A5 zombies present: emit(1,3_2-like) >= emit(1,3_3-like)");

	Check(a1.counters.omittedThisFrameWithoutFinalDescriptor == 0,
		"A10 omittedWithoutFinalDescriptor==0 on accepted plan");

	std::vector<uint64_t> omittedKeys;
	for (const InstanceRecord& inst : p32.instances)
	{
		if (inst.live && inst.omittedSkin)
		{
			omittedKeys.push_back(inst.instanceId);
		}
	}
	std::sort(omittedKeys.begin(), omittedKeys.end());
	omittedKeys.erase(std::unique(omittedKeys.begin(), omittedKeys.end()), omittedKeys.end());
	Check(omittedKeys.size() == 17, "A10 fixture omitted key count");

	SimulatedDynamicGeometry appendCpu;
	Check(SimulateAppendRestoreGeometry(appendCpu, 0) == 0 &&
			appendCpu.indexCount == 0,
		"A10 zero-output append helper returns 0");
	Check(SimulateAppendRestoreGeometry(appendCpu, 3) == 3 &&
			appendCpu.indexCount == 3,
		"A10 positive append helper grows CPU geometry");

	const ForcedPlan forces[] = {
		ForcedPlan::GateDisabled,
		ForcedPlan::TlasCapacityExceeded,
		ForcedPlan::MissingBlas,
		ForcedPlan::ResourceContractMismatch,
		ForcedPlan::MissingCpuRoute
	};

	// Forced plan miss with no append helper: must not invent keys.
	for (ForcedPlan force : forces)
	{
		const PublishFrameResult forced = SimulatePublishFrame(p32, 1, force);
		Check(forced.counters.omittedThisFrameWithoutFinalDescriptor == 17 &&
				forced.counters.restoreCaptured == 0 &&
				CountersBalance(forced.counters),
			"A10 forced miss without append stays omittedWithout");
		for (uint64_t key : omittedKeys)
		{
			Check(std::find(forced.descriptorKeys.begin(), forced.descriptorKeys.end(), key) ==
					forced.descriptorKeys.end(),
				"A10 forced miss does not auto-insert omitted keys");
		}
	}

	RestoreCaptureSim zeroAppend;
	for (uint64_t key : omittedKeys)
	{
		zeroAppend.appendEmittedIndexes[key] = 0;
	}
	const PublishFrameResult zeroOut = SimulatePublishFrame(
		p32, 1, ForcedPlan::MissingBlas, &zeroAppend);
	Check(zeroOut.counters.omittedThisFrameWithoutFinalDescriptor == 17 &&
			zeroOut.counters.restoreCaptured == 0 &&
			CountersBalance(zeroOut.counters),
		"A10 zero-output append fails closed");

	RestoreCaptureSim cpuOnly;
	cpuOnly.commitToThisFrameGpu = false;
	for (uint64_t key : omittedKeys)
	{
		cpuOnly.appendEmittedIndexes[key] = 3;
	}
	const PublishFrameResult lie = SimulatePublishFrame(
		p32, 1, ForcedPlan::MissingBlas, &cpuOnly);
	Check(lie.counters.omittedThisFrameWithoutFinalDescriptor == 17 &&
			lie.counters.restoreCaptured == 0 &&
			!RestoreGeometryInThisFrameGpuProduct(lie.gpuProduct, lie.cpuGeometry.indexCount - 3, 3),
		"A10 CPU append without this-frame GPU commit is not restored");

	RestoreCaptureSim committed;
	for (uint64_t key : omittedKeys)
	{
		committed.appendEmittedIndexes[key] = 3;
	}
	const uint32_t preRestoreIndexes = a1.gpuProduct.uploadedIndexCount;
	for (ForcedPlan force : forces)
	{
		const PublishFrameResult forced = SimulatePublishFrame(p32, 1, force, &committed);
		Check(forced.counters.omittedThisFrameWithoutFinalDescriptor == 0 &&
				forced.counters.restoreCaptured == 17 &&
				CountersBalance(forced.counters),
			"A10 forced miss restore-captures after GPU commit");
		Check(forced.gpuProduct.uploadedIndexCount == preRestoreIndexes + 17u * 3u &&
				forced.gpuProduct.blasIndexCount == forced.gpuProduct.uploadedIndexCount &&
				forced.gpuProduct.uploadedIndexCount == forced.cpuGeometry.indexCount,
			"A10 restored geometry is in this-frame upload/BLAS inputs");
		const uint32_t restoreBegin = preRestoreIndexes;
		Check(RestoreGeometryInThisFrameGpuProduct(forced.gpuProduct, restoreBegin, 17 * 3),
			"A10 restore range is inside this-frame GPU product");
		for (uint64_t key : omittedKeys)
		{
			Check(std::find(forced.descriptorKeys.begin(), forced.descriptorKeys.end(), key) !=
					forced.descriptorKeys.end(),
				"A10 committed restore inserts omitted key");
		}
	}

	RestoreCaptureSim localOnlyGrow;
	localOnlyGrow.forceResizeNewHandles = true;
	localOnlyGrow.propagateReplacementToSmokeBuffers = false;
	for (uint64_t key : omittedKeys)
	{
		localOnlyGrow.appendEmittedIndexes[key] = 3;
	}
	const PublishFrameResult staleBind = SimulatePublishFrame(
		p32, 1, ForcedPlan::MissingBlas, &localOnlyGrow);
	Check(staleBind.gpuProduct.replacementHandles &&
			staleBind.gpuProduct.locals.vertex != staleBind.gpuProduct.bindingSet.vertex &&
			staleBind.gpuProduct.locals.materialIndex != staleBind.gpuProduct.bindingSet.materialIndex &&
			staleBind.gpuProduct.blasSubmit.vertex != staleBind.gpuProduct.committedPackage.vertex &&
			!RestoreReplacementHandlesReachAllConsumers(staleBind.gpuProduct) &&
			staleBind.counters.restoreCaptured == 0 &&
			staleBind.counters.omittedThisFrameWithoutFinalDescriptor == 17,
		"A10 local-only replacement handles cannot clear omittedWithout");

	RestoreCaptureSim forcedGrow;
	forcedGrow.forceResizeNewHandles = true;
	forcedGrow.propagateReplacementToSmokeBuffers = true;
	for (uint64_t key : omittedKeys)
	{
		forcedGrow.appendEmittedIndexes[key] = 3;
	}
	const PublishFrameResult grown = SimulatePublishFrame(
		p32, 1, ForcedPlan::MissingBlas, &forcedGrow);
	Check(grown.gpuProduct.replacementHandles &&
			grown.gpuProduct.locals.vertex != grown.gpuProduct.preRestore.vertex &&
			grown.gpuProduct.locals.materialIndex != grown.gpuProduct.preRestore.materialIndex &&
			grown.gpuProduct.uploadedMaterialIndexCount == grown.gpuProduct.uploadedMaterialCount &&
			grown.gpuProduct.uploadedMaterialIndexCount >= (preRestoreIndexes / 3u) + 17u &&
			grown.counters.omittedThisFrameWithoutFinalDescriptor == 0 &&
			grown.counters.restoreCaptured == 17 &&
			CountersBalance(grown.counters),
		"A10 forced-grow restore-captures after handle propagation");
	Check(RestoreReplacementHandlesReachAllConsumers(grown.gpuProduct) &&
			grown.gpuProduct.blasSubmit.vertex == grown.gpuProduct.bindingSet.vertex &&
			grown.gpuProduct.blasSubmit.vertex == grown.gpuProduct.sceneInputs.vertex &&
			grown.gpuProduct.blasSubmit.vertex == grown.gpuProduct.committedPackage.vertex &&
			grown.gpuProduct.blasSubmit.vertex == grown.gpuProduct.smokeBuffers.vertex &&
			grown.gpuProduct.locals.vertex == grown.gpuProduct.smokeBuffers.vertex &&
			grown.gpuProduct.blasSubmit.materialIndex == grown.gpuProduct.bindingSet.materialIndex &&
			grown.gpuProduct.blasSubmit.materialIndex == grown.gpuProduct.sceneInputs.materialIndex &&
			grown.gpuProduct.blasSubmit.materialIndex == grown.gpuProduct.committedPackage.materialIndex &&
			grown.gpuProduct.blasSubmit.materialIndex == grown.gpuProduct.smokeBuffers.materialIndex &&
			grown.gpuProduct.locals.materialIndex == grown.gpuProduct.smokeBuffers.materialIndex,
		"A10 forced-grow BLAS/bindings/sceneInputs/package share replacement handles");
	Check(RestoreGeometryInThisFrameGpuProduct(grown.gpuProduct, preRestoreIndexes, 17 * 3),
		"A10 forced-grow restore range is inside this-frame GPU product");
	for (uint64_t key : omittedKeys)
	{
		Check(std::find(grown.descriptorKeys.begin(), grown.descriptorKeys.end(), key) !=
				grown.descriptorKeys.end(),
			"A10 forced-grow inserts omitted key only after handle agreement");
	}
	// A11-A14 rigid classifier
	std::vector<SimulatedRigidInstance> rigidFew;
	for (uint32_t i = 0; i < 40; ++i)
	{
		SimulatedRigidInstance inst;
		inst.instanceId = 90000 + i;
		inst.entityIndex = static_cast<int>(i);
		inst.renderEntityNum = static_cast<int>(i);
		inst.modelSurfaceIndex = 0;
		inst.skipCapture = false;
		inst.builderTraceable = true;
		inst.builderAppended = true;
		inst.instanceMask = 2;
		rigidFew.push_back(inst);
	}
	const RigidPublishCounters few = SimulateRigidClassifier(rigidFew, nullptr, true);
	Check(few.rigidPublishEmitted == 0 && few.rigidOmittedThisFrameWithoutFinalDescriptor == 0 &&
			few.Balance(),
		"A11 small rigid set omittedWithout==0 and emitted==0");

	std::vector<SimulatedRigidInstance> skippedMissing;
	SimulatedRigidInstance hole;
	hole.instanceId = 42;
	hole.entityIndex = 7;
	hole.renderEntityNum = 7;
	hole.skipCapture = true;
	hole.builderAppended = false;
	hole.builderTraceable = false;
	hole.builderDecline = 4;
	skippedMissing.push_back(hole);
	const RigidPublishCounters unrestored = SimulateRigidClassifier(skippedMissing, nullptr, false);
	Check(unrestored.rigidOmittedThisFrameWithoutFinalDescriptor == 1 &&
		unrestored.rigidRestoreCaptured == 0 &&
		unrestored.rigidPublishEmitted == 0 &&
		unrestored.Balance(),
		"A11 unrestored skip stays omittedWithout");
	const RigidPublishCounters restored = SimulateRigidClassifier(
		skippedMissing, nullptr, true, SimulatedRigidSubmitRoute::Live(false));
	Check(restored.rigidOmittedThisFrameWithoutFinalDescriptor == 0 &&
		restored.rigidRestoreCaptured == 1 &&
		restored.rigidPublishEmitted == 0 &&
		restored.Balance(),
		"A11 restore-capture closes omittedWithout when dynamic BLAS is submitted");

	std::vector<SimulatedRigidInstance> skippedTraceable;
	SimulatedRigidInstance tracedSkip;
	tracedSkip.instanceId = 43;
	tracedSkip.entityIndex = 8;
	tracedSkip.renderEntityNum = 8;
	tracedSkip.skipCapture = true;
	tracedSkip.builderAppended = true;
	tracedSkip.builderTraceable = true;
	tracedSkip.instanceMask = 2;
	skippedTraceable.push_back(tracedSkip);
	const RigidPublishCounters frozenTraceable = SimulateRigidClassifier(
		skippedTraceable, nullptr, false, SimulatedRigidSubmitRoute::Live(true));
	Check(frozenTraceable.rigidOmittedThisFrameWithoutFinalDescriptor == 1 &&
		frozenTraceable.rigidRestoreCaptured == 0 &&
		frozenTraceable.rigidPublishEmitted == 0 &&
		frozenTraceable.Balance(),
		"A11 frozenStaticCapture keeps omitted on builder-traceable skip");
	const RigidPublishCounters liveTraceable = SimulateRigidClassifier(
		skippedTraceable, nullptr, false, SimulatedRigidSubmitRoute::Live(false));
	Check(liveTraceable.rigidOmittedThisFrameWithoutFinalDescriptor == 0 &&
		liveTraceable.rigidPublishSuppressed == 1 &&
		liveTraceable.rigidPublishEmitted == 0 &&
		liveTraceable.Balance(),
		"A11 builder-traceable skip clears omitted only when extra descriptor is submitted");

	const RigidPublishCounters frozenRestored = SimulateRigidClassifier(
		skippedMissing, nullptr, true, SimulatedRigidSubmitRoute::Live(true));
	Check(frozenRestored.rigidOmittedThisFrameWithoutFinalDescriptor == 1 &&
		frozenRestored.rigidRestoreCaptured == 1 &&
		frozenRestored.rigidPublishEmitted == 0 &&
		frozenRestored.Balance(),
		"A11 frozenStaticCapture keeps omitted on restore-committed skip");

	// A12: live grouped preselect. Input order is not priority order.
	// unseen group is listed first so a missing stable_sort would pick it.
	std::vector<SimulatedRigidInstance> grouped;
	auto pushGroup = [&grouped](
		uint64_t firstId, int entity, int count, bool seen, bool ready)
	{
		for (int i = 0; i < count; ++i)
		{
			SimulatedRigidInstance inst;
			inst.instanceId = firstId + static_cast<uint64_t>(i);
			inst.entityIndex = entity;
			inst.renderEntityNum = entity;
			inst.modelSurfaceIndex = i;
			inst.routeReady = ready;
			inst.seenThisFrame = seen;
			inst.stable = seen;
			inst.transformContinuous = seen;
			inst.skipCapture = !seen || i >= 5;
			inst.builderAppended = seen && i < 5;
			inst.builderTraceable = seen && i < 5;
			inst.instanceMask = (seen && i < 5) ? 2u : 0u;
			grouped.push_back(inst);
		}
	};
	pushGroup(900, 99, 3, false, true);
	{
		SimulatedRigidInstance inst;
		inst.instanceId = 10;
		inst.entityIndex = 1;
		inst.renderEntityNum = 1;
		inst.modelSurfaceIndex = 0;
		inst.routeReady = true;
		inst.seenThisFrame = true;
		inst.stable = true;
		inst.transformContinuous = true;
		inst.builderAppended = true;
		inst.builderTraceable = true;
		inst.instanceMask = 2;
		grouped.push_back(inst);
	}
	pushGroup(20, 2, 4, true, true);
	for (int i = 1; i < 8; ++i)
	{
		SimulatedRigidInstance inst;
		inst.instanceId = static_cast<uint64_t>(10 + i);
		inst.entityIndex = 1;
		inst.renderEntityNum = 1;
		inst.modelSurfaceIndex = i;
		inst.routeReady = true;
		inst.seenThisFrame = true;
		inst.stable = true;
		inst.transformContinuous = true;
		inst.skipCapture = i >= 5;
		inst.builderAppended = i < 5;
		inst.builderTraceable = i < 5;
		inst.instanceMask = i < 5 ? 2u : 0u;
		grouped.push_back(inst);
	}
	const SimulatedPreselectResult groupedPre = SimulateGroupedRigidPreselect(grouped, 5);
	Check(groupedPre.selectedIds.size() == 5,
		"A12 partial first group selects 5 of 8");
	const uint64_t expectedSelected[] = { 10, 11, 12, 13, 14 };
	bool selectedExact = groupedPre.selectedIds.size() == 5;
	for (size_t i = 0; i < groupedPre.selectedIds.size() && i < 5; ++i)
	{
		selectedExact = selectedExact && groupedPre.selectedIds[i] == expectedSelected[i];
	}
	Check(selectedExact, "A12 selected keys are the first five of the sorted first group");
	std::unordered_set<uint64_t> droppedIds;
	int partialMembers = 0;
	for (const SimulatedPreselectDrop& drop : groupedPre.dropped)
	{
		droppedIds.insert(drop.instanceId);
		if (drop.groupPartial)
		{
			++partialMembers;
		}
	}
	const uint64_t expectedDropped[] = { 15, 16, 17, 20, 21, 22, 23, 900, 901, 902 };
	bool droppedExact = droppedIds.size() == 10;
	for (uint64_t id : expectedDropped)
	{
		droppedExact = droppedExact && droppedIds.find(id) != droppedIds.end();
	}
	Check(droppedExact, "A12 dropped keys are the partial tail plus later whole groups");
	Check(partialMembers == 3, "A12 at-most-one partial group reports three tail members");
	Check(droppedIds.find(900) != droppedIds.end() &&
		droppedIds.find(20) != droppedIds.end(),
		"A12 later groups including the unseen first-listed group are wholly dropped");

	const RigidPublishCounters groupedCapped = SimulateRigidClassifier(
		grouped, &groupedPre, true, SimulatedRigidSubmitRoute::Live(false));
	Check(groupedCapped.rigidPublishCandidates == static_cast<uint32_t>(grouped.size()) &&
		groupedCapped.capTruncatedPreselect == 10 &&
		groupedCapped.rigidPublishEmitted == 0 &&
		groupedCapped.rigidOmittedThisFrameWithoutFinalDescriptor == 0 &&
		groupedCapped.rigidRestoreCaptured > 0 &&
		groupedCapped.Balance(),
		"A12 candidates include dropped set and restore closes omitted on live submit");

	std::vector<SimulatedRigidInstance> manyReady;
	for (uint32_t i = 0; i < 600; ++i)
	{
		SimulatedRigidInstance inst;
		inst.instanceId = 1000 + i;
		inst.entityIndex = static_cast<int>(i / 10);
		inst.renderEntityNum = static_cast<int>(i / 10);
		inst.modelSurfaceIndex = static_cast<int>(i % 10);
		inst.routeReady = true;
		inst.seenThisFrame = true;
		inst.stable = true;
		inst.skipCapture = (i >= 510);
		inst.builderAppended = i < 510;
		inst.builderTraceable = i < 510;
		inst.instanceMask = i < 510 ? 2u : 0u;
		manyReady.push_back(inst);
	}
	const SimulatedPreselectResult pre = SimulateGroupedRigidPreselect(manyReady, 510);
	Check(pre.dropped.size() == 90 && pre.selectedIds.size() == 510,
		"A12 preselect drops 90 of 600 ready instances in 10-wide groups");
	int manyPartial = 0;
	for (const SimulatedPreselectDrop& drop : pre.dropped)
	{
		if (drop.groupPartial)
		{
			++manyPartial;
		}
	}
	Check(manyPartial == 0,
		"A12 later 10-wide groups are wholly dropped, none partial");
	const RigidPublishCounters capped = SimulateRigidClassifier(
		manyReady, &pre, true, SimulatedRigidSubmitRoute::Live(false));
	Check(capped.rigidPublishCandidates == 600 &&
		capped.capTruncatedPreselect == 90 &&
		capped.rigidPublishSuppressed == 510 &&
		capped.rigidPublishEmitted == 0 &&
		capped.rigidOmittedThisFrameWithoutFinalDescriptor == 0 &&
		capped.rigidRestoreCaptured == 90 &&
		capped.Balance(),
		"A12 600-ready candidates include dropped set and restore closes A11");

	std::vector<SimulatedRigidInstance> planCap;
	for (uint32_t i = 0; i < 20; ++i)
	{
		SimulatedRigidInstance inst;
		inst.instanceId = 2000 + i;
		inst.entityIndex = static_cast<int>(i);
		inst.renderEntityNum = static_cast<int>(i);
		inst.builderAppended = i < 10;
		inst.builderTraceable = i < 10;
		inst.instanceMask = i < 10 ? 2u : 0u;
		planCap.push_back(inst);
	}
	RigidPublishCounters planStage = SimulateRigidClassifier(planCap, nullptr, true);
	planStage.capTruncatedPlan = 0;
	Check(planStage.rigidPublishEmitted == 0 && planStage.Balance(),
		"A12 plan-stage case keeps emitted==0");

	Check(a1.rigid.rigidPublishEmitted == 0 && a1.rigid.Balance(),
		"A13/A14 pack p32 rigid emitted==0 and balanced");
	Check(w1.rigid.rigidPublishEmitted == 0 && w1.rigid.Balance(),
		"A13/A14 wall rigid emitted==0 and balanced");
	Check(z1.rigid.rigidPublishEmitted == 0 && z1.rigid.Balance(),
		"A13/A14 zombies rigid emitted==0 and balanced");

}

void TestCompareContract()
{
	using namespace cpu_producer_publish;

	CompareFrameInput empty1;
	empty1.decodedMode = 1;
	empty1.rigidCandidates = { 1, 2, 3 };
	empty1.extraSourceIds = { 1, 2, 3 };
	const CompareFrameResult empty = CompareSubmitSets(empty1);
	Check(empty.eligible && empty.partitionOk && empty.verdict == CompareVerdict::Pass &&
		empty.m1.empty() && empty.m2.empty() && empty.m3.empty() &&
		empty.m4.empty() && empty.m5.empty() && empty.m6.empty() && empty.m7.empty() &&
		!empty.m8 && !empty.m9 && empty.submittedInBoth.size() == 3,
		"C2 empty-diff constructed mode-1 frame");

	CompareFrameInput mode0;
	mode0.decodedMode = 0;
	mode0.captureSkipIds = { 9, 10 };
	mode0.rigidCandidates = { 1, 2, 9, 10 };
	mode0.extraSourceIds = { 1, 2 };
	const CompareFrameResult mode0Out = CompareSubmitSets(mode0);
	Check(!mode0Out.eligible && mode0Out.verdict == CompareVerdict::NotEvaluated &&
		mode0Out.m1.empty() && mode0Out.m2.empty() && mode0Out.m3.empty() &&
		mode0Out.m7.empty() &&
		std::find(mode0Out.controlArmSet.begin(), mode0Out.controlArmSet.end(), 9ull) ==
			mode0Out.controlArmSet.end(),
		"C2b mode-0 frame is not eligible and does not invent skip products");
	std::vector<CompareFrameResult> onlyMode0;
	onlyMode0.push_back(mode0Out);
	onlyMode0.push_back(mode0Out);
	Check(CombineCompareFrames(onlyMode0).verdict == CompareVerdict::NotEvaluated,
		"C2b run of only mode-0 frames is not_evaluated");

	CompareFrameInput m1in;
	m1in.decodedMode = 1;
	m1in.extraSourceIds = { 50 };
	m1in.rigidCandidates = { 50 };
	m1in.removeFromActual = { 50 };
	const CompareFrameResult m1 = CompareSubmitSets(m1in);
	Check(!m1.m1.empty() && m1.verdict == CompareVerdict::Fail,
		"C3 M1 fires and FAILs");

	CompareFrameInput m2in;
	m2in.decodedMode = 1;
	m2in.injectActualOnly = { 51 };
	m2in.rigidCandidates = { 51 };
	const CompareFrameResult m2 = CompareSubmitSets(m2in);
	Check(!m2.m2.empty() && m2.verdict == CompareVerdict::Fail,
		"C3 M2 fires and FAILs");

	CompareFrameInput m4in;
	m4in.decodedMode = 1;
	m4in.extraSourceIds = { 52 };
	m4in.restoreCommittedIds = { 52 };
	m4in.hasDynamicBlas = true;
	m4in.rigidCandidates = { 52 };
	const CompareFrameResult m4 = CompareSubmitSets(m4in);
	Check(!m4.m4.empty() && m4.verdict == CompareVerdict::Fail,
		"C3 M4 fires and FAILs");

	CompareFrameInput m5in;
	m5in.decodedMode = 1;
	m5in.extraMaskZeroSourceIds = { 53 };
	m5in.maskZeroTreatedAsProduct = { 53 };
	m5in.rigidCandidates = { 53 };
	const CompareFrameResult m5 = CompareSubmitSets(m5in);
	Check(!m5.m5.empty() && m5.verdict == CompareVerdict::Fail,
		"C3 M5 fires and FAILs");

	CompareFrameInput m6in;
	m6in.decodedMode = 1;
	m6in.restoreCommittedIds = { 54 };
	m6in.hasDynamicBlas = false;
	m6in.rigidCandidates = { 54 };
	const CompareFrameResult m6 = CompareSubmitSets(m6in);
	Check(!m6.m6.empty() && m6.verdict == CompareVerdict::Fail,
		"C3 M6 fires and FAILs");

	CompareFrameInput m7in;
	m7in.decodedMode = 1;
	m7in.capDroppedIds = { 55 };
	m7in.captureSkipIds = { 55 };
	m7in.rigidCandidates = { 55 };
	const CompareFrameResult m7 = CompareSubmitSets(m7in);
	Check(!m7.m7.empty() && m7.verdict == CompareVerdict::Fail,
		"C3 M7 fires and FAILs");

	CompareFrameInput m8in;
	m8in.decodedMode = 0;
	m8in.restoreFired = true;
	const CompareFrameResult m8 = CompareSubmitSets(m8in);
	Check(m8.m8 && !m8.eligible && m8.verdict == CompareVerdict::Fail,
		"C3 M8 fires and FAILs");

	std::unordered_set<uint64_t> wiringRigidNone;
	std::vector<uint64_t> wiringSkinnedOne;
	wiringSkinnedOne.push_back(99);
	CompareFrameInput m8fromBuilder;
	m8fromBuilder.decodedMode = 0;
	FillCompareRestoreFired(m8fromBuilder, wiringRigidNone, wiringSkinnedOne, 0);
	const CompareFrameResult m8wired = CompareSubmitSets(m8fromBuilder);
	Check(m8fromBuilder.restoreFired && m8wired.m8 &&
		m8wired.verdict == CompareVerdict::Fail,
		"C3 M8 FillCompareRestoreFired rigid=0 skinned>0 mode 0 FAILs");

	const fs::path sceneBuildFromHarness =
		fs::path(__FILE__).parent_path() / ".." / "renderer" / "NVRHI" /
		"PathTraceSmokeSceneBuild.cpp";
	const fs::path sceneBuildAbs(
		"E:/prog/rbdoom-3-bfg-rt-producer/neo/renderer/NVRHI/PathTraceSmokeSceneBuild.cpp");
	std::string sceneText;
	for (const fs::path& candidate : { sceneBuildFromHarness, sceneBuildAbs })
	{
		std::ifstream sceneIn(candidate);
		if (!sceneIn)
		{
			continue;
		}
		sceneText.assign(std::istreambuf_iterator<char>(sceneIn),
			std::istreambuf_iterator<char>());
		break;
	}
	const auto fillPos = sceneText.find("FillCompareRestoreFired");
	std::string fillCall;
	if (fillPos != std::string::npos)
	{
		const auto fillEnd = sceneText.find(';', fillPos);
		if (fillEnd != std::string::npos)
		{
			fillCall = sceneText.substr(fillPos, fillEnd - fillPos);
		}
	}
	Check(!sceneText.empty() &&
		fillCall.find("rigidRestoreCommittedIds") != std::string::npos &&
		fillCall.find("skinnedRestoreCommittedIds") != std::string::npos &&
		fillCall.find("compareSkinnedRestoreCaptured") != std::string::npos,
		"C3 M8 production caller passes skinned restore observation to FillCompareRestoreFired");

	CompareFrameInput m9in;
	m9in.decodedMode = 1;
	m9in.rigidPublishEmitted = 1;
	m9in.extraSourceIds = { 56 };
	m9in.rigidCandidates = { 56 };
	const CompareFrameResult m9 = CompareSubmitSets(m9in);
	Check(m9.m9 && m9.verdict == CompareVerdict::Fail,
		"C3 M9 fires and FAILs");

	CompareFrameInput multi;
	multi.decodedMode = 1;
	multi.extraSourceIds = { 70 };
	multi.rigidCandidates = { 70 };
	multi.captureSkipIds = { 70 };
	multi.capDroppedIds = { 70 };
	multi.removeFromActual = { 70 };
	const CompareFrameResult multiOut = CompareSubmitSets(multi);
	Check(!multiOut.m1.empty() && !multiOut.m7.empty() &&
		multiOut.verdict == CompareVerdict::Fail && multiOut.partitionOk,
		"C1 multi-fault M1+M7 reports both memberships and FAILs");

	CompareFrameInput m3in;
	m3in.decodedMode = 1;
	m3in.captureSkipIds = { 80 };
	m3in.restoreCommittedIds = { 80 };
	m3in.hasDynamicBlas = true;
	m3in.rigidCandidates = { 80 };
	const CompareFrameResult m3 = CompareSubmitSets(m3in);
	Check(m3.m3.size() == 1 && m3.m3[0] == 80 &&
		m3.m1.empty() && m3.m2.empty() &&
		m3.m4.empty() && m3.verdict == CompareVerdict::Pass,
		"C4 M3 is reported and does not FAIL");

	std::vector<SimulatedRigidInstance> manyReady;
	for (uint32_t i = 0; i < 600; ++i)
	{
		SimulatedRigidInstance inst;
		inst.instanceId = 1000 + i;
		inst.entityIndex = static_cast<int>(i / 10);
		inst.renderEntityNum = static_cast<int>(i / 10);
		inst.modelSurfaceIndex = static_cast<int>(i % 10);
		inst.routeReady = true;
		inst.seenThisFrame = true;
		inst.stable = true;
		inst.skipCapture = (i >= 510);
		manyReady.push_back(inst);
	}
	const SimulatedPreselectResult pre = SimulateGroupedRigidPreselect(manyReady, 510);
	CompareFrameInput capIn;
	capIn.decodedMode = 1;
	for (const SimulatedRigidInstance& inst : manyReady)
	{
		capIn.rigidCandidates.push_back(inst.instanceId);
		if (inst.instanceId < 1510)
		{
			capIn.extraSourceIds.push_back(inst.instanceId);
		}
		if (inst.skipCapture)
		{
			capIn.captureSkipIds.push_back(inst.instanceId);
		}
	}
	for (const SimulatedPreselectDrop& drop : pre.dropped)
	{
		capIn.capDroppedIds.push_back(drop.instanceId);
	}
	const CompareFrameResult capSkip = CompareSubmitSets(capIn);
	Check(!capSkip.m7.empty() && capSkip.verdict == CompareVerdict::Fail,
		"C5 cap-dropped skip keys with no restore land in M7 and FAIL");

	CompareFrameInput capLegal = capIn;
	capLegal.captureSkipIds.clear();
	const CompareFrameResult capNoSkip = CompareSubmitSets(capLegal);
	Check(capNoSkip.m7.empty() && capNoSkip.m1.empty() && capNoSkip.m2.empty() &&
		capNoSkip.verdict == CompareVerdict::Pass,
		"C5 cap-dropped keys not in the skip set are legal");

	Check(true, "C6 A1/A6/A9-A14 remain in TestPublishContract");
}

void TestRegistryCompare()
{
	using namespace cpu_producer_publish;

	const auto readSource = [](const fs::path& relative) {
		const fs::path fromHarness = fs::path(__FILE__).parent_path() / ".." / relative;
		const fs::path fromTree = fs::path("E:/prog/rbdoom-3-bfg-rt-producer/neo") / relative;
		for (const fs::path& candidate : { fromHarness, fromTree })
		{
			std::ifstream in(candidate);
			if (in)
			{
				return std::string(std::istreambuf_iterator<char>(in),
					std::istreambuf_iterator<char>());
			}
		}
		return std::string();
	};

	const std::string cvars = readSource("renderer/NVRHI/PathTraceCVars.cpp");
	const std::string publish = readSource("renderer/NVRHI/PathTraceCpuProducerPublish.cpp");
	const std::string publishHeader =
		readSource("renderer/NVRHI/PathTraceCpuProducerPublish.h");
	const std::string scene = readSource("renderer/NVRHI/PathTraceSmokeSceneBuild.cpp");
	const std::string universe = readSource("renderer/NVRHI/PathTraceGeometryUniverse.cpp");
	const std::string rigidSubsystem =
		readSource("renderer/NVRHI/PathTraceRigidCandidatePreparedDelta.cpp");
	const std::string lifecycle =
		readSource("renderer/NVRHI/PathTraceGeometryLifecycle.cpp");
	const auto cvarPos = cvars.find("idCVar r_pathTracingCpuProducerRegistryDump(");
	const auto cvarEnd = cvarPos == std::string::npos
		? std::string::npos : cvars.find(");", cvarPos);
	const std::string cvarDecl = cvarEnd == std::string::npos
		? std::string() : cvars.substr(cvarPos, cvarEnd - cvarPos);
	const auto dumpPos = publish.find("void MaybeDumpCpuProducerRegistry(");
	const auto dumpEnd = dumpPos == std::string::npos
		? std::string::npos : publish.find("\n}", dumpPos);
	const std::string dumpFn = dumpEnd == std::string::npos
		? std::string() : publish.substr(dumpPos, dumpEnd - dumpPos);
	const auto writePos = dumpFn.find("file->Write");
	const auto clearAfterWrite = writePos == std::string::npos
		? std::string::npos : dumpFn.find("r_pathTracingCpuProducerRegistryDump.SetString(\"\")", writePos);
	Check(!cvarDecl.empty() &&
		cvarDecl.find("\n    \"\",") != std::string::npos &&
		cvarDecl.find("CVAR_ARCHIVE") == std::string::npos &&
		writePos != std::string::npos && clearAfterWrite != std::string::npos,
		"A5 dump: CVar default \"\", not ARCHIVE, auto-clear after one write");

	CompareFrameInput dumpInput;
	dumpInput.registryDecodedMode = 1;
	dumpInput.registryLive.push_back({ 1, 11, 101, true, true, false, false, true, true });
	dumpInput.registryLive.back().refreshAttempts = 2;
	dumpInput.registryLive.back().refreshSuccesses = 1;
	dumpInput.registryLive.back().refreshFailures = 1;
	dumpInput.registryLive.back().diagnosticReason =
		RigidCpuMeshDiagnosticReason::MissingBlas;
	dumpInput.registryLive.push_back({ 2, 11, 101, true, true, false, false, true, true });
	dumpInput.registryLive.back().refreshAttempts = 2;
	dumpInput.registryLive.back().refreshSuccesses = 1;
	dumpInput.registryLive.back().refreshFailures = 1;
	dumpInput.registryLive.back().diagnosticReason =
		RigidCpuMeshDiagnosticReason::MissingBlas;
	dumpInput.registryRefreshAttempts = 2;
	dumpInput.registryRefreshSuccesses = 1;
	dumpInput.registryRefreshFailures = 1;
	dumpInput.registryLookupHits = 1;
	dumpInput.registryLookupDiagnosticsAvailable = 1;
	dumpInput.registryUniqueMeshRequests = 1;
	dumpInput.registryCandidateRecordCount = 946;
	dumpInput.registryLookupTableSize = 946;
	dumpInput.registryPopulationSnapshotAvailable = 1;
	dumpInput.registryCandidateRecordInsertTotal = 4096;
	dumpInput.registryCandidateRecordInsertsThisFrame = 946;
	dumpInput.registryPersistTargetBoundAtFirstEntityAdd = 0;
	dumpInput.registryPersistCallsTotal = 1679;
	dumpInput.registryPersistedSurfacesTotal = 946;
	dumpInput.registryResidentKeySample[0] = 0x11;
	dumpInput.registryResidentKeySample[1] = 0x22;
	dumpInput.registryResidentKeySampleCount = 2;
	dumpInput.registryPresentRequestKeySample[0] = 0xaa;
	dumpInput.registryPresentRequestKeySample[1] = 0xbb;
	dumpInput.registryPresentRequestKeySampleCount = 2;
	dumpInput.registryIdentitySamplesAvailable = 1;
	dumpInput.registryIdentitySampleCount = 3;
	dumpInput.registryIdentitySamples[0].instanceId = 0x10;
	dumpInput.registryIdentitySamples[0].modelEpoch = 7;
	dumpInput.registryIdentitySamples[0].recomputeAvailable = true;
	dumpInput.registryIdentitySamples[0].storedMeshIds[0] = 0x100;
	dumpInput.registryIdentitySamples[0].storedLookupMembership[0] = 0;
	dumpInput.registryIdentitySamples[0].storedMeshIdCount = 1;
	dumpInput.registryIdentitySamples[0].recomputedMeshIds[0] = 0x200;
	dumpInput.registryIdentitySamples[0].recomputedLookupMembership[0] = 1;
	dumpInput.registryIdentitySamples[0].recomputedMeshIdCount = 1;
	dumpInput.registryIdentitySamples[1].instanceId = 0x20;
	dumpInput.registryIdentitySamples[1].modelEpoch = 8;
	dumpInput.registryIdentitySamples[1].recomputeAvailable = true;
	dumpInput.registryIdentitySamples[1].storedMeshIds[0] = 0x300;
	dumpInput.registryIdentitySamples[1].storedLookupMembership[0] = 0;
	dumpInput.registryIdentitySamples[1].storedMeshIdCount = 1;
	dumpInput.registryIdentitySamples[1].recomputedMeshIds[0] = 0x300;
	dumpInput.registryIdentitySamples[1].recomputedLookupMembership[0] = 0;
	dumpInput.registryIdentitySamples[1].recomputedMeshIdCount = 1;
	dumpInput.registryIdentitySamples[2].instanceId = 0x30;
	dumpInput.registryIdentitySamples[2].modelEpoch = 9;
	dumpInput.registryIdentitySamples[2].recomputeAvailable = true;
	dumpInput.registryIdentitySamples[2].storedMeshIds[0] = 0x400;
	dumpInput.registryIdentitySamples[2].storedLookupMembership[0] = 1;
	dumpInput.registryIdentitySamples[2].storedMeshIdCount = 1;
	dumpInput.registryIdentitySamples[2].recomputedMeshIds[0] = 0x400;
	dumpInput.registryIdentitySamples[2].recomputedLookupMembership[0] = 1;
	dumpInput.registryIdentitySamples[2].recomputedMeshIdCount = 1;
	dumpInput.registryDiagnosticHistogram[
		static_cast<size_t>(RigidCpuMeshDiagnosticReason::MissingBlas)] = 2;
	dumpInput.registrySubmitted.push_back({
		1, 11, 2, 101, 101, kOriginCaptureWalk | kOriginRegistryHook, true });
	const CompareFrameResult dumpResult = CompareSubmitSets(dumpInput);
	const std::string dumpText = FormatRegistryDumpText(dumpInput, dumpResult);
	Check(dumpText.find("live=") != std::string::npos &&
		dumpText.find("gpuResident=") != std::string::npos &&
		dumpText.find("provenance=") != std::string::npos &&
		dumpText.find("descCount=") != std::string::npos &&
		dumpText.find("refreshAttempts=2 refreshSuccesses=1 refreshFailures=1") !=
			std::string::npos &&
		dumpText.find("lookupHits=1") != std::string::npos &&
		dumpText.find("lookupBasis=unique_mesh") != std::string::npos &&
		dumpText.find("uniqueMeshRequests=1") != std::string::npos &&
		dumpText.find("refreshBasis=unique_mesh_cumulative_record") != std::string::npos &&
		dumpText.find("histogramBasis=instance_surface") != std::string::npos &&
		dumpText.find("candidateRecordCount=946 candidateRecordBasis=snapshot") !=
			std::string::npos &&
		dumpText.find("lookupTableSize=946 lookupTableBasis=snapshot") !=
			std::string::npos &&
		dumpText.find("persistTargetBoundAtFirstEntityAdd=0 persistTargetBoundBasis=first_entity_add_latched") !=
			std::string::npos &&
		dumpText.find("persistCallsTotal=1679 persistedSurfacesTotal=946 persistCountersBasis=process_cumulative") !=
			std::string::npos &&
		dumpText.find("persistCountersBasis=process_cumulative candidateRecordInsertTotal=4096 candidateRecordInsertsThisFrame=946 candidateInsertBasis=universe_cumulative_and_current_frame") !=
			std::string::npos &&
		dumpText.find("populationSnapshotAvailable=1 populationSnapshotBasis=post_health_guard") !=
			std::string::npos &&
		dumpText.find("diagnosticHistogram") != std::string::npos &&
		dumpText.find("keySamples residentKeysBasis=sorted_lowest8_unique_lookup_snapshot residentKeyCount=2 residentKeys=0x0000000000000011,0x0000000000000022") !=
			std::string::npos &&
		dumpText.find("presentRequestKeysBasis=sorted_lowest8_unique_nonzero_request_snapshot presentRequestKeyCount=2 presentRequestKeys=0x00000000000000aa,0x00000000000000bb") !=
			std::string::npos &&
		dumpText.find("identitySampleSummary instanceBasis=first8_live_rigid_slot_snapshot surfaceCap=8 surfaceCapBasis=first8_model_surfaces identitySamplesAvailable=1 identitySampleCount=3") !=
			std::string::npos &&
		dumpText.find("instanceId=0x0000000000000010 instanceIdBasis=slot.rigidInstance.instanceId modelEpoch=7 modelEpochBasis=slot.modelEpoch storedKeysBasis=slot.rigidInstance.meshIds_first8 storedKeyCount=1 storedKeys=0x0000000000000100 storedMembershipBasis=direct_lookup_map_post_health_guard storedMembership=0 recomputeAvailable=1 recomputedKeysBasis=ComputeRigidMeshHashesFromPresent_dump_time_same_entity_model_slot_modelEpoch_first8_surfaces recomputedKeyCount=1 recomputedKeys=0x0000000000000200 recomputedMembershipBasis=direct_lookup_map_post_health_guard recomputedMembership=1") !=
			std::string::npos &&
		dumpText.find("instanceId=0x0000000000000020 instanceIdBasis=slot.rigidInstance.instanceId modelEpoch=8 modelEpochBasis=slot.modelEpoch storedKeysBasis=slot.rigidInstance.meshIds_first8 storedKeyCount=1 storedKeys=0x0000000000000300 storedMembershipBasis=direct_lookup_map_post_health_guard storedMembership=0 recomputeAvailable=1 recomputedKeysBasis=ComputeRigidMeshHashesFromPresent_dump_time_same_entity_model_slot_modelEpoch_first8_surfaces recomputedKeyCount=1 recomputedKeys=0x0000000000000300 recomputedMembershipBasis=direct_lookup_map_post_health_guard recomputedMembership=0") !=
			std::string::npos &&
		dumpText.find("instanceId=0x0000000000000030 instanceIdBasis=slot.rigidInstance.instanceId modelEpoch=9 modelEpochBasis=slot.modelEpoch storedKeysBasis=slot.rigidInstance.meshIds_first8 storedKeyCount=1 storedKeys=0x0000000000000400 storedMembershipBasis=direct_lookup_map_post_health_guard storedMembership=1 recomputeAvailable=1 recomputedKeysBasis=ComputeRigidMeshHashesFromPresent_dump_time_same_entity_model_slot_modelEpoch_first8_surfaces recomputedKeyCount=1 recomputedKeys=0x0000000000000400 recomputedMembershipBasis=direct_lookup_map_post_health_guard recomputedMembership=1") !=
			std::string::npos &&
		dumpText.find("missing_blas=2") != std::string::npos &&
		dumpText.find("failReason=missing_blas") != std::string::npos,
		"Registry diagnostic: explicit bases distinguish one shared mesh from two instance surfaces");
	CompareFrameInput unavailablePopulationInput = dumpInput;
	unavailablePopulationInput.registryPopulationSnapshotAvailable = 0;
	unavailablePopulationInput.registryCandidateRecordCount = 0;
	unavailablePopulationInput.registryLookupTableSize = 0;
	const std::string unavailablePopulationText = FormatRegistryDumpText(
		unavailablePopulationInput, CompareSubmitSets(unavailablePopulationInput));
	Check(unavailablePopulationText.find(
			"candidateRecordCount=unknown candidateRecordBasis=snapshot") !=
			std::string::npos &&
		unavailablePopulationText.find(
			"lookupTableSize=unknown lookupTableBasis=snapshot") !=
			std::string::npos &&
		unavailablePopulationText.find(
			"populationSnapshotAvailable=0 populationSnapshotBasis=post_health_guard") !=
			std::string::npos,
		"Registry diagnostic: unavailable population snapshot is unknown, never empty zero");
	struct DiagnosticSnapshotEntry
	{
		bool ready = false;
		bool built = false;
		bool pending = false;
		bool pendingExpired = false;
		uint64_t blasToken = 0;
	};
	std::unordered_map<uint64_t, DiagnosticSnapshotEntry> diagnosticSnapshot;
	diagnosticSnapshot[11] = { true, true, false, false, 101 };
	RigidRegistryLookupDiagnosticCounts sharedMeshCounts;
	Check(ReduceRigidRegistryLookupDiagnostics(
			std::vector<uint64_t>{ 11, 11, 0 }, diagnosticSnapshot,
			sharedMeshCounts) &&
		sharedMeshCounts.available &&
		sharedMeshCounts.uniqueMeshRequests == 1 &&
		sharedMeshCounts.zeroHashSkipped == 1 &&
		sharedMeshCounts.lookupHits == 1 &&
		sharedMeshCounts.lookupMisses == 0 &&
		sharedMeshCounts.ready == 1 && sharedMeshCounts.built == 1 &&
		sharedMeshCounts.blasTokens == 1,
		"Registry diagnostic reducer: duplicate instance meshes count once and zero hashes are explicit");
	RigidRegistryLookupDiagnosticCounts failureCounts;
	failureCounts.lookupHits = 77;
	const size_t snapshotSizeBeforeFailure = diagnosticSnapshot.size();
	const uint64_t snapshotTokenBeforeFailure = diagnosticSnapshot[11].blasToken;
	Check(!ReduceRigidRegistryLookupDiagnostics(
			std::vector<uint64_t>{ 11 }, diagnosticSnapshot, failureCounts, true) &&
		diagnosticSnapshot.size() == snapshotSizeBeforeFailure &&
		diagnosticSnapshot.at(11).blasToken == snapshotTokenBeforeFailure &&
		failureCounts.lookupHits == 77,
		"Registry diagnostic reducer: forced failure leaves completed snapshot and prior counters unchanged");
	Check(std::string(RigidCpuMeshDiagnosticReasonName(
			RigidCpuMeshDiagnosticReason::NullTri)) == "null_tri" &&
		std::string(RigidCpuMeshDiagnosticReasonName(
			RigidCpuMeshDiagnosticReason::GpuSignatureMismatch)) ==
			"gpu_signature_mismatch" &&
		kRigidCpuMeshDiagnosticReasonCount > 20,
		"Registry diagnostic: fail buckets name refresh, usability, and GPU-ready branches");
	Check(!universe.empty() && !rigidSubsystem.empty() &&
		rigidSubsystem.find("++record.cpuCacheRefreshAttempts") != std::string::npos &&
		rigidSubsystem.find("++record.cpuCacheRefreshSuccesses") != std::string::npos &&
		rigidSubsystem.find("++record.cpuCacheRefreshFailures") != std::string::npos &&
		universe.find("entry.diagnosticReason =") != std::string::npos &&
		universe.find("RigidMeshRegistryDiagnosticReason(record)") !=
			std::string::npos &&
		universe.find("ReduceRigidRegistryLookupDiagnostics(") != std::string::npos,
		"Registry diagnostic: production Refresh and lookup snapshot populate counters/reasons");
	const auto snapshotFnPos = universe.find(
		"void RtSmokeGeometryUniverse::SnapshotRigidMeshRegistryLookup");
	const auto snapshotFnEnd = snapshotFnPos == std::string::npos
		? std::string::npos
		: universe.find("\n}\n\nint RtSmokeGeometryUniverse::ComputeRigidMeshHashesFromPresent", snapshotFnPos);
	const std::string snapshotFn = snapshotFnEnd == std::string::npos
		? std::string() : universe.substr(snapshotFnPos, snapshotFnEnd - snapshotFnPos);
	const auto productionLoop = snapshotFn.find("for (size_t hashIndex");
	const auto productionCatch = snapshotFn.find("catch (const std::length_error&)", productionLoop);
	const auto reducerCall = snapshotFn.find("ReduceRigidRegistryLookupDiagnostics(", productionCatch);
	const auto matchBrace = [](const std::string& text, size_t openPos) -> size_t {
		if (openPos == std::string::npos || openPos >= text.size() || text[openPos] != '{')
		{
			return std::string::npos;
		}
		int depth = 0;
		for (size_t pos = openPos; pos < text.size(); ++pos)
		{
			if (text[pos] == '{')
			{
				++depth;
			}
			else if (text[pos] == '}' && --depth == 0)
			{
				return pos;
			}
		}
		return std::string::npos;
	};
	const auto catchOpen = productionCatch == std::string::npos
		? std::string::npos : snapshotFn.find('{', productionCatch);
	const auto catchClose = matchBrace(snapshotFn, catchOpen);
	const std::string productionLoopText =
		productionLoop == std::string::npos || productionCatch == std::string::npos
			? std::string()
			: snapshotFn.substr(productionLoop, productionCatch - productionLoop);
	Check(!productionLoopText.empty() &&
		productionLoopText.find("seenHashes") == std::string::npos &&
		productionLoopText.find("uniqueMeshHashes") == std::string::npos &&
		productionLoopText.find("meshHash == 0") == std::string::npos &&
		productionLoopText.find("std::unordered_set") == std::string::npos &&
		productionLoopText.find("std::vector") == std::string::npos &&
		productionLoopText.find("diagnostics->") == std::string::npos &&
		productionLoopText.find("ReduceRigidRegistryLookupDiagnostics") == std::string::npos &&
		catchClose != std::string::npos && reducerCall != std::string::npos &&
		reducerCall > catchClose &&
		snapshotFn.find("QuarantineRigidMeshCandidateLookup", reducerCall) ==
			std::string::npos,
		"Registry diagnostic pin: production lookup loop has no diagnostic allocation/continue and reducer cannot quarantine");
	const auto ensureGuard = snapshotFn.find("if (!EnsureRigidMeshCandidateLookupLive(0))");
	const auto ensureGuardOpen = ensureGuard == std::string::npos
		? std::string::npos : snapshotFn.find('{', ensureGuard);
	const auto ensureGuardClose = matchBrace(snapshotFn, ensureGuardOpen);
	const auto ensureEarlyReturn = snapshotFn.find("diagnostics->earlyReturn = true", ensureGuard);
	const auto ensureReturn = snapshotFn.find("return;", ensureEarlyReturn);
	const auto candidateRecordCount = snapshotFn.find(
		"diagnostics->candidateRecordCount");
	const auto lookupTableSize = snapshotFn.find("diagnostics->lookupTableSize");
	const auto persistCallsTotal = snapshotFn.find("diagnostics->persistCallsTotal");
	const auto populationSnapshotAvailable = snapshotFn.find(
		"diagnostics->populationSnapshotAvailable = true");
	const auto residentKeySample = snapshotFn.find(
		"diagnostics->residentKeySample", reducerCall);
	const auto requestKeySample = snapshotFn.find(
		"diagnostics->presentRequestKeySample", reducerCall);
	const auto reducerHelper = publishHeader.find(
		"inline bool ReduceRigidRegistryLookupDiagnostics(");
	const auto reducerCatch = publishHeader.find("catch (...)", reducerHelper);
	const auto reducerPublish = publishHeader.find("diagnostics = reduced", reducerCatch);
	const auto isIdentifierChar = [](char c) {
		return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
			(c >= '0' && c <= '9') || c == '_';
	};
	const auto findIdentifierToken = [&isIdentifierChar](
		const std::string& text, const std::string& token) -> size_t {
		for (size_t pos = text.find(token); pos != std::string::npos;
			pos = text.find(token, pos + token.size()))
		{
			const bool leftBoundary = pos == 0 || !isIdentifierChar(text[pos - 1]);
			const size_t end = pos + token.size();
			const bool rightBoundary = end == text.size() ||
				!isIdentifierChar(text[end]);
			if (leftBoundary && rightBoundary)
			{
				return pos;
			}
		}
		return std::string::npos;
	};
	const auto firstLookupMapToken = findIdentifierToken(
		snapshotFn, "m_rigidMeshCandidateLookup");
	Check(ensureGuard != std::string::npos &&
		ensureGuardClose != std::string::npos &&
		ensureEarlyReturn != std::string::npos && ensureReturn != std::string::npos &&
		ensureGuard < ensureEarlyReturn && ensureEarlyReturn < ensureReturn &&
		ensureReturn < ensureGuardClose &&
		reducerHelper != std::string::npos && reducerCatch != std::string::npos &&
		reducerPublish != std::string::npos && reducerCatch < reducerPublish,
		"Registry diagnostic pin: R3 flag precedes unchanged Ensure return and reducer contains failures locally");
	Check(candidateRecordCount != std::string::npos &&
		lookupTableSize != std::string::npos &&
		persistCallsTotal != std::string::npos &&
		populationSnapshotAvailable != std::string::npos &&
		firstLookupMapToken != std::string::npos &&
		persistCallsTotal < ensureGuard &&
		ensureGuard < ensureReturn && ensureReturn < firstLookupMapToken &&
		lookupTableSize < firstLookupMapToken &&
		ensureReturn < candidateRecordCount && ensureReturn < lookupTableSize &&
		lookupTableSize < populationSnapshotAvailable &&
		productionLoopText.find("candidateRecordCount") == std::string::npos &&
		productionLoopText.find("lookupTableSize") == std::string::npos &&
		productionLoopText.find("persistCallsTotal") == std::string::npos,
		"Registry diagnostic pin: health guard returns before any lookup-map token; population samples only on success");
	Check(residentKeySample != std::string::npos &&
		requestKeySample != std::string::npos && catchClose != std::string::npos &&
		residentKeySample > catchClose && requestKeySample > catchClose &&
		productionLoopText.find("residentKeySample") == std::string::npos &&
		productionLoopText.find("presentRequestKeySample") == std::string::npos,
		"Registry diagnostic pin: bounded key samples are reduced only after the production lookup loop");
	const auto identityRecompute = snapshotFn.find(
		"ComputeRigidMeshHashesFromPresent(", reducerCall);
	const auto storedMembershipProbe = snapshotFn.find(
		"m_rigidMeshCandidateLookup.find(key)", reducerCall);
	const auto recomputedMembershipProbe = snapshotFn.find(
		"m_rigidMeshCandidateLookup.find(key)", storedMembershipProbe + 1);
	const auto identityCatch = snapshotFn.find("catch (...)", identityRecompute);
	const auto identityCatchOpen = identityCatch == std::string::npos
		? std::string::npos : snapshotFn.find('{', identityCatch);
	const auto identityCatchClose = matchBrace(snapshotFn, identityCatchOpen);
	const std::string identityCatchText =
		identityCatchOpen == std::string::npos || identityCatchClose == std::string::npos
			? std::string()
			: snapshotFn.substr(
				identityCatchOpen, identityCatchClose - identityCatchOpen + 1);
	Check(identityRecompute != std::string::npos &&
		storedMembershipProbe != std::string::npos &&
		recomputedMembershipProbe != std::string::npos &&
		identityCatch != std::string::npos && identityCatchClose != std::string::npos &&
		ensureReturn < storedMembershipProbe && catchClose < identityRecompute &&
		storedMembershipProbe < identityRecompute &&
		identityRecompute < recomputedMembershipProbe &&
		productionLoopText.find("ComputeRigidMeshHashesFromPresent") == std::string::npos &&
		productionLoopText.find("IdentityDiagnostic") == std::string::npos &&
		identityCatchText.find("QuarantineRigidMeshCandidateLookup") == std::string::npos &&
		identityCatchText.find("out.clear") == std::string::npos &&
		identityCatchText.find("earlyReturn") == std::string::npos &&
		identityCatchText.find("return;") == std::string::npos,
		"Registry identity diagnostic pin: post-Ensure direct membership surrounds dump-only recompute and failure cannot alter lookup/eligibility");
	const auto computeFn = universe.find(
		"int RtSmokeGeometryUniverse::ComputeRigidMeshHashesFromPresent(");
	const auto computeFnEnd = universe.find(
		"\n}\n\nvoid LogRigidMeshCandidateLookupQuarantineOnce", computeFn);
	const std::string computeFnText = computeFn == std::string::npos ||
		computeFnEnd == std::string::npos
			? std::string() : universe.substr(computeFn, computeFnEnd - computeFn);
	Check(computeFnText.find("int maxSurfaceCount") != std::string::npos &&
		computeFnText.find("std::min(modelSurfaceCount, maxSurfaceCount)") !=
			std::string::npos &&
		snapshotFn.find("RT_PT_RIGID_REGISTRY_IDENTITY_SURFACES", identityRecompute) !=
			std::string::npos,
		"Registry identity diagnostic pin: recomputation is capped at eight model surfaces per instance");
	const auto identityDumpGate = scene.find(
		"r_pathTracingCpuProducerRegistryDump.GetString()[0] != '\\0'");
	const auto identityInstanceCap = scene.find(
		"RT_PT_RIGID_REGISTRY_IDENTITY_SAMPLES", identityDumpGate);
	const auto identitySlotEpoch = scene.find(
		"PtGeometryLifecycle::EntityModelEpoch(", identityInstanceCap);
	const auto identitySlotMeshIds = scene.find(
		"instance.meshIds[keyIndex]", identitySlotEpoch);
	const auto identitySnapshotCall = scene.find(
		"identityDiagnosticRequested ? &identityDiagnosticInputs : nullptr",
		identitySlotMeshIds);
	Check(identityDumpGate != std::string::npos &&
		identityInstanceCap != std::string::npos &&
		identitySlotEpoch != std::string::npos &&
		identitySlotMeshIds != std::string::npos &&
		identitySnapshotCall != std::string::npos &&
		identityDumpGate < identityInstanceCap &&
		identityInstanceCap < identitySlotEpoch &&
		identitySlotEpoch < identitySlotMeshIds &&
		identitySlotMeshIds < identitySnapshotCall,
		"Registry identity diagnostic pin: dump-only first-eight live slot sample carries stored meshIds and exact slot modelEpoch into snapshot");
	const auto modelPersistWrapper = lifecycle.find(
		"void PersistRigidMeshFromPresent(const idRenderModel* model)");
	const auto entityPersistWrapper = lifecycle.find(
		"void PersistRigidMeshFromPresent(const idRenderEntityLocal* entity)");
	const auto notifyAdded = lifecycle.find(
		"void NotifyEntityAdded(const idRenderEntityLocal* entity)");
	const auto modelActive = lifecycle.find("ActiveRigidMeshPersistTarget()", modelPersistWrapper);
	const auto modelCallCount = lifecycle.find("NoteRigidMeshPersistCall()", modelActive);
	const auto modelNullGuard = lifecycle.find("if (!universe)", modelCallCount);
	const auto modelPersistCall = lifecycle.find("universe->PersistRigidMeshFromPresent(", modelNullGuard);
	const auto modelSurfaceCount = lifecycle.find("NoteRigidMeshPersistedSurfaces(", modelPersistCall);
	const auto entityActive = lifecycle.find("ActiveRigidMeshPersistTarget()", entityPersistWrapper);
	const auto entityCallCount = lifecycle.find("NoteRigidMeshPersistCall()", entityActive);
	const auto entityNullGuard = lifecycle.find("if (!universe)", entityCallCount);
	const auto entityPersistCall = lifecycle.find("universe->PersistRigidMeshFromPresent(", entityNullGuard);
	const auto entitySurfaceCount = lifecycle.find("NoteRigidMeshPersistedSurfaces(", entityPersistCall);
	const auto firstAddLatch = lifecycle.find("NoteRigidMeshPersistFirstEntityAdd(", notifyAdded);
	const auto firstAddRegistry = lifecycle.find("RegistryForWorld(entity->world)", notifyAdded);
	Check(modelPersistWrapper != std::string::npos &&
		entityPersistWrapper != std::string::npos && notifyAdded != std::string::npos &&
		modelActive != std::string::npos && modelCallCount != std::string::npos &&
		modelNullGuard != std::string::npos && modelPersistCall != std::string::npos &&
		modelSurfaceCount != std::string::npos && entityActive != std::string::npos &&
		entityCallCount != std::string::npos && entityNullGuard != std::string::npos &&
		entityPersistCall != std::string::npos && entitySurfaceCount != std::string::npos &&
		modelActive < modelCallCount && modelCallCount < modelNullGuard &&
		modelNullGuard < modelPersistCall && modelPersistCall < modelSurfaceCount &&
		modelSurfaceCount < entityPersistWrapper &&
		entityActive < entityCallCount && entityCallCount < entityNullGuard &&
		entityNullGuard < entityPersistCall && entityPersistCall < entitySurfaceCount &&
		entitySurfaceCount < notifyAdded &&
		firstAddLatch != std::string::npos && firstAddRegistry != std::string::npos &&
		firstAddLatch < firstAddRegistry &&
		universe.find("compare_exchange_strong") != std::string::npos &&
		universe.find("g_rigidMeshPersistCallsTotal.fetch_add") != std::string::npos &&
		universe.find("g_rigidMeshPersistedSurfacesTotal.fetch_add") != std::string::npos,
		"Registry diagnostic pin: persist entry/surface counters and first-add target latch bind production wrappers");
	const auto commitPrepared = rigidSubsystem.find(
		"bool RtSmokeGeometryUniverse::CommitRigidMeshCandidateDelta");
	const auto bindNewRecord = rigidSubsystem.find(
		"m_rigidMeshCandidateRecords.emplace_back", commitPrepared);
	const auto insertTotal = rigidSubsystem.find(
		"m_rigidMeshCandidateRecordInsertTotal = delta.workingInsertTotal", bindNewRecord);
	const auto generationAfterInsert = rigidSubsystem.find(
		"m_generation = delta.workingGeneration", bindNewRecord);
	const auto beginFrame = universe.find("void RtSmokeGeometryUniverse::BeginFrame(");
	const auto beginFrameLock = universe.find(
		"std::lock_guard<std::mutex> lock(m_rigidMeshCandidateLookupMutex)",
		beginFrame);
	const auto frameInsertStart = universe.find(
		"m_rigidMeshCandidateRecordInsertFrameStart =", beginFrameLock);
	const auto beginFrameClear = universe.find(
		"m_frameRigidMeshCandidateHashes.clear()", frameInsertStart);
	Check(commitPrepared != std::string::npos && bindNewRecord != std::string::npos &&
		insertTotal != std::string::npos && generationAfterInsert != std::string::npos &&
		commitPrepared < bindNewRecord && bindNewRecord < insertTotal &&
		bindNewRecord < generationAfterInsert && beginFrame != std::string::npos &&
		beginFrameLock != std::string::npos && frameInsertStart != std::string::npos &&
		beginFrameClear != std::string::npos && beginFrameLock < frameInsertStart &&
		frameInsertStart < beginFrameClear,
		"Registry diagnostic pin: batch commit publishes records then exact insert/generation totals, and BeginFrame snapshots the delta");
	Check(scene.find("&lookupDiagnostics") != std::string::npos &&
		scene.find("compareIn.registryRefreshAttempts") != std::string::npos &&
		scene.find("compareIn.registryUniqueMeshRequests") != std::string::npos &&
		scene.find("live.diagnosticReason = static_cast<RigidCpuMeshDiagnosticReason>") !=
			std::string::npos &&
		scene.find("registryDiagnosticHistogram[reasonIndex]") != std::string::npos,
		"Registry diagnostic: final compare input wires lookup counters and per-entry histogram");

	Check(RigidRegistrySection5Join(true, 2, 101, 101) &&
		!RigidRegistrySection5Join(false, 2, 101, 101) &&
		!RigidRegistrySection5Join(true, 0, 101, 101),
		"A5 pin: N3/gpuResident is section-5 identity+mask+selected Mesh BLAS join; handle-only fails");
	Check(!scene.empty() &&
		scene.find("FillRegistryCompareFromFinalPhysical(") != std::string::npos &&
		scene.find("n1.push_back") == std::string::npos,
		"A5 pin: SceneBuild delegates registry fill to final-physical builder; no local n1.push_back");
	const auto countText = [](const std::string& haystack, const std::string& needle) {
		size_t count = 0;
		for (size_t pos = haystack.find(needle); pos != std::string::npos;
			pos = haystack.find(needle, pos + needle.size()))
		{
			++count;
		}
		return count;
	};
	const auto finalAssign = scene.rfind("accelSubmitDesc.extraTlasInstances =");
	const auto fillFinal = scene.find("FillRegistryCompareFromFinalPhysical(", finalAssign);
	const auto unifiedCompare = scene.find("CompareSubmitSets(compareIn)", fillFinal);
	const auto unifiedPrint = scene.find("FormatCompareSizeLine(compareOut)", unifiedCompare);
	const auto unifiedS4Dump = scene.find(
		"MaybeDumpCpuProducerCompare(\n            compareIn, compareOut", unifiedCompare);
	const auto unifiedRegistryDump = scene.find(
		"MaybeDumpCpuProducerRegistry(compareIn, compareOut)", unifiedCompare);
	Check(countText(scene, "CompareSubmitSets(") == 1 &&
		finalAssign != std::string::npos && fillFinal != std::string::npos &&
		unifiedCompare != std::string::npos && unifiedPrint != std::string::npos &&
		unifiedS4Dump != std::string::npos && unifiedRegistryDump != std::string::npos &&
		finalAssign < fillFinal && fillFinal < unifiedCompare &&
		unifiedCompare < unifiedPrint && unifiedCompare < unifiedS4Dump &&
		unifiedCompare < unifiedRegistryDump &&
		scene.find("identityMatches = true") == std::string::npos,
		"A5 pin: one post-final CompareSubmitSets result feeds S4 line and both dumps");
	Check(scene.find("BuildRegistryIndependentObservations(") != std::string::npos &&
		scene.find("BuildRegistrySubmittedFromFinalPhysical(") != std::string::npos,
		"A5 pin: production calls independent pre-coalesce observations and final-physical builder");

	RigidRegistryInstanceKey finalKey;
	finalKey.world = reinterpret_cast<const void*>(0xA500);
	finalKey.worldGeneration = 1;
	finalKey.index = 7;
	finalKey.generation = 1;
	const uint64_t finalId = PackRigidRegistryInstanceId(finalKey);
	RigidSubmitBoundaryRecord finalRecord;
	finalRecord.instanceId = finalKey;
	finalRecord.meshId = 501;
	finalRecord.descriptorIndex = 0;
	finalRecord.instanceID = 77;
	finalRecord.instanceMask = 2;
	finalRecord.submittedBlasToken = 601;
	finalRecord.provenance = kOriginRegistryHook;
	finalRecord.exactIdentity = true;
	RigidSubmitBoundaryList finalMetadata;
	finalMetadata.rigidPhysicalCount = 1;
	finalMetadata.records.push_back(finalRecord);
	std::unordered_map<uint64_t, uint64_t> finalSelected;
	finalSelected[501] = 601;
	const auto evaluateFinalPhysical = [&](const std::vector<RegistryComparePhysicalExtra>& physical) {
		CompareFrameInput in;
		in.registryDecodedMode = 1;
		in.registryLive.push_back({
			finalId, 501, 601, true, true, false, false, false, true });
		BuildRegistrySubmittedFromFinalPhysical(
			finalMetadata, physical, finalSelected, in.registrySubmitted);
		return CompareSubmitSets(in);
	};
	RegistryComparePhysicalExtra finalGood;
	finalGood.descriptorIndex = 0;
	finalGood.instanceID = 77;
	finalGood.instanceMask = 2;
	finalGood.blasToken = 601;
	const CompareFrameResult finalGoodOut = evaluateFinalPhysical({ finalGood });
	RegistryComparePhysicalExtra finalMaskZero = finalGood;
	finalMaskZero.instanceMask = 0;
	RegistryComparePhysicalExtra finalBlasChanged = finalGood;
	finalBlasChanged.blasToken = 602;
	RegistryComparePhysicalExtra finalIndexShifted = finalGood;
	finalIndexShifted.descriptorIndex = 1;
	RegistryComparePhysicalExtra finalIdChanged = finalGood;
	finalIdChanged.instanceID = 78;
	Check(finalGoodOut.n3.empty() && finalGoodOut.registryGpuResidentIds.size() == 1 &&
		evaluateFinalPhysical({}).n3.size() == 1 &&
		evaluateFinalPhysical({ finalMaskZero }).n3.size() == 1 &&
		evaluateFinalPhysical({ finalBlasChanged }).n3.size() == 1 &&
		evaluateFinalPhysical({ finalIndexShifted }).n3.size() == 1 &&
		evaluateFinalPhysical({ finalIdChanged }).n3.size() == 1,
		"A5 final physical join: removed/mask-zero/BLAS-changed/index-shifted/instanceID-mismatch each N3");

	CompareFrameInput mode0;
	mode0.decodedMode = 0;
	mode0.registryDecodedMode = 0;
	mode0.registryLive.push_back({ 20, 21, 22, true, true, false, false, true, false });
	mode0.registrySubmitted.push_back({ 20, 21, 0, 99, 22, 0, false });
	mode0.registrySkipProofs.push_back({ 20, kOriginCaptureWalk, true });
	const CompareFrameResult off = CompareSubmitSets(mode0);
	Check(off.n1.empty() && off.n2.empty() && off.n3.empty() &&
		off.n4.empty() && off.n5.empty() && off.n6.empty() &&
		off.verdict == CompareVerdict::NotEvaluated,
		"A5 mode 0: N1..N6 empty, verdict not_evaluated");

	CompareFrameInput noLive;
	noLive.registryDecodedMode = 1;
	const CompareFrameResult noLiveOut = CompareSubmitSets(noLive);
	Check(!noLiveOut.registryEvaluated &&
		noLiveOut.registryVerdict == CompareVerdict::NotEvaluated &&
		noLiveOut.verdict == CompareVerdict::NotEvaluated,
		"A5 registry mode 1/no live: registry and frame not_evaluated, never PASS");
	CompareFrameInput pendingBefore;
	pendingBefore.registryDecodedMode = 1;
	pendingBefore.registryLive.push_back({
		21, 22, 0, true, false, true, false, false, true });
	const CompareFrameResult pendingBeforeOut = CompareSubmitSets(pendingBefore);
	Check(!pendingBeforeOut.registryEvaluated && pendingBeforeOut.n3.empty() &&
		pendingBeforeOut.registryVerdict == CompareVerdict::NotEvaluated &&
		pendingBeforeOut.verdict == CompareVerdict::NotEvaluated,
		"A5 MeshPending at/before E1+1: not_evaluated");
	CompareFrameInput pendingExpired;
	pendingExpired.registryDecodedMode = 1;
	pendingExpired.registryLive.push_back({
		23, 24, 0, true, false, false, true, false, true });
	const CompareFrameResult pendingExpiredOut = CompareSubmitSets(pendingExpired);
	Check(pendingExpiredOut.registryEvaluated && pendingExpiredOut.n3.size() == 1 &&
		pendingExpiredOut.registryVerdict == CompareVerdict::Fail &&
		pendingExpiredOut.verdict == CompareVerdict::Fail,
		"A5 MeshPending after deadline: N3 FAIL and fail-closed to walk");

	CompareFrameInput n1in;
	n1in.registryDecodedMode = 1;
	n1in.registryLive.push_back({ 31, 32, 33, true, true, false, false, false, true });
	const CompareFrameResult n1 = CompareSubmitSets(n1in);
	Check(n1.n1.size() == 1 && n1.n3.size() == 1 &&
		n1.n1[0] == 31 && n1.n3[0] == 31 && n1.verdict == CompareVerdict::Fail,
		"A5 N1: E1+E2, no drawSurf, no HOOK -> n1+n3 and FAIL");

	CompareFrameInput walkOnly;
	walkOnly.registryDecodedMode = 1;
	walkOnly.registryWalkObs.push_back({ 34, 35, 2, 36 });
	const CompareFrameResult walkOnlyOut = CompareSubmitSets(walkOnly);
	Check(walkOnlyOut.n1.size() == 1 && walkOnlyOut.n1[0] == 34 &&
		walkOnlyOut.verdict == CompareVerdict::Fail,
		"A5 N1: walk observation with no live Instance row -> n1 and FAIL");

	CompareFrameInput n2in;
	n2in.registryDecodedMode = 1;
	n2in.registryLive.push_back({ 41, 42, 43, true, true, false, false, false, true });
	n2in.registrySubmitted.push_back({ 41, 42, 2, 43, 43, kOriginRegistryHook, true });
	n2in.registrySubmitted.push_back({ 41, 42, 2, 43, 43, kOriginRegistryHook, true });
	const CompareFrameResult n2 = CompareSubmitSets(n2in);
	Check(n2.n2.size() == 1 && n2.verdict == CompareVerdict::Fail,
		"A5 N2: byte-identical extras same exact pair -> n2 and FAIL");
	const uint64_t legacyPairMul = 0x9E3779B97F4A7C15ull;
	const uint64_t collisionIdA = 101;
	const uint64_t collisionMeshA = 2;
	const uint64_t collisionMeshB = 3;
	const uint64_t collisionIdB = collisionIdA ^
		(collisionMeshA * legacyPairMul) ^ (collisionMeshB * legacyPairMul);
	CompareFrameInput pairCollision;
	pairCollision.registryDecodedMode = 1;
	pairCollision.registryLive.push_back({
		collisionIdA, collisionMeshA, 701, true, true, false, false, false, true });
	pairCollision.registryLive.push_back({
		collisionIdB, collisionMeshB, 702, true, true, false, false, false, true });
	pairCollision.registrySubmitted.push_back({
		collisionIdA, collisionMeshA, 2, 701, 701, kOriginRegistryHook, true });
	pairCollision.registrySubmitted.push_back({
		collisionIdB, collisionMeshB, 2, 702, 702, kOriginRegistryHook, true });
	const CompareFrameResult pairCollisionOut = CompareSubmitSets(pairCollision);
	Check(collisionIdA != collisionIdB && pairCollisionOut.n2.empty() &&
		pairCollisionOut.verdict == CompareVerdict::Pass,
		"A5 N2 exact pair: adversarial legacy-XOR collision remains legal");

	CompareFrameInput n3in;
	n3in.registryDecodedMode = 1;
	n3in.registryLive.push_back({ 51, 52, 53, true, true, false, false, false, true });
	n3in.registrySubmitted.push_back({ 51, 52, 2, 53, 53, kOriginRegistryHook, false });
	const CompareFrameResult n3 = CompareSubmitSets(n3in);
	Check(n3.n3.size() == 1 && n3.n3[0] == 51 && n3.verdict == CompareVerdict::Fail,
		"A5 N3: eligible live fails section-5 join -> n3 and FAIL (handle-only fails)");

	CompareFrameInput n4in;
	n4in.registryDecodedMode = 1;
	n4in.registryLive.push_back({ 61, 62, 63, true, true, false, false, false, true });
	n4in.registrySubmitted.push_back({
		61, 62, 2, 63, 63, kOriginCaptureWalk | kOriginRegistryHook, true });
	n4in.registryWalkObs.push_back({ 61, 63, 2, 62 });
	n4in.registryHookObs.push_back({ 61, 64, 1, 62 });
	const CompareFrameResult n4 = CompareSubmitSets(n4in);
	Check(n4.n4.size() == 1 && n4.n2.empty() && n4.verdict == CompareVerdict::Fail,
		"A5 N4: same instanceId, disagreeing BLAS or mask -> n4 and FAIL, not a second extra");

	RigidRegistryInstanceKey n4Key;
	n4Key.world = reinterpret_cast<const void*>(0xA504);
	n4Key.worldGeneration = 1;
	n4Key.index = 4;
	n4Key.generation = 1;
	RigidSubmitBoundaryRecord n4WalkRecord;
	n4WalkRecord.instanceId = n4Key;
	n4WalkRecord.meshId = 804;
	n4WalkRecord.descriptorIndex = 0;
	n4WalkRecord.instanceID = 44;
	n4WalkRecord.instanceMask = 2;
	n4WalkRecord.submittedBlasToken = 805;
	n4WalkRecord.provenance = kOriginCaptureWalk;
	n4WalkRecord.exactIdentity = true;
	RigidSubmitBoundaryList n4Boundary;
	n4Boundary.rigidPhysicalCount = 1;
	n4Boundary.records.push_back(n4WalkRecord);
	RegistryEligibleSurface n4HookCandidate;
	n4HookCandidate.instanceId = n4Key;
	n4HookCandidate.meshId = 804;
	n4HookCandidate.blasToken = 806;
	n4HookCandidate.presentRecorded = true;
	n4HookCandidate.meshBlasReady = true;
	n4HookCandidate.meshBlasBuilt = true;
	std::vector<RegistryCompareObservation> n4WalkIndependent;
	std::vector<RegistryCompareObservation> n4HookIndependent;
	BuildRegistryIndependentObservations(
		n4Boundary, 1, { n4HookCandidate }, n4WalkIndependent, n4HookIndependent);
	RegistryPublishCounters n4Counters;
	Check(EmitCoalescedRegistryTlas(
		static_cast<int>(RegistryMode::DualWrite), { n4HookCandidate },
		n4Boundary, n4Counters),
		"A5 N4 production coalesce setup keeps one physical descriptor");
	RegistryComparePhysicalExtra n4Physical;
	n4Physical.descriptorIndex = 0;
	n4Physical.instanceID = 44;
	n4Physical.instanceMask = 2;
	n4Physical.blasToken = 805;
	std::unordered_map<uint64_t, uint64_t> n4Selected;
	n4Selected[804] = 806;
	CompareFrameInput n4Production;
	n4Production.registryDecodedMode = 1;
	const uint64_t n4Packed = PackRigidRegistryInstanceId(n4Key);
	n4Production.registryLive.push_back({
		n4Packed, 804, 806, true, true, false, false, true, true });
	n4Production.registryWalkObs = n4WalkIndependent;
	n4Production.registryHookObs = n4HookIndependent;
	BuildRegistrySubmittedFromFinalPhysical(
		n4Boundary, { n4Physical }, n4Selected, n4Production.registrySubmitted);
	const CompareFrameResult n4ProductionOut = CompareSubmitSets(n4Production);
	Check(n4Boundary.records.size() == 1 && n4Production.registrySubmitted.size() == 1 &&
		n4ProductionOut.n4.size() == 1 && n4ProductionOut.n2.empty() &&
		n4ProductionOut.verdict == CompareVerdict::Fail,
		"A5 N4 production-called: pre-coalesce disagreement, one final descriptor -> N4 not N2");

	CompareFrameInput n5in;
	n5in.registryDecodedMode = 1;
	n5in.registrySubmitted.push_back({ 71, 72, 2, 73, 73, kOriginRegistryHook, true });
	const CompareFrameResult n5 = CompareSubmitSets(n5in);
	Check(n5.n5.size() == 1 && n5.n5[0] == 71 && n5.verdict == CompareVerdict::Fail,
		"A5 N5: submitted extra, no live Instance -> n5 and FAIL");

	CompareFrameInput n6in;
	n6in.registryDecodedMode = 1;
	n6in.registryLive.push_back({ 81, 82, 83, true, true, false, false, false, true });
	n6in.registryLive.push_back({ 84, 85, 86, true, true, false, false, false, true });
	n6in.registrySubmitted.push_back({ 81, 82, 2, 99, 83, kOriginCaptureWalk, true });
	n6in.registrySubmitted.push_back({ 84, 85, 2, 98, 86, 0, true });
	n6in.registrySkipProofs.push_back({ 81, kOriginCaptureWalk, true });
	n6in.registrySkipProofs.push_back({ 84, 0, true });
	const CompareFrameResult n6 = CompareSubmitSets(n6in);
	Check(n6.n6.size() == 2 && n6.n3.size() == 2 && n6.verdict == CompareVerdict::Fail,
		"A5 N6: skip proof ORIGIN_CAPTURE_WALK/untagged on a non-join instance -> n6 and FAIL");

	CompareFrameInput skipMiss;
	skipMiss.registryDecodedMode = 1;
	skipMiss.registryLive.push_back({ 91, 92, 93, true, true, false, false, false, true });
	skipMiss.captureSkipIds.push_back(91);
	const CompareFrameResult skipMissOut = CompareSubmitSets(skipMiss);
	Check(skipMissOut.n3.size() == 1 && skipMissOut.n6.size() == 1 &&
		skipMissOut.verdict == CompareVerdict::Fail,
		"A5 skip-vs-join: skip set that would drop a section-5 miss FAILs");

	const auto makeSurfaces = [](uint32_t count, const void* world) {
		std::vector<RegistryEligibleSurface> surfaces;
		for (uint32_t i = 0; i < count; ++i)
		{
			RegistryEligibleSurface surface;
			surface.instanceId.world = world;
			surface.instanceId.worldGeneration = 1;
			surface.instanceId.index = static_cast<int>(i);
			surface.instanceId.generation = 1;
			surface.meshId = 1000 + i;
			surface.blasToken = 2000 + i;
			surface.presentRecorded = true;
			surface.meshBlasReady = true;
			surface.meshBlasBuilt = true;
			surfaces.push_back(surface);
		}
		return surfaces;
	};
	RigidSubmitBoundaryList doom32Extras;
	RigidSubmitBoundaryList doom33Extras;
	RegistryPublishCounters doom32Counters;
	RegistryPublishCounters doom33Counters;
	const std::vector<RegistryEligibleSurface> doom32 =
		makeSurfaces(12, reinterpret_cast<const void*>(0x3200));
	const std::vector<RegistryEligibleSurface> doom33 =
		makeSurfaces(8, reinterpret_cast<const void*>(0x3300));
	const bool emit32 = EmitCoalescedRegistryTlas(
		static_cast<int>(RegistryMode::DualWrite), doom32, doom32Extras, doom32Counters);
	const bool emit33 = EmitCoalescedRegistryTlas(
		static_cast<int>(RegistryMode::DualWrite), doom33, doom33Extras, doom33Counters);
	Check(emit32 && emit33 &&
		doom32Counters.registryEmitted >= doom33Counters.registryEmitted,
		"A5 registryEmitted doom3_2 >= doom3_3 (not rigidPublishEmitted)");
	bool wallProductsOk = emit33 && doom33Extras.records.size() == doom33.size();
	for (size_t i = 0; wallProductsOk && i < doom33Extras.records.size(); ++i)
	{
		const RigidSubmitBoundaryRecord& rec = doom33Extras.records[i];
		wallProductsOk = RigidRegistrySection5Join(
			RigidSubmitBoundaryExact(rec), rec.instanceMask,
			rec.submittedBlasToken, doom33[i].blasToken);
	}
	Check(wallProductsOk,
		"A5 doom3_3 wall: mode-1 eligible live rigids still have a section-5 product");
}

void TestCaseCWalkSet()
{
	using namespace cpu_producer_publish;

	// static 1 = Case C static-bake after surface->bucket join;
	// merged 11 = Case C; 12 = headroom/descriptor;
	// 10 = skip; 30 = omitted-skin.
	std::vector<WalkedSurface> walkedStatic = {
		{ 1, 4, CaseCIdentityDomain::StaticSurface } };
	std::vector<WalkedSurface> walkedMerged = { { 10, 2 }, { 11, 6 }, { 12, 8 } };
	std::unordered_set<uint64_t> skip = { 10 };
	std::unordered_set<uint64_t> omitSkin = { 30 };
	std::unordered_set<uint64_t> extra = { 12 };
	std::unordered_set<uint64_t> restore;
	StaticSurfaceProductSet submittedStatic;
	submittedStatic.ids.insert(1);
	const CaseCWalkSet c = CountCaseCWalkSet(
		walkedStatic, walkedMerged, skip, omitSkin, extra, restore, false,
		submittedStatic);

	Check(c.staticBakeOnly() == 1 && c.staticBakeOnlyIds[0] == 1 &&
		c.staticBakeTriangles == 4,
		"C0 static-bake-only is the walked static key");
	Check(c.mergedDynamicOnly() == 1 && c.mergedDynamicOnlyIds[0] == 11 &&
		c.mergedDynamicTriangles == 6,
		"C0 merged-dynamic-only is the modeled Case C key");

	bool subsetWalked = true;
	bool hasDesc = false;
	bool hasSkip = false;
	for (uint64_t id : c.staticBakeOnlyIds)
	{
		bool found = false;
		for (const WalkedSurface& s : walkedStatic)
		{
			found = found || s.id == id;
		}
		subsetWalked = subsetWalked && found;
	}
	for (uint64_t id : c.mergedDynamicOnlyIds)
	{
		bool found = false;
		for (const WalkedSurface& s : walkedMerged)
		{
			found = found || s.id == id;
		}
		subsetWalked = subsetWalked && found;
		hasDesc = hasDesc || extra.find(id) != extra.end();
		hasSkip = hasSkip || skip.find(id) != skip.end() ||
			omitSkin.find(id) != omitSkin.end();
	}
	Check(subsetWalked, "C0 Case C is a subset of walked");
	Check(!hasDesc, "C0 Case C has no per-instance descriptor");
	Check(!hasSkip, "C0 Case C is not in the skip/omit set");
	Check(std::find(c.mergedDynamicOnlyIds.begin(), c.mergedDynamicOnlyIds.end(), 12ull) ==
			c.mergedDynamicOnlyIds.end(),
		"C0 headroom/descriptor key is not Case C");
	Check(std::find(c.mergedDynamicOnlyIds.begin(), c.mergedDynamicOnlyIds.end(), 10ull) ==
			c.mergedDynamicOnlyIds.end(),
		"C0 skip key is not Case C");
}

void TestCaseCStaticJoin()
{
	using namespace cpu_producer_publish;

	// Surface 100 -> submitted active bucket 7.
	// 200 -> active/exact bucket 8 with the same instanceId as 7 but a
	//        different BLAS; extraTlas has 7's descriptor plus a rigid
	//        descriptor that reuses instanceId 99. Must not count 200.
	// 300 -> missing-BLAS / not exactReady.
	// 400 -> inactive bucket.
	// 500 -> exact/active but descriptor not in extraTlas (frozen / not submit).
	// 1   -> walked static whose key equals a rigid extra source ID.
	const std::vector<StaticBucketProductJoinSurface> surfaces = {
		{ 100, 7 },
		{ 200, 8 },
		{ 300, 9 },
		{ 400, 10 },
		{ 500, 11 },
		{ 1, 12 },
	};
	std::vector<StaticBucketProductJoinBucket> buckets = {
		{ 7, true, true, 1u, 99u, 0xAull },
		{ 8, true, true, 1u, 99u, 0xBull },
		{ 9, true, false, 1u, 88u, 0xCull },
		{ 10, false, true, 1u, 77u, 0xDull },
		{ 11, true, true, 1u, 66u, 0xEull },
		{ 12, true, true, 1u, 55u, 0x10ull },
	};
	const std::vector<StaticBucketSubmittedDescriptor> submitted = {
		{ 99u, 1u, 0xAull },
		{ 99u, 1u, 0xFull },
		{ 1u, 1u, 0xFull },
	};

	const StaticSurfaceProductSet product =
		BuildSubmittedStaticSurfaceProductSet(surfaces, buckets, submitted);
	Check(product.ids.count(100) == 1,
		"C0 static join: walked surface in submitted active bucket COUNTS");
	Check(product.ids.count(200) == 0,
		"C0 static join: same instanceId different BLAS does not manufacture");
	Check(product.ids.count(300) == 0,
		"C0 static join: missing-BLAS / not exactReady does not count");
	Check(product.ids.count(400) == 0,
		"C0 static join: inactive bucket does not count");
	Check(product.ids.count(500) == 0,
		"C0 static join: not-in-submit / frozen bucket does not count");
	Check(product.ids.count(1) == 0,
		"C0 static join: rigid numeric ID does not manufacture a static product");

	std::vector<WalkedSurface> walkedStatic = {
		{ 100, 3, CaseCIdentityDomain::StaticSurface },
		{ 1, 2, CaseCIdentityDomain::StaticSurface },
		{ 200, 4, CaseCIdentityDomain::StaticSurface },
		{ 300, 1, CaseCIdentityDomain::StaticSurface },
		{ 400, 1, CaseCIdentityDomain::StaticSurface },
		{ 500, 1, CaseCIdentityDomain::StaticSurface },
	};
	std::unordered_set<uint64_t> skip;
	std::unordered_set<uint64_t> omit;
	std::unordered_set<uint64_t> extra = { 1, 99 };
	std::unordered_set<uint64_t> restore;
	const CaseCWalkSet c = CountCaseCWalkSet(
		walkedStatic, {}, skip, omit, extra, restore, true, product);
	Check(c.staticBakeOnly() == 1 && c.staticBakeOnlyIds[0] == 100 &&
		c.staticBakeTriangles == 3,
		"C0 staticBakeOnly is the submitted-bucket walked surface");
	Check(std::find(c.staticBakeOnlyIds.begin(), c.staticBakeOnlyIds.end(), 1ull) ==
			c.staticBakeOnlyIds.end(),
		"C0 rigid/bucket numeric IDs cannot hide or manufacture staticBakeOnly");
	Check(std::find(c.staticBakeOnlyIds.begin(), c.staticBakeOnlyIds.end(), 200ull) ==
			c.staticBakeOnlyIds.end() &&
		std::find(c.staticBakeOnlyIds.begin(), c.staticBakeOnlyIds.end(), 300ull) ==
			c.staticBakeOnlyIds.end() &&
		std::find(c.staticBakeOnlyIds.begin(), c.staticBakeOnlyIds.end(), 400ull) ==
			c.staticBakeOnlyIds.end() &&
		std::find(c.staticBakeOnlyIds.begin(), c.staticBakeOnlyIds.end(), 500ull) ==
			c.staticBakeOnlyIds.end(),
		"C0 unpublished static buckets are not staticBakeOnly");

	const StaticSurfaceProductSet frozen =
		BuildSubmittedStaticSurfaceProductSet(surfaces, buckets, {});
	Check(frozen.ids.empty(),
		"C0 static join: empty extraTlas (frozen/not-submit) yields no product");

	buckets[0].instanceMask = 0;
	const StaticSurfaceProductSet zeroMask =
		BuildSubmittedStaticSurfaceProductSet(surfaces, buckets, submitted);
	Check(zeroMask.ids.count(100) == 0,
		"C0 static join: zero instance mask does not count");
}


void TestMergedCaseCWalkSet()
{
	using namespace cpu_producer_publish;

	MergedSubmittedProduct product;
	product.hasDynamicBlas = true;
	product.dynamicBlasNonNull = true;
	product.dynamicBlasBuiltThisFrameFromVectors = true;
	product.tlasTraceable = true;
	product.instanceMask = kMergedDynamicTlasInstanceMask;
	product.blasVertexCount = 100;
	product.blasIndexCount = 300;
	product.bucketIntervals.push_back({ 0, 300, 0, 100 });

	MergedWalkedRange covered;
	covered.id = 11;
	covered.domain = CaseCIdentityDomain::MergedDynamic;
	covered.vertexBegin = 0;
	covered.vertexCount = 10;
	covered.indexBegin = 0;
	covered.indexCount = 30;
	covered.triangleCount = 10;

	MergedWalkedRange outsideBuckets = covered;
	outsideBuckets.id = 12;
	outsideBuckets.indexBegin = 400;
	outsideBuckets.indexCount = 30;

	MergedWalkedRange outsideBlas = covered;
	outsideBlas.id = 13;
	outsideBlas.indexBegin = 0;
	outsideBlas.indexCount = 400;

	MergedWalkedRange skipped = covered;
	skipped.id = 14;

	MergedWalkedRange descriptor = covered;
	descriptor.id = 15;

	MergedWalkedRange particle = covered;
	particle.id = 16;
	particle.particle = true;
	particle.triangleCount = 3;

	MergedWalkedRange deform = covered;
	deform.id = 17;
	deform.trueDeform = true;
	deform.triangleCount = 2;

	MergedWalkedRange rigidLookalike = covered;
	rigidLookalike.id = 99;

	MergedWalkedRange wrongDomain = covered;
	wrongDomain.id = 18;
	wrongDomain.domain = CaseCIdentityDomain::MergedRigid;

	std::vector<MergedWalkedRange> walked = {
		covered, outsideBuckets, outsideBlas, skipped, descriptor,
		particle, deform, wrongDomain
	};
	std::unordered_set<uint64_t> skip = { 14 };
	std::unordered_set<uint64_t> omit;
	std::unordered_set<uint64_t> mergedDesc = { 15 };
	const MergedCaseCWalkSet c = CountMergedCaseCWalkSet(
		walked, skip, omit, mergedDesc, product);

	Check(c.covered() == 3 &&
		std::find(c.coveredIds.begin(), c.coveredIds.end(), 11ull) != c.coveredIds.end() &&
		std::find(c.coveredIds.begin(), c.coveredIds.end(), 16ull) != c.coveredIds.end() &&
		std::find(c.coveredIds.begin(), c.coveredIds.end(), 17ull) != c.coveredIds.end(),
		"M0 covered is walked range coverage (particle/deform still covered)");
	Check(c.excludedParticle == 1 && c.excludedDeform == 1,
		"M0 particle and true deform excluded from stable, reported separately");
	Check(c.stable() == 0,
		"M0 stable is 0 unless a same-frame stability signal is measured");
	Check(std::find(c.coveredIds.begin(), c.coveredIds.end(), 12ull) == c.coveredIds.end(),
		"M0 out-of-bucket-range does not count");
	Check(std::find(c.coveredIds.begin(), c.coveredIds.end(), 13ull) == c.coveredIds.end(),
		"M0 out-of-BLAS-extent does not count");
	Check(std::find(c.coveredIds.begin(), c.coveredIds.end(), 14ull) == c.coveredIds.end(),
		"M0 skip key is not covered");
	Check(std::find(c.coveredIds.begin(), c.coveredIds.end(), 15ull) == c.coveredIds.end(),
		"M0 Case A / descriptor key is not covered");
	Check(std::find(c.coveredIds.begin(), c.coveredIds.end(), 18ull) == c.coveredIds.end(),
		"M0 MergedRigid domain is not MergedDynamic coverage");

	bool subset = true;
	for (uint64_t id : c.coveredIds)
	{
		bool found = false;
		for (const MergedWalkedRange& r : walked)
		{
			found = found || (r.id == id && r.domain == CaseCIdentityDomain::MergedDynamic);
		}
		subset = subset && found;
	}
	Check(subset, "M0 covered is a subset of walked MergedDynamic");

	MergedSubmittedProduct blasOnly = product;
	blasOnly.dynamicBlasBuiltThisFrameFromVectors = false;
	const MergedCaseCWalkSet blasOnlyOut = CountMergedCaseCWalkSet(
		{ covered }, {}, {}, {}, blasOnly);
	Check(blasOnlyOut.covered() == 0,
		"M0 hasDynamicBlas-only / no this-frame vector BLAS does not count");

	MergedSubmittedProduct noMask = product;
	noMask.tlasTraceable = false;
	noMask.instanceMask = 0;
	const MergedCaseCWalkSet noMaskOut = CountMergedCaseCWalkSet(
		{ covered }, {}, {}, {}, noMask);
	Check(noMaskOut.covered() == 0,
		"M0 non-traceable / zero mask is not a product");

	// Rigid numeric ID 99 is in extra/skip/omit/restore. It must not
	// manufacture a merged hit when the range is outside, nor hide a
	// covered merged 99.
	std::unordered_set<uint64_t> rigidIds = { 99 };
	MergedWalkedRange outside99 = outsideBuckets;
	outside99.id = 99;
	const MergedCaseCWalkSet noManufacture = CountMergedCaseCWalkSet(
		{ outside99 }, rigidIds, rigidIds, rigidIds, product);
	Check(noManufacture.covered() == 0,
		"M0 rigid numeric ID cannot manufacture a merged hit");
	MergedWalkedRange covered99 = covered;
	covered99.id = 99;
	const MergedCaseCWalkSet noHide = CountMergedCaseCWalkSet(
		{ covered99 }, {}, {}, {}, product);
	Check(noHide.covered() == 1 && noHide.coveredIds[0] == 99,
		"M0 rigid numeric ID cannot hide a covered merged range");

	MergedSubmittedProduct gapped = product;
	gapped.bucketIntervals.clear();
	gapped.bucketIntervals.push_back({ 0, 30, 0, 10 });
	gapped.bucketIntervals.push_back({ 100, 30, 40, 10 });
	MergedWalkedRange inGap = covered;
	inGap.id = 21;
	inGap.indexBegin = 40;
	inGap.indexCount = 30;
	inGap.vertexBegin = 15;
	inGap.vertexCount = 10;
	const MergedCaseCWalkSet gapOut = CountMergedCaseCWalkSet(
		{ inGap }, {}, {}, {}, gapped);
	Check(gapOut.covered() == 0,
		"M0 range in a gap between non-contiguous buckets does not count");

	MergedSubmittedProduct shortBlas = product;
	const MergedBlasExtent shortExtent = ReadSubmittedDynamicBlasExtents(
		{ { 100, 15 } });
	shortBlas.blasVertexCount = shortExtent.vertexCount;
	shortBlas.blasIndexCount = shortExtent.indexCount;
	const MergedCaseCWalkSet shortOut = CountMergedCaseCWalkSet(
		{ covered }, {}, {}, {}, shortBlas);
	Check(shortOut.covered() == 0 &&
		shortBlas.blasIndexCount == 15 &&
		product.bucketIntervals[0].indexCount == 300,
		"M0 submitted BLAS desc short vs large CPU buckets does not count");

	MergedWalkedRange mapped = covered;
	mapped.id = 31;
	mapped.companionRigidId = 99;
	const MergedDynamicExclusionSets built = BuildMergedDynamicExclusionSets(
		{ mapped },
		{ 77 },
		{},
		{ 99 },
		{});
	Check(built.descriptorIds.count(31) == 1 &&
		built.skipIds.count(31) == 0,
		"M0 builder maps companion rigid extra to MergedDynamic descriptor");
	const MergedCaseCWalkSet mappedOut = CountMergedCaseCWalkSet(
		{ mapped }, built.skipIds, built.omitIds, built.descriptorIds, product);
	Check(mappedOut.covered() == 0,
		"M0 builder descriptor exclusion removes Case A merged surface");
	const MergedCaseCWalkSet emptyWouldCount = CountMergedCaseCWalkSet(
		{ mapped }, {}, {}, {}, product);
	Check(emptyWouldCount.covered() == 1,
		"M0 empty live exclusion sets would still count Case A (why wiring is required)");

	const fs::path sceneBuildFromHarness =
		fs::path(__FILE__).parent_path() / ".." / "renderer" / "NVRHI" /
		"PathTraceSmokeSceneBuild.cpp";
	const fs::path sceneBuildAbs(
		"E:/prog/rbdoom-3-bfg-rt-producer/neo/renderer/NVRHI/PathTraceSmokeSceneBuild.cpp");
	std::string sceneText;
	for (const fs::path& candidate : { sceneBuildFromHarness, sceneBuildAbs })
	{
		std::ifstream sceneIn(candidate);
		if (!sceneIn)
		{
			continue;
		}
		sceneText.assign(std::istreambuf_iterator<char>(sceneIn),
			std::istreambuf_iterator<char>());
		break;
	}
	const auto buildPos = sceneText.find("BuildMergedDynamicExclusionSets");
	std::string buildCall;
	if (buildPos != std::string::npos)
	{
		const auto buildEnd = sceneText.find(';', buildPos);
		if (buildEnd != std::string::npos)
		{
			buildCall = sceneText.substr(buildPos, buildEnd - buildPos);
		}
	}
	const auto extentPos = sceneText.find("ReadSubmittedDynamicBlasExtents");
	const bool emptySkipLiteral =
		sceneText.find("const std::unordered_set<uint64_t> mergedDomainSkip;") !=
		std::string::npos;
	const bool cpuCountExtent =
		sceneText.find("std::max(0, dynamicVertexCount)") != std::string::npos;
	Check(!sceneText.empty() &&
		buildCall.find("headroomSkip") != std::string::npos &&
		buildCall.find("headroomOmitSkin") != std::string::npos &&
		buildCall.find("rigidSubmittedExtras") != std::string::npos &&
		buildCall.find("submittedSkinnedSet") != std::string::npos &&
		extentPos != std::string::npos &&
		!emptySkipLiteral &&
		!cpuCountExtent,
		"M0 production caller uses BuildMergedDynamicExclusionSets and submitted BLAS extents");
}

void TestHeadroom()
{
	using namespace cpu_producer_publish;

	// walked: 10 (skip), 11 (Case C, no product), 12 (headroom rigid), 20 (headroom skinned)
	// skip/omit: 10, 30
	// submitted extras: 12, 20, 99 (99 not walked)
	std::vector<uint64_t> walkedRigid = { 10, 11, 12 };
	std::vector<uint64_t> walkedSkinned = { 20 };
	std::unordered_set<uint64_t> skip = { 10 };
	std::unordered_set<uint64_t> omitSkin = { 30 };
	std::unordered_set<uint64_t> extra = { 12, 20, 99 };
	std::unordered_set<uint64_t> restore;
	const HeadroomResult hr = CountHeadroom(
		walkedRigid, walkedSkinned, skip, omitSkin, extra, restore, false);

	Check(hr.total() == 2 && hr.rigid() == 1 && hr.skinned() == 1,
		"H0 headroom counts walked-and-submitted only");
	Check(hr.rigidIds.size() == 1 && hr.rigidIds[0] == 12,
		"H0 rigid headroom is the walked submitted key");
	Check(hr.skinnedIds.size() == 1 && hr.skinnedIds[0] == 20,
		"H0 skinned headroom is the walked submitted key");

	bool subsetWalked = true;
	bool subsetSubmitted = true;
	bool hasSkip = false;
	bool hasCaseC = false;
	bool hasUnwalked = false;
	for (uint64_t id : hr.rigidIds)
	{
		subsetWalked = subsetWalked &&
			(std::find(walkedRigid.begin(), walkedRigid.end(), id) != walkedRigid.end());
		subsetSubmitted = subsetSubmitted && extra.find(id) != extra.end();
		hasSkip = hasSkip || skip.find(id) != skip.end();
		hasCaseC = hasCaseC || id == 11;
		hasUnwalked = hasUnwalked || id == 99;
	}
	for (uint64_t id : hr.skinnedIds)
	{
		subsetWalked = subsetWalked &&
			(std::find(walkedSkinned.begin(), walkedSkinned.end(), id) != walkedSkinned.end());
		subsetSubmitted = subsetSubmitted && extra.find(id) != extra.end();
		hasSkip = hasSkip || omitSkin.find(id) != omitSkin.end() ||
			skip.find(id) != skip.end();
		hasUnwalked = hasUnwalked || id == 99;
	}
	Check(subsetWalked, "H0 headroom is a subset of walked");
	Check(subsetSubmitted, "H0 headroom is a subset of submitted products");
	Check(!hasSkip, "H0 headroom never includes the existing omit/skip set");
	Check(!hasCaseC, "H0 headroom never counts Case C");
	Check(!hasUnwalked, "H0 headroom never includes unwalked submitted keys");

	std::unordered_set<uint64_t> restoreOnly = { 11 };
	const HeadroomResult viaRestore = CountHeadroom(
		walkedRigid, walkedSkinned, skip, omitSkin, extra, restoreOnly, true);
	Check(std::find(viaRestore.rigidIds.begin(), viaRestore.rigidIds.end(), 11ull) !=
			viaRestore.rigidIds.end(),
		"H0 restore-committed + hasDynamicBlas is a submitted product");
}

void TestRigidMeshPersist()
{
	using namespace cpu_producer_publish;

	const uint64_t modelIdentity = 0x1000ULL;
	const uint64_t ambientCache = 0xA11CULL;
	const uint64_t indexCache = 0xB22CULL;
	const uint64_t cpuVerts = 0xC33EULL;
	const uint64_t cpuIndexes = 0xD44EULL;
	const int numVerts = 8;
	const int numIndexes = 12;
	const uint32_t vertexFormat = 1;
	const uint32_t materialId = 42;
	const uint32_t routedSignature = 0x51C1u;
	const uint32_t sourceKind = 1;
	const int surfaceIndex = 0;

	const RigidMeshIdentityInputs persistInitial = MakeRigidMeshIdentity(
		modelIdentity,
		kRigidMeshInitialModelEpoch,
		surfaceIndex,
		ambientCache,
		indexCache,
		numVerts,
		numIndexes,
		vertexFormat,
		materialId,
		routedSignature,
		sourceKind);
	const RigidMeshIdentityInputs routeInitial = MakeRigidMeshIdentity(
		modelIdentity,
		kRigidMeshInitialModelEpoch,
		surfaceIndex,
		ambientCache,
		indexCache,
		numVerts,
		numIndexes,
		vertexFormat,
		materialId,
		routedSignature,
		sourceKind);
	const uint32_t swappedEpoch = kRigidMeshInitialModelEpoch + 1u;
	const RigidMeshIdentityInputs persistSwapped = MakeRigidMeshIdentity(
		modelIdentity,
		swappedEpoch,
		surfaceIndex,
		ambientCache,
		indexCache,
		numVerts,
		numIndexes,
		vertexFormat,
		materialId,
		routedSignature,
		sourceKind);
	const RigidMeshIdentityInputs routeSwapped = MakeRigidMeshIdentity(
		modelIdentity,
		swappedEpoch,
		surfaceIndex,
		ambientCache,
		indexCache,
		numVerts,
		numIndexes,
		vertexFormat,
		materialId,
		routedSignature,
		sourceKind);

	Check(HashRigidMeshIdentity(persistInitial) == HashRigidMeshIdentity(routeInitial) &&
			HashRigidMeshIdentity(persistInitial) != 0,
		"A2 Present and rigid route share meshHash at initial EntityModelEpoch");
	Check(HashRigidMeshIdentity(persistSwapped) == HashRigidMeshIdentity(routeSwapped) &&
			HashRigidMeshIdentity(persistSwapped) != HashRigidMeshIdentity(persistInitial),
		"A2 Present and rigid route share meshHash after model-swap epoch");

	std::unordered_map<uint64_t, RigidMeshCandidateJoinRecord> table;
	RigidMeshCandidateJoinRecord persistRec;
	RigidMeshCandidateJoinRecord routeRec;
	const uint64_t persistHash = FindOrCreateRigidMeshCandidateByIdentity(
		table, persistInitial, &persistRec);
	const uint64_t routeHash = FindOrCreateRigidMeshCandidateByIdentity(
		table, routeInitial, &routeRec);
	Check(persistHash == routeHash && table.size() == 1 &&
			persistRec.meshHash == routeRec.meshHash &&
			persistRec.modelEpoch == kRigidMeshInitialModelEpoch &&
			persistRec.materialClassSignature == routedSignature &&
			persistRec.vertexBufferIdentity == RigidMeshVertexCacheIdentity(ambientCache),
		"A2 Present and rigid route join the same Mesh record at initial epoch");

	RigidMeshCandidateJoinRecord persistSwapRec;
	RigidMeshCandidateJoinRecord routeSwapRec;
	const uint64_t persistSwapHash = FindOrCreateRigidMeshCandidateByIdentity(
		table, persistSwapped, &persistSwapRec);
	const uint64_t routeSwapHash = FindOrCreateRigidMeshCandidateByIdentity(
		table, routeSwapped, &routeSwapRec);
	Check(persistSwapHash == routeSwapHash && table.size() == 2 &&
			persistSwapRec.modelEpoch == swappedEpoch &&
			persistSwapHash != persistHash,
		"A2 Present and rigid route join the same Mesh record after model swap");

	const RigidMeshIdentityInputs splitVerts = MakeRigidMeshIdentity(
		modelIdentity,
		kRigidMeshInitialModelEpoch,
		surfaceIndex,
		cpuVerts,
		cpuIndexes,
		numVerts,
		numIndexes,
		vertexFormat,
		materialId,
		routedSignature,
		sourceKind);
	Check(HashRigidMeshIdentity(splitVerts) != HashRigidMeshIdentity(routeInitial),
		"A2 split hash verts vs ambientCache differs");

	const RigidMeshIdentityInputs splitEpoch = MakeRigidMeshIdentity(
		modelIdentity,
		0,
		surfaceIndex,
		ambientCache,
		indexCache,
		numVerts,
		numIndexes,
		vertexFormat,
		materialId,
		routedSignature,
		sourceKind);
	Check(HashRigidMeshIdentity(splitEpoch) != HashRigidMeshIdentity(routeInitial),
		"A2 split hash epoch 0 vs EntityModelEpoch differs");

	const RigidMeshIdentityInputs defectPersist = MakeRigidMeshIdentity(
		modelIdentity,
		0,
		surfaceIndex,
		cpuVerts,
		cpuIndexes,
		numVerts,
		numIndexes,
		vertexFormat,
		materialId,
		0,
		sourceKind);
	Check(HashRigidMeshIdentity(defectPersist) != HashRigidMeshIdentity(routeInitial),
		"A2 defect persist (verts/signature0/epoch0) != route meshHash");

	const fs::path lifeFromHarness =
		fs::path(__FILE__).parent_path() / ".." / "renderer" / "NVRHI" /
		"PathTraceGeometryLifecycle.cpp";
	const fs::path lifeAbs(
		"E:/prog/rbdoom-3-bfg-rt-producer/neo/renderer/NVRHI/PathTraceGeometryLifecycle.cpp");
	const fs::path uniFromHarness =
		fs::path(__FILE__).parent_path() / ".." / "renderer" / "NVRHI" /
		"PathTraceGeometryUniverse.cpp";
	const fs::path uniAbs(
		"E:/prog/rbdoom-3-bfg-rt-producer/neo/renderer/NVRHI/PathTraceGeometryUniverse.cpp");
	const fs::path rigidSubsystemFromHarness =
		fs::path(__FILE__).parent_path() / ".." / "renderer" / "NVRHI" /
		"PathTraceRigidCandidatePreparedDelta.cpp";
	const fs::path rigidSubsystemAbs(
		"E:/prog/rbdoom-3-bfg-rt-producer/neo/renderer/NVRHI/PathTraceRigidCandidatePreparedDelta.cpp");
	const fs::path modelFromHarness =
		fs::path(__FILE__).parent_path() / ".." / ".." / "renderer" / "Model.cpp";
	const fs::path modelAbs(
		"E:/prog/rbdoom-3-bfg-rt-producer/neo/renderer/Model.cpp");
	const fs::path identFromHarness =
		fs::path(__FILE__).parent_path() / ".." / "renderer" / "NVRHI" /
		"PathTraceRigidIdentity.cpp";
	const fs::path identAbs(
		"E:/prog/rbdoom-3-bfg-rt-producer/neo/renderer/NVRHI/PathTraceRigidIdentity.cpp");
	const fs::path captureFromHarness =
		fs::path(__FILE__).parent_path() / ".." / "renderer" / "NVRHI" /
		"PathTraceDrawSurfCapture.cpp";
	const fs::path captureAbs(
		"E:/prog/rbdoom-3-bfg-rt-producer/neo/renderer/NVRHI/PathTraceDrawSurfCapture.cpp");
	auto readText = [](const fs::path& a, const fs::path& b) -> std::string {
		for (const fs::path& candidate : { a, b })
		{
			std::ifstream in(candidate);
			if (!in)
			{
				continue;
			}
			return std::string(std::istreambuf_iterator<char>(in),
				std::istreambuf_iterator<char>());
		}
		return std::string();
	};
	const std::string lifeText = readText(lifeFromHarness, lifeAbs);
	const std::string uniText = readText(uniFromHarness, uniAbs);
	const std::string rigidSubsystemText =
		readText(rigidSubsystemFromHarness, rigidSubsystemAbs);
	const std::string modelText = readText(modelFromHarness, modelAbs);
	const std::string identText = readText(identFromHarness, identAbs);
	const std::string captureText = readText(captureFromHarness, captureAbs);

	const auto addPos = lifeText.find("void NotifyEntityAdded");
	std::string added;
	if (addPos != std::string::npos)
	{
		added = lifeText.substr(addPos, 1800);
	}
	Check(!added.empty() &&
		added.find("PersistRigidMeshFromPresent") != std::string::npos,
		"A2 source-pin: NotifyEntityAdded calls PersistRigidMeshFromPresent");

	const auto updPos = lifeText.find("void NotifyEntityUpdated");
	std::string updated;
	if (updPos != std::string::npos)
	{
		updated = lifeText.substr(updPos, 2800);
	}
	Check(!updated.empty() &&
		updated.find("PersistRigidMeshFromPresent") != std::string::npos,
		"A2 source-pin: NotifyEntityUpdated calls PersistRigidMeshFromPresent");

	const auto entityPersistPos = lifeText.find(
		"void PersistRigidMeshFromPresent(const idRenderEntityLocal* entity)");
	std::string entityPersistFn;
	if (entityPersistPos != std::string::npos)
	{
		entityPersistFn = lifeText.substr(entityPersistPos, 700);
	}
	Check(!entityPersistFn.empty() &&
		entityPersistFn.find("EntityModelEpoch") != std::string::npos,
		"A2 source-pin: entity persist hashes EntityModelEpoch");

	const auto modelPersistPos = lifeText.find(
		"void PersistRigidMeshFromPresent(const idRenderModel* model)");
	std::string modelPersistFn;
	if (modelPersistPos != std::string::npos)
	{
		modelPersistFn = lifeText.substr(modelPersistPos, 500);
	}
	Check(!modelPersistFn.empty() &&
		modelPersistFn.find("kRigidMeshInitialModelEpoch") != std::string::npos,
		"A2 source-pin: model-load persist uses initial EntityModelEpoch 1");

	const auto persistPos = uniText.find(
		"int RtSmokeGeometryUniverse::PersistRigidMeshFromPresent");
	std::string persistFn;
	if (persistPos != std::string::npos)
	{
		persistFn = uniText.substr(persistPos, 5000);
	}
	Check(!persistFn.empty() &&
		persistFn.find("RecordRigidMeshCandidate(observation)") != std::string::npos,
		"A2 source-pin: persist calls RecordRigidMeshCandidate");
	Check(!persistFn.empty() &&
		persistFn.find("FillPathTraceRigidRouteMeshKey") != std::string::npos,
		"A2 source-pin: persist uses FillPathTraceRigidRouteMeshKey");
	Check(!persistFn.empty() &&
		persistFn.find("SmokeMaterialRouteClassSignature") != std::string::npos,
		"A2 source-pin: persist hashes routed material-class signature");
	Check(!persistFn.empty() &&
		persistFn.find("model, modelEpoch") != std::string::npos &&
		persistFn.find("model, 0,") == std::string::npos,
		"A2 source-pin: persist hashes modelEpoch not 0");
	Check(!persistFn.empty() &&
		persistFn.find("reinterpret_cast<uintptr_t>(tri->verts)") == std::string::npos &&
		persistFn.find("reinterpret_cast<uintptr_t>(tri->indexes)") == std::string::npos,
		"A2 source-pin: persist does not hash CPU verts/indexes");
	const auto recPos = rigidSubsystemText.find(
		"void RtSmokeGeometryUniverse::RecordRigidMeshCandidate");
	std::string recFn;
	if (recPos != std::string::npos)
	{
		recFn = rigidSubsystemText.substr(recPos, 800);
	}
	Check(!recFn.empty() &&
		recFn.find("PrepareRigidMeshCandidateDelta") != std::string::npos &&
		recFn.find("CommitRigidMeshCandidateDelta") != std::string::npos,
		"A2 source-pin: RecordRigidMeshCandidate uses prepared-delta authority");
	Check(!persistFn.empty() &&
		persistFn.find("drawSurf_t") == std::string::npos &&
		persistFn.find("drawSurfIndex = -1") != std::string::npos,
		"A2 source-pin: persist stores no drawSurf_t*");
	Check(!modelText.empty() &&
		modelText.find("PersistRigidMeshFromPresent") != std::string::npos,
		"A2 source-pin: model load/spawn calls PersistRigidMeshFromPresent");

	const auto fillPos = identText.find("void FillPathTraceRigidRouteMeshKey");
	std::string fillFn;
	if (fillPos != std::string::npos)
	{
		fillFn = identText.substr(fillPos, 1200);
	}
	Check(!fillFn.empty() &&
		fillFn.find("ambientCache") != std::string::npos &&
		fillFn.find("indexCache") != std::string::npos &&
		fillFn.find("tri->verts") == std::string::npos &&
		fillFn.find("tri->indexes") == std::string::npos,
		"A2 source-pin: Fill uses ambientCache/indexCache not verts/indexes");

	const auto hashPos = identText.find(
		"uint64 BuildPathTraceRigidMeshHashFromPod");
	std::string hashFn;
	if (hashPos != std::string::npos)
	{
		hashFn = identText.substr(hashPos, 1200);
	}
	Check(!hashFn.empty() &&
		hashFn.find("MakeRigidMeshIdentity") != std::string::npos &&
		hashFn.find("HashRigidMeshIdentity") != std::string::npos,
		"A2 source-pin: pointer-free rigid mesh kernel uses shared HashRigidMeshIdentity");

	Check(!captureText.empty() &&
		captureText.find("FillPathTraceRigidRouteMeshKey") != std::string::npos &&
		uniText.find("FillPathTraceRigidRouteMeshKey") != std::string::npos,
		"A2 source-pin: rigid route observation uses FillPathTraceRigidRouteMeshKey");
}

void TestRigidRegistryInstance()
{
	using namespace cpu_producer_publish;

	const void* world = reinterpret_cast<const void*>(0x11);
	RigidRegistryInstanceKey g1;
	g1.world = world;
	g1.worldGeneration = 7;
	g1.index = 3;
	g1.generation = 1;
	RigidRegistryInstanceKey g2 = g1;
	g2.generation = 2;

	RigidRegistryXform xformA;
	xformA.origin[0] = 1.0f;
	xformA.axis[0] = 1.0f;
	RigidRegistryXform xformB;
	xformB.origin[0] = 8.0f;
	xformB.axis[0] = 1.0f;

	const uint64_t meshId0 = HashRigidMeshIdentity(MakeRigidMeshIdentity(
		0x1000ULL, kRigidMeshInitialModelEpoch, 0,
		0xA11CULL, 0xB22CULL, 8, 12, 1, 42, 0x51C1u, 1));
	const uint64_t meshId1 = HashRigidMeshIdentity(MakeRigidMeshIdentity(
		0x1000ULL, kRigidMeshInitialModelEpoch, 1,
		0xA22DULL, 0xB33DULL, 16, 24, 1, 99, 0x62D2u, 1));
	const uint64_t remesh0 = HashRigidMeshIdentity(MakeRigidMeshIdentity(
		0x2000ULL, kRigidMeshInitialModelEpoch + 1u, 0,
		0xC11CULL, 0xD22CULL, 4, 6, 1, 7, 0x73E3u, 1));
	const uint64_t remesh1 = HashRigidMeshIdentity(MakeRigidMeshIdentity(
		0x2000ULL, kRigidMeshInitialModelEpoch + 1u, 1,
		0xC22DULL, 0xD33DULL, 10, 15, 1, 8, 0x84F4u, 1));
	Check(meshId0 != 0 && meshId1 != 0 && meshId0 != meshId1,
		"A3 meshIds are A2 shared hashes and are distinct per surface");

	const std::vector<uint64_t> oneSurface = { meshId0 };
	const std::vector<uint64_t> twoSurfaces = { meshId0, meshId1 };
	const std::vector<uint64_t> remeshSet = { remesh0, remesh1 };
	const std::vector<uint64_t> emptyIds;

	std::unordered_map<uint64_t, RigidRegistryInstanceRecord> table;
	RigidRegistryInstanceRecord rec;
	Check(PresentRigidRegistryInstance(
			table, g1, oneSurface, xformA, RigidRegistryClass::Rigid, &rec) &&
			rec.alive && rec.meshIds.size() == 1 && rec.meshIds[0] == meshId0 &&
			rec.generation == 1 &&
			RigidRegistryInstanceKeysEqual(rec.instanceId, g1) &&
			rec.instanceId.world == world &&
			rec.instanceId.worldGeneration == 7 &&
			rec.instanceId.index == 3 &&
			rec.instanceId.generation == 1 &&
			RigidRegistryXformsEqual(rec.currentXform, xformA) &&
			RigidRegistryXformsEqual(rec.previousXform, xformA) &&
			(rec.dirty & kRigidInstanceDirtyXform) &&
			(rec.dirty & kRigidInstanceDirtyMaterial) &&
			(rec.dirty & kRigidInstanceDirtyMesh),
		"A3 Present without drawSurf creates Instance with PtRenderDefKey and meshIds");

	Check(!PresentRigidRegistryInstance(
			table, g1, oneSurface, xformA, RigidRegistryClass::World, &rec),
		"A3 World does not create a Phase A Instance");
	Check(!PresentRigidRegistryInstance(
			table, g1, oneSurface, xformA, RigidRegistryClass::Deforming, &rec),
		"A3 Deforming does not create a Phase A Instance");
	Check(!PresentRigidRegistryInstance(
			table, g1, emptyIds, xformA, RigidRegistryClass::Rigid, &rec),
		"A3 Present rejects an empty meshIds set");

	Check(UpdateRigidRegistryInstance(
			table, g1, oneSurface, xformB, false, RigidRegistryClass::Rigid, &rec) &&
			rec.alive &&
			RigidRegistryXformsEqual(rec.currentXform, xformB) &&
			RigidRegistryXformsEqual(rec.previousXform, xformA),
		"A3 Update writes current xform and keeps previous");

	Check(RigidRegistryInstanceLive(table, g1),
		"A3 off-screen: Instance remains live with no drawSurf this frame");

	Check(FreeRigidRegistryInstance(table, g1) &&
			!RigidRegistryInstanceLive(table, g1) &&
			FindRigidRegistryInstance(table, g1) != nullptr &&
			!FindRigidRegistryInstance(table, g1)->alive,
		"A3 Free retires the Instance (not alive)");

	Check(PresentRigidRegistryInstance(
			table, g2, oneSurface, xformA, RigidRegistryClass::Rigid, &rec) &&
			rec.generation == 2 &&
			PackRigidRegistryInstanceId(g1) != PackRigidRegistryInstanceId(g2) &&
			!RigidRegistryInstanceLive(table, g1) &&
			RigidRegistryInstanceLive(table, g2),
		"A3 G1/G2 Free+reuse same index is a different Instance");

	RigidRegistryInstanceKey firstUpdate;
	firstUpdate.world = world;
	firstUpdate.worldGeneration = 7;
	firstUpdate.index = 9;
	firstUpdate.generation = 1;
	Check(UpdateRigidRegistryInstance(
			table, firstUpdate, oneSurface, xformA, false, RigidRegistryClass::Rigid, &rec) &&
			rec.alive && rec.meshIds.size() == 1 && rec.meshIds[0] == meshId0 &&
			RigidRegistryInstanceKeysEqual(rec.instanceId, firstUpdate),
		"A3 first constructing Update Present-creates the Instance");

	RigidRegistryInstanceKey multi;
	multi.world = world;
	multi.worldGeneration = 7;
	multi.index = 11;
	multi.generation = 1;
	Check(PresentRigidRegistryInstance(
			table, multi, twoSurfaces, xformA, RigidRegistryClass::Rigid, &rec) &&
			rec.meshIds.size() == 2 &&
			rec.meshIds[0] == meshId0 && rec.meshIds[1] == meshId1,
		"A3 two-surface Present stores both meshIds in order");

	Check(UpdateRigidRegistryInstance(
			table, multi, remeshSet, xformB, true, RigidRegistryClass::Rigid, &rec) &&
			rec.meshIds.size() == 2 &&
			rec.meshIds[0] == remesh0 && rec.meshIds[1] == remesh1 &&
			rec.meshIds[0] != meshId0,
		"A3 Update remesh replaces the complete meshIds set");

	const fs::path recFromHarness =
		fs::path(__FILE__).parent_path() / ".." / "renderer" / "NVRHI" /
		"PathTraceRigidInstanceRecord.h";
	const fs::path recAbs(
		"E:/prog/rbdoom-3-bfg-rt-producer/neo/renderer/NVRHI/PathTraceRigidInstanceRecord.h");
	const fs::path lifeFromHarness =
		fs::path(__FILE__).parent_path() / ".." / "renderer" / "NVRHI" /
		"PathTraceGeometryLifecycle.cpp";
	const fs::path lifeAbs(
		"E:/prog/rbdoom-3-bfg-rt-producer/neo/renderer/NVRHI/PathTraceGeometryLifecycle.cpp");
	const fs::path uniFromHarness =
		fs::path(__FILE__).parent_path() / ".." / "renderer" / "NVRHI" /
		"PathTraceGeometryUniverse.cpp";
	const fs::path uniAbs(
		"E:/prog/rbdoom-3-bfg-rt-producer/neo/renderer/NVRHI/PathTraceGeometryUniverse.cpp");
	auto readText = [](const fs::path& a, const fs::path& b) -> std::string {
		for (const fs::path& candidate : { a, b })
		{
			std::ifstream in(candidate);
			if (!in)
			{
				continue;
			}
			return std::string(std::istreambuf_iterator<char>(in),
				std::istreambuf_iterator<char>());
		}
		return std::string();
	};
	const std::string recText = readText(recFromHarness, recAbs);
	const std::string lifeText = readText(lifeFromHarness, lifeAbs);
	const std::string uniText = readText(uniFromHarness, uniAbs);

	const auto recPos = recText.find("struct RigidRegistryInstanceRecord");
	std::string recStruct;
	if (recPos != std::string::npos)
	{
		recStruct = recText.substr(recPos, 900);
	}
	Check(!recStruct.empty() &&
		recStruct.find("gpuResident") == std::string::npos &&
		recStruct.find("meshIds") != std::string::npos &&
		recStruct.find("uint64_t meshId") == std::string::npos &&
		recStruct.find("(instanceId, surface meshId)") != std::string::npos &&
		recStruct.find("currentXform") != std::string::npos &&
		recStruct.find("previousXform") != std::string::npos,
		"A3 source-pin: durable Instance has meshIds[] and no gpuResident");

	const auto slotPos = lifeText.find("struct PtGeometryLifecycleSlotState");
	std::string slotStruct;
	if (slotPos != std::string::npos)
	{
		slotStruct = lifeText.substr(slotPos, 500);
	}
	Check(!slotStruct.empty() &&
		slotStruct.find("rigidInstance") != std::string::npos &&
		slotStruct.find("gpuResident") == std::string::npos,
		"A3 source-pin: entitySlots store Instance and not gpuResident");

	const auto addPos = lifeText.find("void NotifyEntityAdded");
	std::string added;
	if (addPos != std::string::npos)
	{
		added = lifeText.substr(addPos, 2200);
	}
	Check(!added.empty() &&
		added.find("WriteAuthoritativeRigidInstancePresent") != std::string::npos &&
		added.find("ResolveAuthoritativeRigidMeshIds") != std::string::npos &&
		added.find("PersistRigidMeshFromPresent") != std::string::npos,
		"A3 source-pin: NotifyEntityAdded writes the Instance table");

	const auto updPos = lifeText.find("void NotifyEntityUpdated");
	std::string updated;
	if (updPos != std::string::npos)
	{
		updated = lifeText.substr(updPos, 3600);
	}
	Check(!updated.empty() &&
		updated.find("WriteAuthoritativeRigidInstanceUpdate") != std::string::npos &&
		updated.find("ResolveAuthoritativeRigidMeshIds") != std::string::npos,
		"A3 source-pin: NotifyEntityUpdated writes the Instance table");

	const auto freePos = lifeText.find("void NotifyEntityFreed");
	std::string freed;
	if (freePos != std::string::npos)
	{
		freed = lifeText.substr(freePos, 1200);
	}
	Check(!freed.empty() &&
		freed.find("RetireAuthoritativeRigidInstance") != std::string::npos,
		"A3 source-pin: NotifyEntityFreed retires the Instance");

	const auto unchPos = lifeText.find("void NotifyEntityUnchanged");
	std::string unchanged;
	if (unchPos != std::string::npos)
	{
		unchanged = lifeText.substr(unchPos, 800);
	}
	Check(!unchanged.empty() &&
		unchanged.find("RetireAuthoritativeRigidInstance") == std::string::npos,
		"A3 source-pin: Unchanged does not evict an off-screen Instance");

	Check(!added.empty() &&
		added.find("extraTlas") == std::string::npos &&
		added.find("rigidPublishEmitted") == std::string::npos &&
		updated.find("extraTlas") == std::string::npos &&
		freed.find("extraTlas") == std::string::npos,
		"A3 source-pin: Notify hooks do not emit extraTlas or increment rigidPublishEmitted");

	const auto resolvePos = lifeText.find("void ResolveAuthoritativeRigidMeshIds");
	std::string resolveFn;
	if (resolvePos != std::string::npos)
	{
		resolveFn = lifeText.substr(resolvePos, 700);
	}
	Check(!resolveFn.empty() &&
		resolveFn.find("ComputeRigidMeshHashesFromPresent") != std::string::npos &&
		resolveFn.find("ComputeFirstRigidMeshHashFromPresent") == std::string::npos,
		"A3 source-pin: resolver uses complete mesh hash set not first-surface-only");

	const auto computePos = uniText.find(
		"int RtSmokeGeometryUniverse::ComputeRigidMeshHashesFromPresent");
	std::string computeFn;
	if (computePos != std::string::npos)
	{
		computeFn = uniText.substr(computePos, 4000);
	}
	Check(!computeFn.empty() &&
		computeFn.find("FillPathTraceRigidRouteMeshKey") != std::string::npos &&
		computeFn.find("BuildPathTraceRigidMeshHash") != std::string::npos &&
		computeFn.find("SmokeMaterialRouteClassSignature") != std::string::npos &&
		computeFn.find("outHashes.push_back") != std::string::npos &&
		computeFn.find("return meshHash") == std::string::npos &&
		uniText.find("ComputeFirstRigidMeshHashFromPresent") == std::string::npos,
		"A3 source-pin: production resolver collects every surface hash");
}

void TestRegistryPublish()
{
	using namespace cpu_producer_publish;

	const void* world = reinterpret_cast<const void*>(0x21);
	RigidRegistryInstanceKey g1;
	g1.world = world;
	g1.worldGeneration = 3;
	g1.index = 5;
	g1.generation = 1;
	RigidRegistryInstanceKey g2 = g1;
	g2.generation = 2;

	const uint64_t meshA = HashRigidMeshIdentity(MakeRigidMeshIdentity(
		0x10ULL, kRigidMeshInitialModelEpoch, 0,
		0xA1ULL, 0xB1ULL, 8, 12, 1, 1, 0x11u, 1));
	const uint64_t meshB = HashRigidMeshIdentity(MakeRigidMeshIdentity(
		0x10ULL, kRigidMeshInitialModelEpoch, 1,
		0xA2ULL, 0xB2ULL, 8, 12, 1, 2, 0x22u, 1));
	const uint64_t blasA = 0xB1A50ULL;
	const uint64_t blasB = 0xB1A51ULL;

	RegistryEligibleSurface surfA;
	surfA.instanceId = g1;
	surfA.meshId = meshA;
	surfA.blasToken = blasA;
	surfA.presentRecorded = true;
	surfA.meshBlasReady = true;
	surfA.meshBlasBuilt = true;
	RegistryEligibleSurface surfB = surfA;
	surfB.meshId = meshB;
	surfB.blasToken = blasB;

	RigidSubmitBoundaryList extras;
	RegistryPublishCounters counters;
	const std::vector<RegistryEligibleSurface> one = { surfA };
	Check(EmitCoalescedRegistryTlas(0, one, extras, counters) &&
			extras.rigidPhysicalCount == 0 && extras.records.empty() &&
			counters.registryEmitted == 0,
		"A4 mode 0: extraTlas count unchanged by registry");

	counters = RegistryPublishCounters();
	Check(EmitCoalescedRegistryTlas(
			static_cast<int>(RegistryMode::DualWrite), one, extras, counters) &&
			extras.rigidPhysicalCount == 1 && extras.records.size() == 1 &&
			extras.records[0].meshId == meshA &&
			extras.records[0].instanceMask != 0 &&
			extras.records[0].exactIdentity &&
			(extras.records[0].provenance & kOriginRegistryHook) &&
			!(extras.records[0].provenance & kOriginCaptureWalk) &&
			counters.registryEmitted == 1,
		"A4 mode 1: no-walk eligible emits exactly one desc");

	RigidSubmitBoundaryList withWalk;
	withWalk.rigidPhysicalCount = 1;
	RigidSubmitBoundaryRecord walk;
	walk.instanceId = g1;
	walk.meshId = meshA;
	walk.descriptorIndex = 0;
	walk.instanceID = 2;
	walk.instanceMask = 0x02;
	walk.submittedBlasToken = blasA;
	walk.provenance = kOriginCaptureWalk;
	walk.exactIdentity = true;
	withWalk.records.push_back(walk);
	counters = RegistryPublishCounters();
	Check(EmitCoalescedRegistryTlas(
			static_cast<int>(RegistryMode::DualWrite), one, withWalk, counters) &&
			withWalk.rigidPhysicalCount == 1 && withWalk.records.size() == 1 &&
			(withWalk.records[0].provenance & kOriginCaptureWalk) &&
			(withWalk.records[0].provenance & kOriginRegistryHook) &&
			counters.registryEmitted == 0,
		"A4 mode 1: walk desc coalesces, no second append");

	RigidSubmitBoundaryList twoExtras;
	counters = RegistryPublishCounters();
	const std::vector<RegistryEligibleSurface> two = { surfA, surfB };
	Check(EmitCoalescedRegistryTlas(
			static_cast<int>(RegistryMode::DualWrite), two, twoExtras, counters) &&
			twoExtras.rigidPhysicalCount == 2 && twoExtras.records.size() == 2 &&
			twoExtras.records[0].meshId == meshA && twoExtras.records[1].meshId == meshB &&
			counters.registryEmitted == 2 &&
			!HasRegistryTlasN2(twoExtras),
		"A4 two-surface Instance emits two descs, not one, not four");

	RegistryEligibleSurface surfG2 = surfA;
	surfG2.instanceId = g2;
	RigidSubmitBoundaryList genExtras;
	genExtras.rigidPhysicalCount = 1;
	RigidSubmitBoundaryRecord g1Desc;
	g1Desc.instanceId = g1;
	g1Desc.meshId = meshA;
	g1Desc.instanceMask = 0x02;
	g1Desc.submittedBlasToken = blasA;
	g1Desc.provenance = kOriginCaptureWalk;
	g1Desc.exactIdentity = true;
	genExtras.records.push_back(g1Desc);
	counters = RegistryPublishCounters();
	Check(EmitCoalescedRegistryTlas(
			static_cast<int>(RegistryMode::DualWrite),
			std::vector<RegistryEligibleSurface>{ surfG2 },
			genExtras, counters) &&
			genExtras.rigidPhysicalCount == 2 &&
			CountSubmitBoundaryFor(genExtras, g1, meshA) == 1 &&
			CountSubmitBoundaryFor(genExtras, g2, meshA) == 1,
		"A4 G1/G2 do not attach G1 desc to G2");

	RigidSubmitBoundaryList n2list;
	n2list.rigidPhysicalCount = 2;
	n2list.records.push_back(walk);
	n2list.records.push_back(walk);
	Check(HasRegistryTlasN2(n2list),
		"A4 N2 detector: two InstanceDescs share (instanceId, meshId)");
	counters = RegistryPublishCounters();
	Check(EmitCoalescedRegistryTlas(
			static_cast<int>(RegistryMode::DualWrite), one, extras, counters) &&
			!HasRegistryTlasN2(extras) &&
			CountSubmitBoundaryFor(extras, g1, meshA) == 1,
		"A4 emit does not create two InstanceDescs for one (instanceId, meshId)");

	RigidSubmitBoundaryList unmapped;
	unmapped.rigidPhysicalCount = 1;
	counters = RegistryPublishCounters();
	Check(RigidSubmitBoundaryHasUnmappedPhysical(unmapped),
		"A4 unmapped physical walk extra is visible in the coalescing domain");
	Check(EmitCoalescedRegistryTlas(
			static_cast<int>(RegistryMode::DualWrite), one, unmapped, counters) &&
			unmapped.rigidPhysicalCount == 1 &&
			unmapped.records.empty() &&
			counters.registryEmitted == 0 &&
			counters.registrySuppressed >= 1,
		"A4 unmapped walk extra fail-closed: no second physical descriptor");

	RegistryGapKey liveKey = MakeRegistryGapKey(
		reinterpret_cast<uint64_t>(g1.world), g1.worldGeneration, g1.index, g1.generation);
	RegistryGapLiveRecord liveRec;
	liveRec.key = liveKey;
	liveRec.alive = true;
	liveRec.geometryClass = RegistryGapClass::RigidAtRest;
	std::vector<RegistryGapLiveRecord> live = { liveRec };

	std::unordered_map<uint64_t, uint64_t> selected;
	selected[meshA] = blasA;
	RigidSubmitBoundaryList offscreen = extras;
	const std::vector<RegistryGapProduct> okProducts =
		RegistryGapProductsFromSubmitBoundary(offscreen, selected);
	const RegistryGapResult cleared = CountRegistryGap(live, okProducts);
	Check(cleared.gap() == 0 && cleared.product() == 1,
		"A4 registry-only exact metadata clears A1.5 gap");

	RegistryGapProduct wrongGen = okProducts[0];
	wrongGen.key.generation = 2;
	Check(CountRegistryGap(live, { wrongGen }).gap() == 1,
		"A4 wrong generation remains a gap");
	RegistryGapProduct maskZero = okProducts[0];
	maskZero.instanceMask = 0;
	Check(CountRegistryGap(live, { maskZero }).gap() == 1,
		"A4 mask 0 remains a gap");
	RegistryGapProduct blasMismatch = okProducts[0];
	blasMismatch.submittedBlasToken = 0xDEADULL;
	Check(CountRegistryGap(live, { blasMismatch }).gap() == 1,
		"A4 BLAS mismatch remains a gap");
	Check(CountRegistryGap(live, {}).gap() == 1,
		"A4 absent metadata remains a gap");

	Check((withWalk.records[0].provenance & kOriginCaptureWalk) &&
			(withWalk.records[0].provenance & kOriginRegistryHook),
		"A4 final physical metadata has both origin bits after coalesce");

	const fs::path sceneFromHarness =
		fs::path(__FILE__).parent_path() / ".." / "renderer" / "NVRHI" /
		"PathTraceSmokeSceneBuild.cpp";
	const fs::path sceneAbs(
		"E:/prog/rbdoom-3-bfg-rt-producer/neo/renderer/NVRHI/PathTraceSmokeSceneBuild.cpp");
	const fs::path uniFromHarness =
		fs::path(__FILE__).parent_path() / ".." / "renderer" / "NVRHI" /
		"PathTraceGeometryUniverse.cpp";
	const fs::path uniAbs(
		"E:/prog/rbdoom-3-bfg-rt-producer/neo/renderer/NVRHI/PathTraceGeometryUniverse.cpp");
	auto readText = [](const fs::path& a, const fs::path& b) -> std::string {
		for (const fs::path& candidate : { a, b })
		{
			std::ifstream in(candidate);
			if (!in)
			{
				continue;
			}
			return std::string(std::istreambuf_iterator<char>(in),
				std::istreambuf_iterator<char>());
		}
		return std::string();
	};
	const std::string sceneText = readText(sceneFromHarness, sceneAbs);
	const std::string uniText = readText(uniFromHarness, uniAbs);

	const auto pushPos = uniText.find("instanceDescs.push_back(instanceDesc);");
	std::string pushFn;
	if (pushPos != std::string::npos)
	{
		pushFn = uniText.substr(pushPos, 1200);
	}
	Check(!pushFn.empty() &&
		pushFn.find("submitMetadata") != std::string::npos &&
		pushFn.find("kOriginCaptureWalk") != std::string::npos &&
		pushFn.find("exactIdentity") != std::string::npos,
		"A4 source-pin: metadata populated at walk desc creation");

	const auto emitPos = sceneText.find("static void EmitCoalescedRegistryExtrasIntoLiveTlas");
	std::string emitFn;
	if (emitPos != std::string::npos)
	{
		emitFn = sceneText.substr(emitPos, 4500);
	}
	Check(!emitFn.empty() &&
		emitFn.find("DecodeCpuProducerRegistryMode() != 1") != std::string::npos &&
		emitFn.find(">= 1") == std::string::npos &&
		emitFn.find("registryEmitted") != std::string::npos &&
		emitFn.find("rigidPublishEmitted") == std::string::npos &&
		emitFn.find("planInstanceId") == std::string::npos &&
		emitFn.find("0x81000000") == std::string::npos &&
		emitFn.find("records.erase") != std::string::npos,
		"A4 source-pin: mode == 1, registryEmitted, atomic metadata+desc, no builder-bridge reconstruct");

	const auto walkInsert = sceneText.find(
		"liveExtraTlasInstances.end(),\n            rigidTlasRouteInstances.begin()");
	const auto walkInsertCrlf = sceneText.find(
		"liveExtraTlasInstances.end(),\r\n            rigidTlasRouteInstances.begin()");
	const size_t insertAt = walkInsert != std::string::npos ? walkInsert : walkInsertCrlf;
	const auto emitCall = (insertAt == std::string::npos)
		? std::string::npos
		: sceneText.find("EmitCoalescedRegistryExtrasIntoLiveTlas(", insertAt);
	Check(insertAt != std::string::npos && emitCall != std::string::npos && insertAt < emitCall,
		"A4 source-pin: emit is after walk extra insertion");

	const auto gapPos = sceneText.find(
		"static cpu_producer_publish::RegistryGapResult BuildRegistryGapAtSubmitBoundary");
	std::string gapFn;
	if (gapPos != std::string::npos)
	{
		gapFn = sceneText.substr(gapPos, 2500);
	}
	Check(!gapFn.empty() &&
		gapFn.find("RigidSubmitBoundaryList") != std::string::npos &&
		gapFn.find("RegistryGapProductsFromSubmitBoundary") != std::string::npos &&
		gapFn.find("planInstanceId") == std::string::npos,
		"A4 source-pin: gap join reads submit-boundary metadata");

	const auto gapCall = sceneText.rfind("BuildRegistryGapAtSubmitBoundary(");
	std::string gapCallSlice;
	if (gapCall != std::string::npos)
	{
		gapCallSlice = sceneText.substr(gapCall, 220);
	}
	Check(gapCall != std::string::npos &&
		gapCall != gapPos &&
		gapCallSlice.find("rigidSubmitBoundary") != std::string::npos &&
		gapCallSlice.find("m_smokeGeometryUniverse") != std::string::npos,
		"A4 source-pin: gap join is passed the persistent submit-boundary metadata");

	const fs::path pubFromHarness =
		fs::path(__FILE__).parent_path() / ".." / "renderer" / "NVRHI" /
		"PathTraceCpuProducerPublish.h";
	const fs::path pubAbs(
		"E:/prog/rbdoom-3-bfg-rt-producer/neo/renderer/NVRHI/PathTraceCpuProducerPublish.h");
	const std::string pubText = readText(pubFromHarness, pubAbs);
	const auto coalescePos = pubText.find("inline bool EmitCoalescedRegistryTlas(");
	std::string coalesceFn;
	if (coalescePos != std::string::npos)
	{
		coalesceFn = pubText.substr(coalescePos, 2800);
	}
	Check(!coalesceFn.empty() &&
		coalesceFn.find("RigidSubmitBoundaryHasUnmappedPhysical") != std::string::npos &&
		coalesceFn.find("kOriginRegistryHook") != std::string::npos &&
		coalesceFn.find("kOriginCaptureWalk | kOriginRegistryHook") == std::string::npos &&
		coalesceFn.find("0x81000000") == std::string::npos,
		"A4 source-pin: coalesce against metadata, HOOK-only on existing, no private ID scheme");
}

void TestSmokeTlasCapacity()
{
	using namespace cpu_producer_publish;

	Check(kPathTraceSmokeTlasMaxInstances ==
			kPathTraceSmokeTlasBaseInstances +
				kPathTraceSmokeTlasWalkBudget +
				kPathTraceSmokeTlasRegistryBudget +
				kPathTraceSmokeTlasStaticBucketBudget,
		"A4 TLAS cap is the named walk+registry+static+base sum");
	Check(kPathTraceSmokeTlasMaxInstances >
			kPathTraceSmokeTlasWalkBudget + kPathTraceSmokeTlasBaseInstances,
		"A4 TLAS cap is larger than walk+base (the old 512 abort)");
	Check(kPathTraceSmokeTlasRegistryBudget >= 2048u,
		"A4 TLAS cap includes A1.5-class registry budget");

	const uint32_t createMax = kPathTraceSmokeTlasMaxInstances;
	const uint32_t under = CountSmokeTlasSubmitInstances(2, 510);
	Check(SmokeTlasSubmitAllowed(under, createMax),
		"A4 walk+base submit is allowed under the create max");
	Check(SmokeTlasSubmitAllowed(createMax, createMax),
		"A4 submit at exact create max is allowed");
	Check(!SmokeTlasSubmitAllowed(createMax + 1u, createMax),
		"A4 submit extra+base > create max is not allowed");
	Check(!SmokeTlasSubmitAllowed(100u, 0u),
		"A4 submit with no create max is not allowed");
	Check(!SmokeTlasSubmitAllowed(createMax + 2000u, 512u),
		"A4 a 512 create max cannot submit A1.5-class extras");

	Check(SmokeTlasKeepRegistryExtras(512u, 2000u, 512u) == 0u,
		"A4 overflow: registry extras suppressed when walk+base fills max");
	Check(SmokeTlasKeepRegistryExtras(100u, 2000u, 512u) == 412u,
		"A4 overflow: keep only registry extras that fit; walk stays");
	Check(SmokeTlasKeepRegistryExtras(100u, 50u, 512u) == 50u,
		"A4 no overflow: keep all registry extras");

	const fs::path resFromHarness =
		fs::path(__FILE__).parent_path() / ".." / "renderer" / "NVRHI" /
		"PathTraceSmokeResources.cpp";
	const fs::path resAbs(
		"E:/prog/rbdoom-3-bfg-rt-producer/neo/renderer/NVRHI/PathTraceSmokeResources.cpp");
	const fs::path accelFromHarness =
		fs::path(__FILE__).parent_path() / ".." / "renderer" / "NVRHI" /
		"PathTraceAcceleration.cpp";
	const fs::path accelAbs(
		"E:/prog/rbdoom-3-bfg-rt-producer/neo/renderer/NVRHI/PathTraceAcceleration.cpp");
	const fs::path sceneFromHarness =
		fs::path(__FILE__).parent_path() / ".." / "renderer" / "NVRHI" /
		"PathTraceSmokeSceneBuild.cpp";
	const fs::path sceneAbs(
		"E:/prog/rbdoom-3-bfg-rt-producer/neo/renderer/NVRHI/PathTraceSmokeSceneBuild.cpp");
	auto readText = [](const fs::path& a, const fs::path& b) -> std::string {
		for (const fs::path& candidate : { a, b })
		{
			std::ifstream in(candidate);
			if (!in)
			{
				continue;
			}
			return std::string(std::istreambuf_iterator<char>(in),
				std::istreambuf_iterator<char>());
		}
		return std::string();
	};
	const std::string resText = readText(resFromHarness, resAbs);
	const std::string accelText = readText(accelFromHarness, accelAbs);
	const std::string sceneText = readText(sceneFromHarness, sceneAbs);

	const auto createPos = resText.find("setTopLevelMaxInstances(");
	std::string createSlice;
	if (createPos != std::string::npos)
	{
		createSlice = resText.substr(createPos, 160);
	}
	Check(!createSlice.empty() &&
			createSlice.find("kPathTraceSmokeTlasMaxInstances") != std::string::npos,
		"A4 source-pin: smoke TLAS create uses the named capacity constant");

	const auto submitPos = accelText.find("bool SubmitSmokeAccelerationBuilds");
	std::string submitFn;
	if (submitPos != std::string::npos)
	{
		submitFn = accelText.substr(submitPos, 12000);
	}
	const auto buildPos = submitFn.find("buildTopLevelAccelStruct");
	const auto guardPos = submitFn.find("SmokeTlasSubmitAllowed");
	const auto maxPos = submitFn.find("tlasMaxInstances");
	Check(!submitFn.empty() &&
			guardPos != std::string::npos &&
			maxPos != std::string::npos &&
			buildPos != std::string::npos &&
			guardPos < buildPos &&
			maxPos < buildPos,
		"A4 source-pin: submit guards buildTopLevelAccelStruct with tlas create max");
	const auto dynamicValidatePos = submitFn.find(
		"ValidateSmokeAccelStructGeometryBufferRanges(desc.dynamicBlasDesc)");
	const auto dynamicSanitizePos = submitFn.find(
		"submitPlanInput.hasDynamicBlas = desc.hasDynamicBlas && dynamicGeometryValid");
	const auto submitPlanPos = submitFn.find("BuildSmokeAccelerationSubmitPlan");
	const auto bottomLevelBuildPos = submitFn.find("BuildBottomLevelAccelStruct");
	Check(dynamicValidatePos != std::string::npos &&
			dynamicSanitizePos != std::string::npos &&
			submitPlanPos != std::string::npos &&
			bottomLevelBuildPos != std::string::npos &&
			dynamicValidatePos < dynamicSanitizePos &&
			dynamicSanitizePos < submitPlanPos &&
			submitPlanPos < bottomLevelBuildPos,
		"dynamic BLAS range is validated and removed before submit planning/build");
	const auto createBlasPos = accelText.find("RtSmokeBlasCreateResult CreateSmokeBlas");
	const auto nextSubmitPos = accelText.find(
		"bool SubmitSmokeAccelerationBuilds", createBlasPos);
	const std::string createBlasFn =
		createBlasPos != std::string::npos && nextSubmitPos != std::string::npos
			? accelText.substr(createBlasPos, nextSubmitPos - createBlasPos)
			: std::string();
	Check(createBlasFn.find("ValidateSmokeTriangleGeometryBufferRanges") !=
			std::string::npos &&
			createBlasFn.find("ValidateSmokeTriangleGeometryBufferRanges") <
				createBlasFn.find("createAccelStruct"),
		"BLAS creation validates exact buffer ranges before allocation");
	const auto staticCreateCall = sceneText.find(
		"staticBlasCreateResult = CreateSmokeBlas(staticBlasCreateDesc)");
	const auto dynamicCreateCall = sceneText.find(
		"dynamicBlasCreateResult = CreateSmokeBlas(dynamicBlasCreateDesc)");
	const std::string staticCreatePolicy =
		staticCreateCall != std::string::npos &&
			dynamicCreateCall != std::string::npos
			? sceneText.substr(staticCreateCall,
				dynamicCreateCall - staticCreateCall)
			: std::string();
	const auto dynamicCreatePolicyEnd = sceneText.find(
		"r_pathTracingHardwareOpaqueGeometry.ClearModified()",
		dynamicCreateCall);
	const std::string dynamicCreatePolicy =
		dynamicCreateCall != std::string::npos &&
			dynamicCreatePolicyEnd != std::string::npos
			? sceneText.substr(dynamicCreateCall,
				dynamicCreatePolicyEnd - dynamicCreateCall)
			: std::string();
	Check(staticCreatePolicy.find("ApplySmokeBlasCreateStatus") !=
			std::string::npos &&
		staticCreatePolicy.find("staticBlasCreateResult.status") !=
			std::string::npos &&
		staticCreatePolicy.find("if (!createPolicy.continueFrame)") !=
			std::string::npos &&
		dynamicCreatePolicy.find("ApplySmokeBlasCreateStatus") !=
			std::string::npos &&
		dynamicCreatePolicy.find("dynamicBlasCreateResult.status") !=
			std::string::npos &&
		dynamicCreatePolicy.find("if (!createPolicy.continueFrame)") !=
			std::string::npos,
		"static and dynamic create call sites use shared component-local failure policy");

	Check(sceneText.find("EnsureSmokeTlasCapacity") != std::string::npos &&
			sceneText.find("walkExtraEnd") != std::string::npos &&
			sceneText.find("tlasMaxInstances") != std::string::npos,
		"A4 source-pin: SceneBuild grows or suppresses against the live create max");
	Check(sceneText.find("grew-or-suppressed=") != std::string::npos,
		"A4 source-pin: capacity log names submitted/max/grew-or-suppressed");
	SmokeTlasCommittedMembers committed;
	committed.tlasToken = 0xA100u;
	committed.bindingToken = 0xB100u;
	committed.sceneInputsTlasToken = 0xA100u;
	committed.maxInstances = kPathTraceSmokeTlasMaxInstances;
	const SmokeTlasGrowLifecycleResult life = SimulateSmokeTlasGrowLifecycle(
		committed,
		committed.maxInstances + 128u,
		0xA200u,
		0xB200u);
	Check(life.candidate.grew && life.candidate.tlasToken == 0xA200u,
		"A4 grow-lifecycle: Ensure produces a candidate TLAS");
	Check(life.committedDuringSubmit.tlasToken == 0xA100u &&
			life.committedDuringSubmit.bindingToken == 0xB100u &&
			life.committedDuringSubmit.sceneInputsTlasToken == 0xA100u,
		"A4 grow-lifecycle: committed TLAS/binding unchanged until Commit");
	Check(life.capturedBeforeSwap &&
			life.retired.tlasToken == 0xA100u &&
			life.retired.bindingToken == 0xB100u &&
			life.retired.inRetiredList &&
			life.retired.completionArmed,
		"A4 grow-lifecycle: old TLAS+binding captured before swap into retired path");
	Check(life.retainedUntilCompletion && !life.retired.completed,
		"A4 grow-lifecycle: old package retained until completion");
	Check(life.sinks.accelSubmitTlas == 0xA200u &&
			life.sinks.bindingBuildTlas == 0xA200u &&
			life.sinks.sceneInputsTlas == 0xA200u &&
			life.sinks.resourceCommitTlas == 0xA200u &&
			life.sinks.submitMax == life.candidate.maxInstances,
		"A4 grow-lifecycle: submit/binding/sceneInputs/commit receive the grown handle");
	Check(life.committedAfter.tlasToken == 0xA200u &&
			life.committedAfter.bindingToken == 0xB200u &&
			life.committedAfter.sceneInputsTlasToken == 0xA200u,
		"A4 grow-lifecycle: Commit installs the candidate after capture");

	const auto ensurePos = resText.find("bool PathTracePrimaryPass::EnsureSmokeTlasCapacity");
	std::string ensureFn;
	if (ensurePos != std::string::npos)
	{
		ensureFn = resText.substr(ensurePos, 2200);
	}
	Check(!ensureFn.empty() &&
			ensureFn.find("candidate.tlas") != std::string::npos &&
			ensureFn.find("candidate.maxInstances") != std::string::npos &&
			ensureFn.find("m_smokeTlas = grown") == std::string::npos &&
			ensureFn.find("m_smokeBindingSet = nullptr") == std::string::npos &&
			ensureFn.find("m_sceneInputs.geometry.tlas") == std::string::npos,
		"A4 source-pin: Ensure writes a candidate and does not swap committed members");

	const auto commitPos = resText.find(
		"void PathTracePrimaryPass::CommitRayTracingSmokeSceneResources");
	std::string commitFn;
	if (commitPos != std::string::npos)
	{
		commitFn = resText.substr(commitPos, 9000);
	}
	const auto capPos = commitFn.find("CaptureRetiredRayTracingSmokeScenePackage");
	const auto pushPos = commitFn.find("PushRetiredRayTracingSmokeScenePackage");
	const auto swapPos = commitFn.find("m_smokeTlas = desc.tlas");
	Check(!commitFn.empty() &&
			capPos != std::string::npos &&
			pushPos != std::string::npos &&
			swapPos != std::string::npos &&
			capPos < pushPos &&
			pushPos < swapPos,
		"A4 source-pin: Commit captures/pushes the old package before installing tlas");

	const auto ensureCall = sceneText.find("EnsureSmokeTlasCapacity(");
	const auto accelAssign = sceneText.find("accelSubmitDesc.tlas = tlasCandidate.tlas");
	const auto bindingAssign = sceneText.find("bindingBuildDesc.tlas = tlasCandidate.tlas");
	const auto sceneAssign = sceneText.find("sceneInputs.geometry.tlas = tlasCandidate.tlas");
	const auto commitAssign = sceneText.find("resourceCommitBuildDesc.tlas = tlasCandidate.tlas");
	// The rewrite has an earlier independent commit. Anchor this legacy
	// transaction's final call after its own candidate descriptor assignment.
	const auto commitCall = sceneText.find("CommitRayTracingSmokeSceneResources(", commitAssign);
	Check(ensureCall != std::string::npos &&
			accelAssign != std::string::npos &&
			bindingAssign != std::string::npos &&
			sceneAssign != std::string::npos &&
			commitAssign != std::string::npos &&
			commitCall != std::string::npos &&
			ensureCall < accelAssign &&
			accelAssign < bindingAssign &&
			bindingAssign < sceneAssign &&
			sceneAssign < commitAssign &&
			commitAssign < commitCall,
		"A4 source-pin: candidate tlas is threaded to submit/binding/sceneInputs/commit before Commit");
}

void TestRigidCpuMeshSnapshot()
{
	using namespace cpu_producer_publish;

	alignas(8) unsigned char vertsBlob[64];
	alignas(8) unsigned char indexesBlob[48];
	alignas(8) unsigned char triBlob[8];
	const void* tri = triBlob;
	const void* verts = vertsBlob;
	const void* indexes = indexesBlob;

	RigidCpuMeshPointerSnapshot snap;
	Check(SnapshotRigidCpuMeshPointers(tri, verts, indexes, 8, 12, 8, 12, snap) &&
			snap.valid && snap.verts == verts && snap.indexes == indexes,
		"A4 cpu-cache: valid tri/verts/indexes snapshot");
	Check(!SnapshotRigidCpuMeshPointers(tri, nullptr, indexes, 8, 12, 8, 12, snap),
		"A4 cpu-cache: null verts fail closed");
	Check(!SnapshotRigidCpuMeshPointers(nullptr, verts, indexes, 8, 12, 8, 12, snap),
		"A4 cpu-cache: null tri fail closed");
	Check(!SnapshotRigidCpuMeshPointers(tri, verts, nullptr, 8, 12, 8, 12, snap),
		"A4 cpu-cache: null indexes fail closed");
	Check(!SnapshotRigidCpuMeshPointers(tri, verts, indexes, 4, 12, 8, 12, snap),
		"A4 cpu-cache: short numVerts fail closed");

	Check(SnapshotRigidCpuMeshPointers(tri, verts, indexes, 8, 12, 8, 12, snap),
		"A4 cpu-cache: resnapshot after fail-closed");
	const void* liveTri = tri;
	const void* usedVerts = nullptr;
	const void* usedIndexes = nullptr;
	Check(RigidCpuMeshConvertAfterLiveTriNull(snap, liveTri, usedVerts, usedIndexes) &&
			liveTri == nullptr &&
			usedVerts == verts &&
			usedIndexes == indexes,
		"A4 cpu-cache: convert uses snapped pointers after live tri goes null");

	const void* clearedPersistTri = nullptr;
	Check(RigidOwnedCpuMeshCacheIsUsable(
			true, true, 0xA70001u, 3, 3, 1, 3, 3, true) &&
			clearedPersistTri == nullptr,
		"A7 persist cache: owned verts/indexes remain BLAS-usable after tri=nullptr");
	Check(!RigidOwnedCpuMeshCacheIsUsable(
			true, false, 0, 3, 3, 1, 0, 0, false) &&
			clearedPersistTri == nullptr,
		"A7 persist cache: empty cache plus tri=nullptr fails closed without pointer access");

	const fs::path uniFromHarness =
		fs::path(__FILE__).parent_path() / ".." / "renderer" / "NVRHI" /
		"PathTraceGeometryUniverse.cpp";
	const fs::path uniAbs(
		"E:/prog/rbdoom-3-bfg-rt-producer/neo/renderer/NVRHI/PathTraceGeometryUniverse.cpp");
	const fs::path rigidFromHarness =
		fs::path(__FILE__).parent_path() / ".." / "renderer" / "NVRHI" /
		"PathTraceRigidCandidatePreparedDelta.cpp";
	const fs::path rigidAbs(
		"E:/prog/rbdoom-3-bfg-rt-producer/neo/renderer/NVRHI/PathTraceRigidCandidatePreparedDelta.cpp");
	const fs::path sceneFromHarness =
		fs::path(__FILE__).parent_path() / ".." / "renderer" / "NVRHI" /
		"PathTraceSmokeSceneBuild.cpp";
	const fs::path sceneAbs(
		"E:/prog/rbdoom-3-bfg-rt-producer/neo/renderer/NVRHI/PathTraceSmokeSceneBuild.cpp");
	auto readText = [](const fs::path& a, const fs::path& b) -> std::string {
		for (const fs::path& candidate : { a, b })
		{
			std::ifstream in(candidate);
			if (!in)
			{
				continue;
			}
			return std::string(std::istreambuf_iterator<char>(in),
				std::istreambuf_iterator<char>());
		}
		return std::string();
	};
	auto extractFunctionBody = [](const std::string& text,
		const std::string& signature) -> std::string {
		const auto signaturePos = text.find(signature);
		const auto bracePos = signaturePos == std::string::npos
			? std::string::npos : text.find('{', signaturePos);
		if (bracePos == std::string::npos)
		{
			return std::string();
		}
		int depth = 0;
		for (size_t index = bracePos; index < text.size(); ++index)
		{
			if (text[index] == '{')
			{
				++depth;
			}
			else if (text[index] == '}' && --depth == 0)
			{
				return text.substr(bracePos, index - bracePos + 1);
			}
		}
		return std::string();
	};
	auto findMatchingDelimiter = [](const std::string& text,
		size_t openPos, char openChar, char closeChar) -> size_t {
		if (openPos == std::string::npos || openPos >= text.size() ||
			text[openPos] != openChar)
		{
			return std::string::npos;
		}
		int depth = 0;
		for (size_t index = openPos; index < text.size(); ++index)
		{
			if (text[index] == openChar)
			{
				++depth;
			}
			else if (text[index] == closeChar && --depth == 0)
			{
				return index;
			}
		}
		return std::string::npos;
	};
	const std::string uniText = readText(uniFromHarness, uniAbs);
	const std::string sceneText = readText(sceneFromHarness, sceneAbs);
	const std::string rigidText = readText(rigidFromHarness, rigidAbs);
	const bool frameBatchAuthority =
		rigidText.find("PrepareRigidMeshCandidateBatch") != std::string::npos &&
		rigidText.find("delta.workingRecords = m_rigidMeshCandidateRecords") != std::string::npos &&
		rigidText.find("m_rigidMeshCandidateSemanticRevision != delta.baseRigidRevision") != std::string::npos &&
		rigidText.find("RtCommitRigidCandidateCpuFields") != std::string::npos &&
		rigidText.find("m_rigidMeshCandidateRecords.emplace_back") != std::string::npos &&
		rigidText.find("swap(m_rigidMeshCandidateLookup, delta.workingLookup)") != std::string::npos &&
		rigidText.find("AdvanceRigidMeshCandidateSemanticRevisionUnlocked") != std::string::npos;
	Check(frameBatchAuthority,
		"A4/A7 source-pin: extracted frame-batch stages off-side and publishes once through revision-checked CPU allowlist");

	const std::string refreshFn = extractFunctionBody(
		rigidText, "bool RtPathTraceRefreshRigidMeshCandidateCpuCache");
	const auto refreshResize = refreshFn.find("RtPathTraceBuildRigidOwnedCpuCache(localVerts,");
	std::string refreshAfter;
	if (refreshResize != std::string::npos)
	{
		refreshAfter = refreshFn.substr(refreshResize);
	}
	Check(!refreshFn.empty() &&
			refreshFn.find("SnapshotRigidCpuMeshPointers") != std::string::npos &&
			refreshResize != std::string::npos &&
			refreshFn.find("SnapshotRigidCpuMeshPointers") < refreshResize &&
			refreshAfter.find("record.tri->verts") == std::string::npos &&
			refreshAfter.find("record.tri->indexes") == std::string::npos &&
			refreshAfter.find("localVerts") != std::string::npos &&
			refreshAfter.find("localIndexes") != std::string::npos,
		"A4 source-pin: Refresh snapshots before owned-cache builder and passes local pointers");
	const std::string payloadText = readText(
		fs::path(__FILE__).parent_path() / ".." / "renderer" / "NVRHI" /
			"PathTraceRigidPreparedPayload.h", fs::path());
	const std::string cacheBuildFn = extractFunctionBody(
		payloadText, "inline bool RtPathTraceBuildRigidOwnedCpuCache");
	Check(!cacheBuildFn.empty() &&
		cacheBuildFn.find("output.vertices.resize(vertexCount)") != std::string::npos &&
		cacheBuildFn.find("drawVertices[vertexIndex]") != std::string::npos &&
		cacheBuildFn.find("output.indexes.resize(indexCount)") != std::string::npos &&
		cacheBuildFn.find("drawIndexes[indexIndex]") != std::string::npos &&
		cacheBuildFn.find("output.boundsMin[axis]") != std::string::npos &&
		cacheBuildFn.find("record.tri") == std::string::npos,
		"A4 source-pin: shared owned-cache builder converts copied pointer inputs and builds bounds");
	const auto ownedReuseIf = refreshFn.find(
		"if (RtPathTraceRigidMeshHasCachedRouteData(record))");
	const auto reuseConditionOpen = ownedReuseIf == std::string::npos
		? std::string::npos : refreshFn.find('(', ownedReuseIf);
	const auto reuseConditionClose = findMatchingDelimiter(
		refreshFn, reuseConditionOpen, '(', ')');
	const auto reuseGuardOpen = reuseConditionClose == std::string::npos
		? std::string::npos
		: refreshFn.find_first_not_of(" \t\r\n", reuseConditionClose + 1);
	const auto reuseGuardClose = reuseGuardOpen == std::string::npos ||
		refreshFn[reuseGuardOpen] != '{'
		? std::string::npos
		: findMatchingDelimiter(refreshFn, reuseGuardOpen, '{', '}');
	const auto reuseReturnTrue = reuseGuardOpen == std::string::npos
		? std::string::npos : refreshFn.find("return true", reuseGuardOpen + 1);
	const auto firstRecordTri = refreshFn.find("record.tri");
	const auto snapshotCall = refreshFn.find("SnapshotRigidCpuMeshPointers");
	const auto ownedVertexStage = refreshFn.find(
		"RtPathTraceRigidOwnedCpuCache cache");
	const auto ownedIndexStage = refreshFn.find(
		"RtPathTraceBuildRigidOwnedCpuCache(localVerts,");
	const auto ownedBoundsBuild = refreshFn.find("record.localBounds[0].Set(cache.boundsMin[0]");
	const auto ownedVertexCommit = refreshFn.find(
		"record.cachedLocalVertices.swap(cache.vertices)");
	const auto ownedIndexCommit = refreshFn.find(
		"record.cachedLocalIndexes.swap(cache.indexes)");
	const auto cacheValidCommit = refreshFn.find(
		"record.cachedRouteDataValid = true");
	Check(ownedReuseIf != std::string::npos &&
			reuseConditionClose != std::string::npos &&
			reuseGuardOpen != std::string::npos &&
			reuseGuardClose != std::string::npos &&
			reuseReturnTrue != std::string::npos &&
			firstRecordTri != std::string::npos &&
			snapshotCall != std::string::npos &&
			ownedVertexStage != std::string::npos &&
			ownedIndexStage != std::string::npos &&
			ownedBoundsBuild != std::string::npos &&
			ownedVertexCommit != std::string::npos &&
			ownedIndexCommit != std::string::npos &&
			cacheValidCommit != std::string::npos &&
			ownedReuseIf < reuseGuardOpen &&
			reuseGuardOpen < reuseReturnTrue &&
			reuseReturnTrue < reuseGuardClose &&
			reuseGuardClose < firstRecordTri &&
			firstRecordTri < snapshotCall &&
			snapshotCall < ownedVertexStage &&
			ownedVertexStage < ownedIndexStage &&
			ownedIndexStage < ownedVertexCommit &&
			ownedVertexCommit < ownedIndexCommit &&
			ownedIndexCommit < ownedBoundsBuild &&
			ownedBoundsBuild < cacheValidCommit,
		"A7 source-pin: exact cache-reuse guard returns before first tri read, then stages and commits owned data");

	const std::string buildFn = extractFunctionBody(
		uniText, "bool BuildRigidLocalMeshData");
	Check(!buildFn.empty() &&
			buildFn.find("RigidMeshHasCachedRouteData") != std::string::npos &&
			buildFn.find("cachedLocalVertices") != std::string::npos &&
			buildFn.find("record.tri") == std::string::npos &&
			buildFn.find("record.tri->") == std::string::npos &&
			buildFn.find("SnapshotRigidCpuMeshPointers") == std::string::npos,
		"A4 source-pin: Build returns owned cache first and never reads record.tri");

	const std::string cachePredicateSignature =
		"bool RtPathTraceRigidMeshHasCachedRouteData";
	const std::string cachePredicateFn = extractFunctionBody(
		rigidText, cachePredicateSignature);
	Check(!cachePredicateFn.empty() &&
			cachePredicateFn.find("RigidOwnedCpuMeshCacheIsUsable") != std::string::npos &&
			cachePredicateFn.find("record.tri") == std::string::npos,
		"A7 source-pin: cache usability is owned-count/signature/bounds state, not tri lifetime");

	const std::string persistFn = extractFunctionBody(
		uniText, "int RtSmokeGeometryUniverse::PersistRigidMeshFromPresent");
	const std::string persistTriAssignment = "observation.tri = tri";
	const std::string persistVertAssignment =
		"observation.numVerts = tri->numVerts";
	const std::string persistIndexAssignment =
		"observation.numIndexes = tri->numIndexes";
	const std::string persistMatrixFill = "BuildRigidNormalTexMatrix";
	const std::string persistRecordText =
		"RecordRigidMeshCandidate(observation)";
	const auto persistTri = persistFn.find(persistTriAssignment);
	const auto persistVerts = persistFn.find(persistVertAssignment);
	const auto persistIndexes = persistFn.find(persistIndexAssignment);
	const auto persistMatrix = persistFn.find(persistMatrixFill);
	const auto persistRecordCall = persistFn.find("RecordRigidMeshCandidate(observation)");
	Check(!persistFn.empty() &&
			persistTri != std::string::npos &&
			persistVerts != std::string::npos &&
			persistIndexes != std::string::npos &&
			persistMatrix != std::string::npos &&
			persistRecordCall != std::string::npos &&
			persistTri < persistVerts &&
			persistVerts < persistIndexes &&
			persistIndexes < persistMatrix &&
			persistMatrix < persistRecordCall &&
			persistFn.find(persistTriAssignment, persistTri + 1) == std::string::npos &&
			persistFn.find(persistVertAssignment, persistVerts + 1) == std::string::npos &&
			persistFn.find(persistIndexAssignment, persistIndexes + 1) == std::string::npos &&
			persistFn.find(persistMatrixFill, persistMatrix + 1) == std::string::npos &&
			persistFn.find(persistRecordText, persistRecordCall + 1) == std::string::npos,
		"A7 source-pin: unique Persist tri/count/matrix fills precede the unique production Record call");

	const std::string recordA7Fn = extractFunctionBody(
		uniText, "void RtSmokeGeometryUniverse::RecordRigidMeshCandidate");
	const auto cacheChangedIf = recordA7Fn.find("if (cpuMeshCacheChanged)");
	const auto cacheChangedConditionOpen = cacheChangedIf == std::string::npos
		? std::string::npos : recordA7Fn.find('(', cacheChangedIf);
	const auto cacheChangedConditionClose = findMatchingDelimiter(
		recordA7Fn, cacheChangedConditionOpen, '(', ')');
	const auto cacheChangedGuardOpen = cacheChangedConditionClose == std::string::npos
		? std::string::npos
		: recordA7Fn.find_first_not_of(" \t\r\n", cacheChangedConditionClose + 1);
	const auto cacheChangedGuardClose = cacheChangedGuardOpen == std::string::npos ||
		recordA7Fn[cacheChangedGuardOpen] != '{'
		? std::string::npos
		: findMatchingDelimiter(recordA7Fn, cacheChangedGuardOpen, '{', '}');
	const auto recordInvalidate = cacheChangedGuardOpen == std::string::npos
		? std::string::npos
		: recordA7Fn.find("cachedRouteDataValid = false", cacheChangedGuardOpen + 1);
	const auto recordSignatureZero = cacheChangedGuardOpen == std::string::npos
		? std::string::npos
		: recordA7Fn.find("cpuMeshContentSignature = 0", cacheChangedGuardOpen + 1);
	const auto recordBoundsInvalidate = cacheChangedGuardOpen == std::string::npos
		? std::string::npos
		: recordA7Fn.find("localBoundsValid = false", cacheChangedGuardOpen + 1);
	const auto recordRefresh = cacheChangedGuardOpen == std::string::npos
		? std::string::npos
		: recordA7Fn.find(
			"RefreshRigidMeshCandidateCpuCache(m_rigidMeshCandidateRecords[liveIndex])",
			cacheChangedGuardOpen + 1);
	Check(frameBatchAuthority || (!recordA7Fn.empty() &&
			cacheChangedIf != std::string::npos &&
			cacheChangedConditionClose != std::string::npos &&
			cacheChangedGuardOpen != std::string::npos &&
			cacheChangedGuardClose != std::string::npos &&
			recordInvalidate != std::string::npos &&
			recordSignatureZero != std::string::npos &&
			recordBoundsInvalidate != std::string::npos &&
			recordRefresh != std::string::npos &&
			cacheChangedIf < cacheChangedGuardOpen &&
			cacheChangedGuardOpen < recordInvalidate &&
			recordInvalidate < recordSignatureZero &&
			recordSignatureZero < recordBoundsInvalidate &&
			recordBoundsInvalidate < recordRefresh &&
			recordRefresh < cacheChangedGuardClose),
		"A7 source-pin: cache invalidation/refresh is owned by legacy body or extracted frame-batch closure");

	const std::string findFn = extractFunctionBody(
		uniText, "size_t RtSmokeGeometryUniverse::FindOrCreateRigidMeshCandidate");
	const auto refreshCall = findFn.find("RefreshRigidMeshCandidateCpuCache(record)");
	const auto pushPos = findFn.find("m_rigidMeshCandidateRecords.push_back");
	Check(frameBatchAuthority || (!findFn.empty() &&
			refreshCall != std::string::npos &&
			pushPos != std::string::npos &&
			refreshCall < pushPos &&
			findFn.find("return recordIndex;") != std::string::npos &&
			findFn.find("return &m_rigidMeshCandidateRecords.back()") == std::string::npos),
		"A4 source-pin: new-record construction is legacy indexed or extracted frame-batch owned");

	const std::string emitFn = extractFunctionBody(
		sceneText, "static void EmitCoalescedRegistryExtrasIntoLiveTlas");
	Check(!emitFn.empty() &&
			emitFn.find("SnapshotRigidMeshRegistryLookup") != std::string::npos &&
			emitFn.find("builtBlas") != std::string::npos &&
			emitFn.find("RefreshRigidMeshCandidateCpuCache") == std::string::npos &&
			emitFn.find("BuildRigidLocalMeshData") == std::string::npos,
		"A4 source-pin: registry emit is BLAS-handle based and does not recode CPU verts");

	RigidStaleTriCacheRecord poisonComplete;
	poisonComplete.poisonTriToken = 0xDEAD0001ULL;
	poisonComplete.ownedCacheComplete = true;
	poisonComplete.cachedVertSentinel = 0xCACE0001u;
	poisonComplete.cachedIndexSentinel = 0xCACE0002u;
	uint32_t builtVerts = 0;
	uint32_t builtIndexes = 0;
	Check(BuildRigidLocalFromOwnedCacheOnly(poisonComplete, builtVerts, builtIndexes) &&
			builtVerts == 0xCACE0001u &&
			builtIndexes == 0xCACE0002u &&
			poisonComplete.poisonTriToken == 0xDEAD0001ULL,
		"A4 stale-tri: complete owned cache builds without reading poison tri");
	Check(ValidateRigidOwnedCacheOnly(poisonComplete) == 0u &&
			poisonComplete.poisonTriToken == 0xDEAD0001ULL,
		"A4 stale-tri: complete owned cache validates without reading poison tri");

	RigidStaleTriCacheRecord poisonIncomplete;
	poisonIncomplete.poisonTriToken = 0xDEAD0001ULL;
	poisonIncomplete.ownedCacheComplete = false;
	uint32_t missVerts = 0xFFu;
	uint32_t missIndexes = 0xFFu;
	Check(!BuildRigidLocalFromOwnedCacheOnly(poisonIncomplete, missVerts, missIndexes) &&
			ValidateRigidOwnedCacheOnly(poisonIncomplete) != 0u &&
			poisonIncomplete.poisonTriToken == 0xDEAD0001ULL,
		"A4 stale-tri: incomplete cache fail-closed without reading poison tri");

	const std::string validateFn = extractFunctionBody(
		uniText, "uint32_t ValidateRigidBlasInputRecord");
	Check(!validateFn.empty() &&
			validateFn.find("RigidMeshHasCachedRouteData") != std::string::npos &&
			validateFn.find("record.tri") == std::string::npos &&
			validateFn.find("record.tri->") == std::string::npos,
		"A4 source-pin: Validate uses owned cache and never reads record.tri");

	const std::string recordFn = extractFunctionBody(
		uniText, "void RtSmokeGeometryUniverse::RecordRigidMeshCandidate");
	const auto assignObs = recordFn.find(
		"m_rigidMeshCandidateRecords[liveIndex].tri = observation.tri");
	const auto refreshExisting = recordFn.find(
		"RefreshRigidMeshCandidateCpuCache(m_rigidMeshCandidateRecords[liveIndex])");
	Check(frameBatchAuthority || (!recordFn.empty() &&
			assignObs != std::string::npos &&
			refreshExisting != std::string::npos &&
			assignObs < refreshExisting &&
			recordFn.find("RefreshRigidMeshCandidateCpuCache(record)") == std::string::npos &&
			recordFn.find("RigidMeshCandidateRecord& record = m_rigidMeshCandidateRecords[recordIndex]") == std::string::npos &&
			recordFn.find("RigidMeshCandidateRecord& record = m_rigidMeshCandidateRecords[liveIndex]") == std::string::npos),
		"A4 source-pin: existing refresh uses legacy indexing or extracted off-side records");

	Check(frameBatchAuthority || (!findFn.empty() &&
			findFn.find("record.tri = observation.tri") != std::string::npos &&
			findFn.find("record.tri = observation.tri") < refreshCall),
		"A4 source-pin: new-record refresh is observation-owned in legacy or extracted batch closure");
}

void TestRigidMeshCandidateMapGuard()
{
	using namespace cpu_producer_publish;

	Check(!RigidMeshCandidateMapStateIsPlausible(1, 1, 1, 0.0f),
		"A4 map-guard: max_load_factor 0 is implausible");
	Check(!RigidMeshCandidateMapStateIsPlausible(1, 1, 1, -1.0f),
		"A4 map-guard: negative max_load_factor is implausible");
	Check(!RigidMeshCandidateMapStateIsPlausible(1, 1, 1, 1.0e-6f),
		"A4 map-guard: tiny-positive max_load_factor is implausible");
	Check(!RigidMeshCandidateMapStateIsPlausible(1024, 1024, 8, 1.0e-6f),
		"A4 map-guard: tiny load would demand an invalid bucket count");
	Check(!RigidMeshCandidateMapStateIsPlausible(
			static_cast<uint64_t>(kRigidMeshCandidateMapMaxEntries) + 1ull, 0, 1, 1.0f),
		"A4 map-guard: records.size() above max is implausible");
	Check(!RigidMeshCandidateMapStateIsPlausible(
			0, static_cast<uint64_t>(kRigidMeshCandidateMapMaxEntries) + 1ull, 1, 1.0f),
		"A4 map-guard: map size above max is implausible");
	Check(!RigidMeshCandidateMapStateIsPlausible(
			0, 0, static_cast<uint64_t>(kRigidMeshCandidateMapMaxBuckets) + 1ull, 1.0f),
		"A4 map-guard: bucket count above max is implausible");
	Check(!RigidMeshCandidateMapStateIsPlausible(1, 100, 8, 1.0f),
		"A4 map-guard: lookup.size() >> records.size() is implausible");
	Check(RigidMeshCandidateMapStateIsPlausible(4, 4, 8, 1.0f),
		"A4 map-guard: healthy size/buckets/load-factor is plausible");
	Check(RigidMeshLookupBindHoldsMutex() &&
		RigidMeshRegistryEmitUsesImmutableSnapshot() &&
		RigidMeshLookupEmitAndBindCannotOverlapUnlocked(),
		"A4 map-guard: emit snapshot and Bind mutex cannot overlap unlocked");
	Check(RigidMeshRegistryRecordVectorBoundaryHolds(true, false),
		"A4 map-guard: snapshot copy with locked record mutate holds the boundary");
	Check(!RigidMeshRegistryRecordVectorBoundaryHolds(true, true),
		"A4 map-guard: snapshot copy vs unlocked record mutate is a race");
	Check(RigidMeshRegistryRecordVectorBoundaryHolds(false, true),
		"A4 map-guard: unlocked mutate without snapshot copy is outside this seam");
	Check(std::string(kRigidMeshCandidateRecordVectorBoundaryName) ==
		"m_rigidMeshCandidateLookupMutex",
		"A4 map-guard: record-vector boundary is the lookup mutex");
	{
		RigidMeshGpuPublishCommit gpuCommit;
		Check(!RigidMeshGpuPublishCommitBound(gpuCommit),
			"A4 map-guard: GPU publish unbound by default");
		gpuCommit.mutexHeld = true;
		Check(!RigidMeshGpuPublishCommitBound(gpuCommit),
			"A4 map-guard: GPU publish without identity revalidation is unbound");
		gpuCommit.identityRevalidated = true;
		Check(!RigidMeshGpuPublishCommitBound(gpuCommit),
			"A4 map-guard: GPU publish without submitted flag is unbound");
		gpuCommit.gpuBlasBuildSubmitted = true;
		Check(RigidMeshGpuPublishCommitBound(gpuCommit),
			"A4 map-guard: GPU publish bound only with mutex+revalidate+submitted");
		gpuCommit.mutexHeld = false;
		Check(!RigidMeshGpuPublishCommitBound(gpuCommit),
			"A4 map-guard: GPU publish with mutexHeld=false is unbound");
		RigidMeshRecordCompactCommit compactCommit;
		Check(RigidMeshRecordCompactCommitBound(compactCommit),
			"A4 map-guard: compact with no swap is bound");
		compactCommit.vectorSwapped = true;
		Check(!RigidMeshRecordCompactCommitBound(compactCommit),
			"A4 map-guard: vector swap without mutex is unbound");
		compactCommit.mutexHeld = true;
		Check(!RigidMeshRecordCompactCommitBound(compactCommit),
			"A4 map-guard: vector swap without Unlocked rebuild is unbound");
		compactCommit.lookupRebuiltUnlocked = true;
		Check(RigidMeshRecordCompactCommitBound(compactCommit),
			"A4 map-guard: compact bound only with mutex+swap+Unlocked rebuild");
		compactCommit.mutexHeld = false;
		Check(!RigidMeshRecordCompactCommitBound(compactCommit),
			"A4 map-guard: compact mutexHeld=false with swap is unbound");
	}

	RigidMeshCandidateLookupHealthState health;
	bool mapTouched = true;
	Check(!RigidMeshCandidateLookupTryAccess(
			health, 1024, 1024, 8, 1.0e-6f, false, mapTouched) &&
		!mapTouched &&
		RigidMeshCandidateLookupIsQuarantined(health),
		"A4 map-guard: tiny-load transitions to quarantined without map callback");
	mapTouched = true;
	Check(!RigidMeshCandidateLookupTryAccess(
			health, 4, 4, 8, 1.0f, false, mapTouched) &&
		!mapTouched &&
		RigidMeshCandidateLookupIsQuarantined(health),
		"A4 map-guard: quarantined read/write fail-closed without map callback");

	RigidMeshCandidateLookupHealthState insertHealth;
	mapTouched = false;
	Check(RigidMeshCandidateLookupTryAccess(
			insertHealth, 4, 4, 8, 1.0f, false, mapTouched) &&
		mapTouched &&
		!RigidMeshCandidateLookupIsQuarantined(insertHealth),
		"A4 map-guard: healthy access may touch the map");
	mapTouched = true;
	Check(!RigidMeshCandidateLookupTryAccess(
			insertHealth, 4, 4, 8, 1.0f, true, mapTouched) &&
		mapTouched &&
		RigidMeshCandidateLookupIsQuarantined(insertHealth),
		"A4 map-guard: simulated insertion failure quarantines");
	mapTouched = true;
	Check(!RigidMeshCandidateLookupTryAccess(
			insertHealth, 4, 4, 8, 1.0f, false, mapTouched) &&
		!mapTouched,
		"A4 map-guard: after insert failure later access does not touch the map");

	mapTouched = true;
	Check(!RigidMeshCandidateLookupTryRebuild(insertHealth, mapTouched) &&
		!mapTouched &&
		RigidMeshCandidateLookupIsQuarantined(insertHealth),
		"A4 map-guard: quarantined rebuild does not touch or reset health");
	RigidMeshCandidateLookupHealthState logicalHealth;
	logicalHealth.health = kRigidMeshCandidateLookupHealthQuarantined;
	logicalHealth.reason = kRigidMeshCandidateLookupReasonLogical;
	logicalHealth.structurallyValid = 0;
	mapTouched = true;
	Check(!RigidMeshCandidateLookupTryRebuild(logicalHealth, mapTouched) &&
		!mapTouched &&
		RigidMeshCandidateLookupIsQuarantined(logicalHealth),
		"A4 map-guard: logical quarantine without structural proof stays quarantined");
	RigidMeshCandidateLookupHealthState liveHealth;
	mapTouched = false;
	Check(RigidMeshCandidateLookupTryRebuild(liveHealth, mapTouched) &&
		mapTouched &&
		!RigidMeshCandidateLookupIsQuarantined(liveHealth),
		"A4 map-guard: healthy rebuild may touch the existing map");

	const fs::path uniFromHarness =
		fs::path(__FILE__).parent_path() / ".." / "renderer" / "NVRHI" /
		"PathTraceGeometryUniverse.cpp";
	const fs::path uniAbs(
		"E:/prog/rbdoom-3-bfg-rt-producer/neo/renderer/NVRHI/PathTraceGeometryUniverse.cpp");
	const fs::path lifeFromHarness =
		fs::path(__FILE__).parent_path() / ".." / "renderer" / "NVRHI" /
		"PathTraceGeometryLifecycle.cpp";
	const fs::path lifeAbs(
		"E:/prog/rbdoom-3-bfg-rt-producer/neo/renderer/NVRHI/PathTraceGeometryLifecycle.cpp");
	const fs::path sceneFromHarness =
		fs::path(__FILE__).parent_path() / ".." / "renderer" / "NVRHI" /
		"PathTraceSmokeSceneBuild.cpp";
	const fs::path sceneAbs(
		"E:/prog/rbdoom-3-bfg-rt-producer/neo/renderer/NVRHI/PathTraceSmokeSceneBuild.cpp");
	const fs::path rigidFromHarness =
		fs::path(__FILE__).parent_path() / ".." / "renderer" / "NVRHI" /
		"PathTraceRigidCandidatePreparedDelta.cpp";
	const fs::path rigidAbs(
		"E:/prog/rbdoom-3-bfg-rt-producer/neo/renderer/NVRHI/PathTraceRigidCandidatePreparedDelta.cpp");
	auto readText = [](const fs::path& a, const fs::path& b) -> std::string {
		for (const fs::path& candidate : { a, b })
		{
			std::ifstream in(candidate);
			if (!in)
			{
				continue;
			}
			return std::string(std::istreambuf_iterator<char>(in),
				std::istreambuf_iterator<char>());
		}
		return std::string();
	};
	auto extractFunctionBody = [](const std::string& text,
		const std::string& signature) -> std::string {
		const auto sigPos = text.find(signature);
		if (sigPos == std::string::npos)
		{
			return std::string();
		}
		const auto bracePos = text.find('{', sigPos);
		if (bracePos == std::string::npos)
		{
			return std::string();
		}
		int depth = 0;
		for (size_t index = bracePos; index < text.size(); ++index)
		{
			const char ch = text[index];
			if (ch == '{')
			{
				++depth;
			}
			else if (ch == '}')
			{
				--depth;
				if (depth == 0)
				{
					return text.substr(bracePos, index - bracePos + 1);
				}
			}
		}
		return std::string();
	};
	const std::string uniText = readText(uniFromHarness, uniAbs);
	const std::string lifeText = readText(lifeFromHarness, lifeAbs);
	const std::string sceneText = readText(sceneFromHarness, sceneAbs);
	const std::string rigidText = readText(rigidFromHarness, rigidAbs);
	const bool frameBatchAuthority =
		rigidText.find("PrepareRigidMeshCandidateBatch") != std::string::npos &&
		rigidText.find("delta.workingRecords = m_rigidMeshCandidateRecords") != std::string::npos &&
		rigidText.find("m_rigidMeshCandidateSemanticRevision != delta.baseRigidRevision") != std::string::npos &&
		rigidText.find("RtCommitRigidCandidateCpuFields") != std::string::npos &&
		rigidText.find("m_rigidMeshCandidateRecords.emplace_back") != std::string::npos &&
		rigidText.find("swap(m_rigidMeshCandidateLookup, delta.workingLookup)") != std::string::npos;

	const std::string recordBody = extractFunctionBody(
		rigidText, "void RtSmokeGeometryUniverse::RecordRigidMeshCandidate");
	const auto recordRefresh = recordBody.find(
		"RefreshRigidMeshCandidateCpuCache(m_rigidMeshCandidateRecords[liveIndex])");
	Check(frameBatchAuthority || (!recordBody.empty() &&
		recordBody.find("const size_t recordIndex = FindOrCreateRigidMeshCandidate") != std::string::npos &&
		recordBody.find("m_rigidMeshCandidateRecords[liveIndex].tri = observation.tri") != std::string::npos &&
		recordRefresh != std::string::npos &&
		recordBody.find("RefreshRigidMeshCandidateCpuCache(record)") == std::string::npos &&
		recordBody.find("RigidMeshCandidateRecord& record = m_rigidMeshCandidateRecords[recordIndex]") == std::string::npos),
		"A4 source-pin: serial Record delegates to indexed legacy or extracted frame-batch authority");

	const std::string findBody = extractFunctionBody(
		uniText, "size_t RtSmokeGeometryUniverse::FindOrCreateRigidMeshCandidate");
	const auto firstGuard = findBody.find("EnsureRigidMeshCandidateLookupLive");
	const auto firstFind = findBody.find("RigidMeshCandidateLookupFind");
	const auto refreshPos = findBody.find("RefreshRigidMeshCandidateCpuCache(record)");
	const auto secondGuard = (refreshPos == std::string::npos)
		? std::string::npos
		: findBody.find("EnsureRigidMeshCandidateLookupLive", refreshPos);
	const auto pushPos = findBody.find("m_rigidMeshCandidateRecords.push_back");
	const auto bindPos = findBody.find("RigidMeshCandidateLookupBind");
	const auto catchPos = (bindPos == std::string::npos)
		? std::string::npos
		: findBody.find("catch (const std::length_error&)", bindPos);
	const auto rollbackPos = (bindPos == std::string::npos)
		? std::string::npos
		: findBody.find("pop_back", bindPos);
	Check(frameBatchAuthority || (!findBody.empty() &&
		firstGuard != std::string::npos &&
		firstFind != std::string::npos &&
		refreshPos != std::string::npos &&
		secondGuard != std::string::npos &&
		pushPos != std::string::npos &&
		bindPos != std::string::npos &&
		catchPos != std::string::npos &&
		rollbackPos != std::string::npos &&
		firstGuard < firstFind &&
		firstFind < refreshPos &&
		refreshPos < secondGuard &&
		secondGuard < pushPos &&
		secondGuard < bindPos &&
		pushPos < bindPos &&
		bindPos < catchPos &&
		bindPos < rollbackPos),
		"A4 source-pin: lookup/new-record publication is legacy rollback or atomic frame-batch commit");

	const std::string persistBody = extractFunctionBody(
		uniText, "int RtSmokeGeometryUniverse::PersistRigidMeshFromPresent");
	const auto persistGuard = persistBody.find("EnsureRigidMeshCandidateLookupLive");
	const auto persistFind = persistBody.find("RigidMeshCandidateLookupFind");
	Check(!persistBody.empty() &&
		persistGuard != std::string::npos &&
		persistFind != std::string::npos &&
		persistGuard < persistFind &&
		persistBody.find("deferredSinceFrame") != std::string::npos &&
		persistBody.find("catch (const std::length_error&)") != std::string::npos &&
		persistBody.find("BuildBottomLevelAccelStruct") == std::string::npos &&
		persistBody.find("buildTopLevelAccelStruct") == std::string::npos &&
		persistBody.find("createAccelStruct") == std::string::npos,
		"A4 source-pin: Persist guard is before lookup find and only sets deferredSinceFrame");

	const std::string notifyBody = extractFunctionBody(
		lifeText, "void NotifyEntityUpdated(");
	const auto persist1 = notifyBody.find("PersistRigidMeshFromPresent");
	const auto catch1 = (persist1 == std::string::npos)
		? std::string::npos
		: notifyBody.find("catch (const std::length_error&)", persist1);
	const auto persist2 = (catch1 == std::string::npos)
		? std::string::npos
		: notifyBody.find("PersistRigidMeshFromPresent", catch1);
	const auto catch2 = (persist2 == std::string::npos)
		? std::string::npos
		: notifyBody.find("catch (const std::length_error&)", persist2);
	Check(!notifyBody.empty() &&
		persist1 != std::string::npos &&
		catch1 != std::string::npos &&
		persist2 != std::string::npos &&
		catch2 != std::string::npos &&
		persist1 < catch1 &&
		catch1 < persist2 &&
		persist2 < catch2 &&
		notifyBody.find("DecodeCpuProducerRegistryMode") == std::string::npos &&
		notifyBody.find("BuildBottomLevelAccelStruct") == std::string::npos,
		"A4 source-pin: both Notify Persist calls have their own length_error catch");

	const std::string findAccessor = extractFunctionBody(
		uniText, "bool RtSmokeGeometryUniverse::RigidMeshCandidateLookupFindUnlocked(");
	Check(!findAccessor.empty() &&
		findAccessor.find("m_rigidMeshCandidateLookup.find") != std::string::npos &&
		findAccessor.find("EnsureRigidMeshCandidateLookupLive") != std::string::npos &&
		findAccessor.find("lock_guard") == std::string::npos,
		"A4 source-pin: LookupFind is the checked lookup.find accessor");

	const std::string rebuildBody = extractFunctionBody(
		uniText, "bool RtSmokeGeometryUniverse::RebuildRigidMeshCandidateLookupFromRecordsUnlocked(");
	const auto rebuildDecide = rebuildBody.find("RigidMeshCandidateLookupTryRebuild");
	const auto rebuildIfNeg =
		rebuildDecide == std::string::npos
			? std::string::npos
			: rebuildBody.rfind("if (!", rebuildDecide);
	const auto rebuildIfBrace =
		rebuildIfNeg == std::string::npos
			? std::string::npos
			: rebuildBody.find('{', rebuildIfNeg);
	const auto rebuildCondClose =
		rebuildIfBrace == std::string::npos
			? std::string::npos
			: rebuildBody.rfind(')', rebuildIfBrace);
	size_t rebuildIfClose = std::string::npos;
	if (rebuildIfBrace != std::string::npos)
	{
		int depth = 0;
		for (size_t index = rebuildIfBrace; index < rebuildBody.size(); ++index)
		{
			if (rebuildBody[index] == '{')
			{
				++depth;
			}
			else if (rebuildBody[index] == '}')
			{
				--depth;
				if (depth == 0)
				{
					rebuildIfClose = index;
					break;
				}
			}
		}
	}
	const auto rebuildGuardReturn =
		rebuildIfBrace == std::string::npos
			? std::string::npos
			: rebuildBody.find("return false", rebuildIfBrace);
	const auto rebuildFresh =
		rebuildIfClose == std::string::npos
			? std::string::npos
			: rebuildBody.find("std::unordered_map", rebuildIfClose);
	const auto rebuildSwap = rebuildBody.find("m_rigidMeshCandidateLookup.swap");
	const auto rebuildReset = rebuildBody.find("RigidMeshCandidateLookupResetHealth");
	Check(!rebuildBody.empty() &&
		rebuildBody.find("lock_guard") == std::string::npos &&
		rebuildDecide != std::string::npos &&
		rebuildIfNeg != std::string::npos &&
		rebuildCondClose != std::string::npos &&
		rebuildIfBrace != std::string::npos &&
		rebuildIfClose != std::string::npos &&
		rebuildGuardReturn != std::string::npos &&
		rebuildFresh != std::string::npos &&
		rebuildSwap != std::string::npos &&
		rebuildReset != std::string::npos &&
		rebuildIfNeg < rebuildDecide &&
		rebuildDecide < rebuildCondClose &&
		rebuildCondClose < rebuildIfBrace &&
		rebuildIfBrace < rebuildGuardReturn &&
		rebuildGuardReturn < rebuildIfClose &&
		rebuildIfClose < rebuildFresh &&
		rebuildFresh < rebuildSwap &&
		rebuildSwap < rebuildReset,
		"A4 source-pin: Rebuild guarded return is inside if (!TryRebuild) before fresh/swap/reset");

	const std::string clearBody = extractFunctionBody(
		uniText, "void RtSmokeGeometryUniverse::ClearRigidResidencyCaches");
	Check(!clearBody.empty() &&
		clearBody.find("RebuildRigidMeshCandidateLookupFromRecordsUnlocked") != std::string::npos &&
		clearBody.find("RebuildRigidMeshCandidateLookupFromRecords(") == std::string::npos &&
		clearBody.find("m_rigidMeshCandidateLookup.swap") == std::string::npos &&
		clearBody.find("m_rigidMeshCandidateLookup.clear") == std::string::npos,
		"A4 source-pin: Clear goes through health-checked Rebuild and does not touch lookup");

	const auto compactPos = uniText.find("if (removedMeshRecord)");
	std::string compactFn;
	if (compactPos != std::string::npos)
	{
		compactFn = uniText.substr(compactPos, 1200);
	}
	Check(!compactFn.empty() &&
		compactFn.find("RebuildRigidMeshCandidateLookupFromRecordsUnlocked") != std::string::npos &&
		compactFn.find("m_rigidMeshCandidateLookup.swap") == std::string::npos &&
		compactFn.find("m_rigidMeshCandidateLookup.clear") == std::string::npos,
		"A4 source-pin: compact goes through health-checked Rebuild and does not touch lookup");

	const std::string ensureBody = extractFunctionBody(
		uniText, "bool RtSmokeGeometryUniverse::EnsureRigidMeshCandidateLookupLive");
	const auto ensureHealth = ensureBody.find("RigidMeshCandidateLookupIsQuarantined");
	const auto ensureCanary = ensureBody.find("kRigidMeshCandidateLookupCanaryLive");
	Check(!ensureBody.empty() &&
		ensureHealth != std::string::npos &&
		ensureCanary != std::string::npos &&
		ensureHealth < ensureCanary &&
		ensureBody.find("m_rigidMeshCandidateLookup.size") == std::string::npos &&
		ensureBody.find("bucket_count") == std::string::npos &&
		ensureBody.find("max_load_factor") == std::string::npos &&
		ensureBody.find("first-touch fail-closed") != std::string::npos,
		"A4 source-pin: first live Ensure uses canary, not smashed map scalars");

	const std::string emitFn = extractFunctionBody(
		sceneText, "static void EmitCoalescedRegistryExtrasIntoLiveTlas");
	Check(!emitFn.empty() &&
		emitFn.find("DecodeCpuProducerRegistryMode() != 1") != std::string::npos &&
		emitFn.find("SnapshotRigidMeshRegistryLookup") != std::string::npos &&
		emitFn.find("lookupSnapshot") != std::string::npos &&
		emitFn.find("ClassifyRigidMeshForRegistry") == std::string::npos &&
		emitFn.find("RigidMeshCandidateBlasWasBuilt") == std::string::npos &&
		emitFn.find("RigidMeshCandidateBuiltBlas") == std::string::npos &&
		emitFn.find("RigidMeshCandidateLookupFind") == std::string::npos,
		"A4 source-pin: Registry-1 emit reads snapshot, not live LookupFind");

	const std::string bindBody = extractFunctionBody(
		uniText, "bool RtSmokeGeometryUniverse::RigidMeshCandidateLookupBind(");
	const std::string findBodyLock = extractFunctionBody(
		uniText, "bool RtSmokeGeometryUniverse::RigidMeshCandidateLookupFind(");
	const std::string findUnlockedBody = extractFunctionBody(
		uniText, "bool RtSmokeGeometryUniverse::RigidMeshCandidateLookupFindUnlocked(");
	const std::string bindUnlockedBody = extractFunctionBody(
		uniText, "bool RtSmokeGeometryUniverse::RigidMeshCandidateLookupBindUnlocked(");
	const std::string snapBody = extractFunctionBody(
		uniText, "void RtSmokeGeometryUniverse::SnapshotRigidMeshRegistryLookup");
	const std::string rebuildLockBody = extractFunctionBody(
		uniText, "bool RtSmokeGeometryUniverse::RebuildRigidMeshCandidateLookupFromRecords(");
	Check(!bindBody.empty() &&
		bindBody.find("m_rigidMeshCandidateLookupMutex") != std::string::npos &&
		bindBody.find("lock_guard") != std::string::npos &&
		bindBody.find("RigidMeshCandidateLookupBindUnlocked") != std::string::npos,
		"A4 source-pin: Persist Bind rehash holds the lookup mutex");
	Check(!findBodyLock.empty() &&
		findBodyLock.find("m_rigidMeshCandidateLookupMutex") != std::string::npos &&
		findBodyLock.find("lock_guard") != std::string::npos &&
		findBodyLock.find("RigidMeshCandidateLookupFindUnlocked") != std::string::npos,
		"A4 source-pin: LookupFind holds the lookup mutex");
	Check(!findUnlockedBody.empty() &&
		findUnlockedBody.find("m_rigidMeshCandidateLookup.find") != std::string::npos &&
		findUnlockedBody.find("lock_guard") == std::string::npos &&
		findUnlockedBody.find("m_rigidMeshCandidateLookupMutex") == std::string::npos,
		"A4 source-pin: FindUnlocked is the checked lookup.find and does not lock");
	Check(!bindUnlockedBody.empty() &&
		bindUnlockedBody.find("m_rigidMeshCandidateLookup[") != std::string::npos &&
		bindUnlockedBody.find("lock_guard") == std::string::npos &&
		bindUnlockedBody.find("m_rigidMeshCandidateLookupMutex") == std::string::npos,
		"A4 source-pin: BindUnlocked is the checked operator[] and does not lock");
	Check(!snapBody.empty() &&
		snapBody.find("m_rigidMeshCandidateLookupMutex") != std::string::npos &&
		snapBody.find("lock_guard") != std::string::npos &&
		snapBody.find("RigidMeshCandidateLookupFind") == std::string::npos &&
		snapBody.find("RigidMeshCandidateLookupBind") == std::string::npos,
		"A4 source-pin: snapshot copies under the same mutex and does not Bind");

	{
		const std::string a8ProbeBody = extractFunctionBody(
			uniText, "RtSmokeGeometryUniverse::ProbeA8S1LegacyCandidates");
		const std::string a8ProductBody = extractFunctionBody(
			uniText, "RtSmokeGeometryUniverse::CaptureA8S1LegacyCandidateProduct");
		size_t lookupFindCount = 0;
		size_t snapshotLookupFindCount = 0;
		for (size_t pos = uniText.find("m_rigidMeshCandidateLookup.find");
			pos != std::string::npos;
			pos = uniText.find("m_rigidMeshCandidateLookup.find", pos + 1))
		{
			++lookupFindCount;
		}
		for (size_t pos = snapBody.find("m_rigidMeshCandidateLookup.find");
			pos != std::string::npos;
			pos = snapBody.find("m_rigidMeshCandidateLookup.find", pos + 1))
		{
			++snapshotLookupFindCount;
		}
		const auto probeGuard = a8ProbeBody.find("EnsureRigidMeshCandidateLookupLive");
		const auto probeFind = a8ProbeBody.find("m_rigidMeshCandidateLookup.find");
		const auto productGuard = a8ProductBody.find("EnsureRigidMeshCandidateLookupLive");
		const auto productFind = a8ProductBody.find("m_rigidMeshCandidateLookup.find");
		Check(frameBatchAuthority || (lookupFindCount == 6 && snapshotLookupFindCount == 3 &&
			findUnlockedBody.find("m_rigidMeshCandidateLookup.find") != std::string::npos &&
			snapBody.find("m_rigidMeshCandidateLookup.find") != std::string::npos &&
			a8ProbeBody.find("m_rigidMeshCandidateLookupMutex") != std::string::npos &&
			a8ProductBody.find("m_rigidMeshCandidateLookupMutex") != std::string::npos &&
			probeGuard < probeFind && productGuard < productFind),
			"A4 source-pin: lookup reads remain locked while frame-batch uses owned lookup state");
	}
	Check(!rebuildLockBody.empty() &&
		rebuildLockBody.find("m_rigidMeshCandidateLookupMutex") != std::string::npos &&
		rebuildLockBody.find("lock_guard") != std::string::npos &&
		rebuildLockBody.find("RebuildRigidMeshCandidateLookupFromRecordsUnlocked") != std::string::npos &&
		rebuildLockBody.find("m_rigidMeshCandidateLookup.swap") == std::string::npos,
		"A4 source-pin: Rebuild holds the lookup mutex");

	auto innermostScopeContaining = [](const std::string& body, size_t pos)
		-> std::pair<size_t, size_t> {
		size_t bestOpen = std::string::npos;
		size_t bestClose = std::string::npos;
		if (pos == std::string::npos)
		{
			return { bestOpen, bestClose };
		}
		for (size_t index = 0; index < body.size() && index <= pos; ++index)
		{
			if (body[index] != '{')
			{
				continue;
			}
			int depth = 0;
			for (size_t cursor = index; cursor < body.size(); ++cursor)
			{
				if (body[cursor] == '{')
				{
					++depth;
				}
				else if (body[cursor] == '}')
				{
					--depth;
					if (depth == 0)
					{
						if (cursor >= pos)
						{
							bestOpen = index;
							bestClose = cursor;
						}
						break;
					}
				}
			}
		}
		return { bestOpen, bestClose };
	};

	const auto findLockPos = findBody.find(
		"std::lock_guard<std::mutex> lock(m_rigidMeshCandidateLookupMutex)");
	const auto findPushPos = findBody.find("m_rigidMeshCandidateRecords.push_back");
	const auto findBindUnlockedPos = findBody.find(
		"RigidMeshCandidateLookupBindUnlocked");
	const auto findPopPos = findBody.find("pop_back");
	const auto findLockScope = innermostScopeContaining(findBody, findLockPos);
	Check(frameBatchAuthority || (findLockPos != std::string::npos &&
		findPushPos != std::string::npos &&
		findBindUnlockedPos != std::string::npos &&
		findPopPos != std::string::npos &&
		findLockScope.first != std::string::npos &&
		findLockPos < findPushPos &&
		findPushPos < findBindUnlockedPos &&
		findBindUnlockedPos < findPopPos &&
		findLockScope.first < findLockPos &&
		findPushPos < findLockScope.second &&
		findBindUnlockedPos < findLockScope.second &&
		findPopPos < findLockScope.second &&
		findBody.find("RigidMeshCandidateLookupBind(") == std::string::npos),
		"A4 source-pin: new record and lookup publish under one legacy lock or batch commit");

	const auto snapLockPos = snapBody.find(
		"std::lock_guard<std::mutex> lock(m_rigidMeshCandidateLookupMutex)");
	const auto snapFindPos = snapBody.find("m_rigidMeshCandidateLookup.find");
	const auto snapRecordPos = snapBody.find("m_rigidMeshCandidateRecords[");
	const auto snapReadyPos = snapBody.find("entry.ready");
	const auto snapDeferredPos = snapBody.find("deferredSinceFrame");
	const auto snapBuiltPos = snapBody.find("gpuBlasBuildSubmitted");
	const auto snapBlasPos = snapBody.find("entry.builtBlas");
	Check(snapLockPos != std::string::npos &&
		snapFindPos != std::string::npos &&
		snapRecordPos != std::string::npos &&
		snapReadyPos != std::string::npos &&
		snapDeferredPos != std::string::npos &&
		snapBuiltPos != std::string::npos &&
		snapBlasPos != std::string::npos &&
		snapLockPos < snapFindPos &&
		snapLockPos < snapRecordPos &&
		snapFindPos < snapRecordPos &&
		snapRecordPos < snapReadyPos &&
		snapRecordPos < snapDeferredPos &&
		snapRecordPos < snapBuiltPos &&
		snapRecordPos < snapBlasPos,
		"A4 source-pin: Snapshot record field copies stay under the same mutex as lookup.find");

	const auto recordLockPos = recordBody.find(
		"std::lock_guard<std::mutex> recordLock(m_rigidMeshCandidateLookupMutex)");
	const auto recordRefreshPos = recordBody.find(
		"RefreshRigidMeshCandidateCpuCache(m_rigidMeshCandidateRecords[liveIndex])");
	const auto recordLockScope = innermostScopeContaining(recordBody, recordLockPos);
	Check(frameBatchAuthority || (recordLockPos != std::string::npos &&
		recordRefreshPos != std::string::npos &&
		recordLockScope.first != std::string::npos &&
		recordLockPos < recordRefreshPos &&
		recordRefreshPos < recordLockScope.second),
		"A4 source-pin: cache writes are legacy locked or off-side until batch commit");
	{
		const auto recordFindPos = recordBody.find(
			"RigidMeshCandidateLookupFindUnlocked");
		const auto recordHashCheckPos = recordBody.find(
			"m_rigidMeshCandidateRecords[liveIndex].meshHash == observation.meshHash");
		const auto recordMaterialPos = recordBody.find(
			"m_rigidMeshCandidateRecords[liveIndex].materialName");
		const auto recordModelPos = recordBody.find(
			"m_rigidMeshCandidateRecords[liveIndex].modelName");
		Check(frameBatchAuthority || (recordLockPos != std::string::npos &&
			recordFindPos != std::string::npos &&
			recordHashCheckPos != std::string::npos &&
			recordMaterialPos != std::string::npos &&
			recordModelPos != std::string::npos &&
			recordLockPos < recordFindPos &&
			recordFindPos < recordHashCheckPos &&
			recordHashCheckPos < recordMaterialPos &&
			recordHashCheckPos < recordModelPos &&
			recordFindPos < recordLockScope.second &&
			recordMaterialPos < recordLockScope.second &&
			recordModelPos < recordLockScope.second &&
			recordBody.find("m_rigidMeshCandidateRecords[recordIndex].materialName") == std::string::npos &&
			recordBody.find("m_rigidMeshCandidateRecords[recordIndex].modelName") == std::string::npos),
			"A4 source-pin: string writes are legacy re-found or off-side batch-owned");
	}
	{
		size_t recordsTokens = 0;
		size_t recordsOutside = 0;
		for (size_t pos = recordBody.find("m_rigidMeshCandidateRecords");
			pos != std::string::npos;
			pos = recordBody.find("m_rigidMeshCandidateRecords", pos + 1))
		{
			++recordsTokens;
			if (recordLockPos == std::string::npos ||
				pos < recordLockPos ||
				pos > recordLockScope.second)
			{
				++recordsOutside;
			}
		}
		Check(frameBatchAuthority || (recordsTokens >= 1 && recordsOutside == 0),
			"A4 source-pin: serial Record has no unlocked live-vector mutation");
		const auto hashSizePos = recordBody.find("m_frameRigidMeshCandidateHashes.size");
		const auto hashBucketPos = recordBody.find("m_frameRigidMeshCandidateHashes.bucket_count");
		const auto hashLoadPos = recordBody.find("m_frameRigidMeshCandidateHashes.max_load_factor");
		const auto hashInsertPos = recordBody.find("m_frameRigidMeshCandidateHashes.insert");
		Check(frameBatchAuthority || (hashSizePos != std::string::npos &&
			hashBucketPos != std::string::npos &&
			hashLoadPos != std::string::npos &&
			hashInsertPos != std::string::npos &&
			recordLockPos < hashSizePos &&
			recordLockPos < hashBucketPos &&
			recordLockPos < hashLoadPos &&
			recordLockPos < hashInsertPos &&
			hashSizePos < recordLockScope.second &&
			hashBucketPos < recordLockScope.second &&
			hashLoadPos < recordLockScope.second &&
			hashInsertPos < recordLockScope.second),
			"A4 source-pin: frame hashes publish only under legacy lock or batch commit");
	}
	{
		const char* recordTokens[] = {
			"gpuBlasCreated",
			"gpuBlasBuildSubmitted",
			"gpuBlasVertexCount",
			".rigidBlas",
			"deferredSinceFrame"
		};
		for (const char* token : recordTokens)
		{
			const auto tokenPos = recordBody.find(token);
			if (tokenPos == std::string::npos)
			{
				continue;
			}
			Check(recordLockPos != std::string::npos &&
				recordLockPos < tokenPos &&
				tokenPos < recordLockScope.second,
				"A4 source-pin: Record Snapshot-consumed field writes hold the lookup mutex");
		}
	}

	const auto persistLockPos = persistBody.find(
		"std::lock_guard<std::mutex> lock(m_rigidMeshCandidateLookupMutex)");
	const auto persistDeferredPos = persistBody.find("deferredSinceFrame");
	const auto persistUnlockedFind = persistBody.find(
		"RigidMeshCandidateLookupFindUnlocked");
	const auto persistLockScope = innermostScopeContaining(persistBody, persistLockPos);
	Check(persistLockPos != std::string::npos &&
		persistDeferredPos != std::string::npos &&
		persistUnlockedFind != std::string::npos &&
		persistLockScope.first != std::string::npos &&
		persistLockPos < persistDeferredPos &&
		persistUnlockedFind < persistDeferredPos &&
		persistDeferredPos < persistLockScope.second &&
		persistBody.find("RigidMeshCandidateLookupFind(") == std::string::npos &&
		persistBody.find("BuildBottomLevelAccelStruct") == std::string::npos,
		"A4 source-pin: Persist deferredSinceFrame write holds the lookup mutex");
	{
		const char* persistTokens[] = {
			"gpuBlasCreated",
			"gpuBlasBuildSubmitted",
			".rigidBlas"
		};
		for (const char* token : persistTokens)
		{
			const auto tokenPos = persistBody.find(token);
			if (tokenPos == std::string::npos)
			{
				continue;
			}
			Check(persistLockPos != std::string::npos &&
				persistLockPos < tokenPos &&
				tokenPos < persistLockScope.second,
				"A4 source-pin: Persist Snapshot-consumed BLAS writes hold the lookup mutex");
		}
	}

	Check(frameBatchAuthority || (findBody.find(kRigidMeshCandidateRecordVectorBoundaryName) != std::string::npos &&
		snapBody.find(kRigidMeshCandidateRecordVectorBoundaryName) != std::string::npos &&
		recordBody.find(kRigidMeshCandidateRecordVectorBoundaryName) != std::string::npos &&
		persistBody.find(kRigidMeshCandidateRecordVectorBoundaryName) != std::string::npos),
		"A4 source-pin: legacy readers and extracted batch share the candidate mutex boundary");

	const std::string gpuBody = extractFunctionBody(
		uniText, "RtPathTraceRigidBlasGpuStats RtSmokeGeometryUniverse::UpdateRigidBlasGpuScaffold");
	const auto gpuNvrhiPos = gpuBody.find("BuildBottomLevelAccelStruct");
	const auto gpuSubmittedPos = gpuBody.find("live.gpuBlasBuildSubmitted = true");
	const auto gpuCommitLockPos =
		gpuSubmittedPos == std::string::npos
			? std::string::npos
			: gpuBody.rfind(
				"std::lock_guard<std::mutex> lock(m_rigidMeshCandidateLookupMutex)",
				gpuSubmittedPos);
	const auto gpuFindPos =
		gpuNvrhiPos == std::string::npos
			? std::string::npos
			: gpuBody.find("RigidMeshCandidateLookupFindUnlocked", gpuNvrhiPos);
	const auto gpuBoundPos = gpuBody.find("RigidMeshGpuPublishCommitBound");
	const auto gpuBlasPos = gpuBody.find("live.rigidBlas =");
	const auto gpuLockScope = innermostScopeContaining(gpuBody, gpuCommitLockPos);
	Check(!gpuBody.empty() &&
		gpuNvrhiPos != std::string::npos &&
		gpuCommitLockPos != std::string::npos &&
		gpuFindPos != std::string::npos &&
		gpuBoundPos != std::string::npos &&
		gpuSubmittedPos != std::string::npos &&
		gpuBlasPos != std::string::npos &&
		gpuLockScope.first != std::string::npos &&
		gpuNvrhiPos < gpuCommitLockPos &&
		gpuCommitLockPos < gpuFindPos &&
		gpuFindPos < gpuBoundPos &&
		gpuBoundPos < gpuSubmittedPos &&
		gpuCommitLockPos < gpuBlasPos &&
		gpuSubmittedPos < gpuLockScope.second &&
		gpuBlasPos < gpuLockScope.second &&
		gpuNvrhiPos < gpuLockScope.first,
		"A4 source-pin: GPU BLAS field commit is after lock_guard and nvrhi build is before the lock");
	{
		size_t submittedAssigns = 0;
		size_t outsideLock = 0;
		for (size_t pos = gpuBody.find("gpuBlasBuildSubmitted = true");
			pos != std::string::npos;
			pos = gpuBody.find("gpuBlasBuildSubmitted = true", pos + 1))
		{
			++submittedAssigns;
			if (gpuCommitLockPos == std::string::npos ||
				pos < gpuCommitLockPos ||
				pos > gpuLockScope.second)
			{
				++outsideLock;
			}
		}
		Check(submittedAssigns >= 1 && outsideLock == 0,
			"A4 source-pin: gpuBlasBuildSubmitted=true cannot leave the GPU commit lock");
	}
	const auto gpuSnapLockPos = gpuBody.find(
		"std::lock_guard<std::mutex> lock(m_rigidMeshCandidateLookupMutex)");
	const auto gpuFirstRecordsPos = gpuBody.find("m_rigidMeshCandidateRecords");
	const auto gpuPlansPushPos = gpuBody.find("gpuPlans.push_back");
	Check(gpuSnapLockPos != std::string::npos &&
		gpuFirstRecordsPos != std::string::npos &&
		gpuPlansPushPos != std::string::npos &&
		gpuSnapLockPos < gpuFirstRecordsPos &&
		gpuSnapLockPos < gpuPlansPushPos &&
		gpuPlansPushPos < gpuNvrhiPos &&
		gpuBody.find("RigidMeshCandidateRecord& record") == std::string::npos &&
		gpuBody.find("const RigidMeshCandidateRecord& gpuPlan") != std::string::npos,
		"A4 source-pin: GPU first record-vector access is a locked owned-plan snapshot");
	{
		const std::string gpuAfterNvrhi =
			gpuNvrhiPos == std::string::npos
				? std::string()
				: gpuBody.substr(gpuNvrhiPos);
		Check(!gpuAfterNvrhi.empty() &&
			gpuAfterNvrhi.find("record.") == std::string::npos &&
			gpuAfterNvrhi.find("sample.meshHash = gpuPlan.meshHash") != std::string::npos &&
			gpuAfterNvrhi.find("sample.meshHash = record.") == std::string::npos,
			"A4 source-pin: no durable record& spans nvrhi; post-build samples use gpuPlan");
	}

	const std::string pruneBody = extractFunctionBody(
		uniText, "void RtSmokeGeometryUniverse::PruneRigidCachesToCurrentFrame");
	const auto pruneLockPos = pruneBody.find(
		"std::lock_guard<std::mutex> lock(m_rigidMeshCandidateLookupMutex)");
	const auto pruneSwapPos = pruneBody.find("m_rigidMeshCandidateRecords.swap");
	const auto pruneRebuildPos = pruneBody.find(
		"RebuildRigidMeshCandidateLookupFromRecordsUnlocked");
	const auto pruneBoundPos = pruneBody.find("RigidMeshRecordCompactCommitBound");
	const auto pruneRetirePos = pruneBody.find("RetireRigidMeshGpuResources");
	const auto pruneLockScope = innermostScopeContaining(pruneBody, pruneLockPos);
	Check(!pruneBody.empty() &&
		pruneLockPos != std::string::npos &&
		pruneSwapPos != std::string::npos &&
		pruneRebuildPos != std::string::npos &&
		pruneBoundPos != std::string::npos &&
		pruneRetirePos != std::string::npos &&
		pruneLockScope.first != std::string::npos &&
		pruneLockPos < pruneSwapPos &&
		pruneSwapPos < pruneRebuildPos &&
		pruneRebuildPos < pruneBoundPos &&
		pruneSwapPos < pruneLockScope.second &&
		pruneRebuildPos < pruneLockScope.second &&
		pruneBoundPos < pruneLockScope.second &&
		pruneRetirePos > pruneLockScope.second &&
		pruneBody.find("RebuildRigidMeshCandidateLookupFromRecords(") == std::string::npos,
		"A4 source-pin: EndFrame compact swap and RebuildUnlocked share one lock_guard");
	{
		const auto compactStart = pruneBody.find("detachedMeshRecords");
		const std::string compactSection =
			compactStart == std::string::npos
				? std::string()
				: pruneBody.substr(compactStart);
		const auto compactLockPos = compactSection.find(
			"std::lock_guard<std::mutex> lock(m_rigidMeshCandidateLookupMutex)");
		const auto compactRecordsPos = compactSection.find("m_rigidMeshCandidateRecords");
		const auto compactEmptyPos = compactSection.find("m_rigidMeshCandidateRecords.empty");
		Check(!compactSection.empty() &&
			compactLockPos != std::string::npos &&
			compactRecordsPos != std::string::npos &&
			compactEmptyPos != std::string::npos &&
			compactLockPos < compactRecordsPos &&
			compactLockPos < compactEmptyPos,
			"A4 source-pin: compact first record-vector token is after lock_guard");
	}
	{
		const auto clearLockPos = clearBody.find(
			"std::lock_guard<std::mutex> lock(m_rigidMeshCandidateLookupMutex)");
		const auto clearRecordsPos = clearBody.find("m_rigidMeshCandidateRecords");
		const auto clearRebuildPos = clearBody.find(
			"RebuildRigidMeshCandidateLookupFromRecordsUnlocked");
		const auto clearRetirePos = clearBody.find("RetireRigidMeshGpuResources");
		const auto clearLockScope = innermostScopeContaining(clearBody, clearLockPos);
		Check(clearLockPos != std::string::npos &&
			clearRecordsPos != std::string::npos &&
			clearRebuildPos != std::string::npos &&
			clearRetirePos != std::string::npos &&
			clearLockScope.first != std::string::npos &&
			clearLockPos < clearRecordsPos &&
			clearLockPos < clearRebuildPos &&
			clearRebuildPos < clearLockScope.second &&
			clearRecordsPos < clearLockScope.second &&
			clearRetirePos > clearLockScope.second,
			"A4 source-pin: Clear first record-vector token and RebuildUnlocked share the lock; Retire after");
	{
		size_t hashTokens = 0;
		size_t hashOutside = 0;
		for (size_t pos = clearBody.find("m_frameRigidMeshCandidateHashes");
			pos != std::string::npos;
			pos = clearBody.find("m_frameRigidMeshCandidateHashes", pos + 1))
		{
			++hashTokens;
			if (clearLockPos == std::string::npos ||
				pos < clearLockPos ||
				pos > clearLockScope.second)
			{
				++hashOutside;
			}
		}
		Check(hashTokens >= 1 &&
			hashOutside == 0 &&
			clearBody.find("detachedHashes.swap(m_frameRigidMeshCandidateHashes)") != std::string::npos &&
			clearLockPos < clearBody.find("detachedHashes.swap(m_frameRigidMeshCandidateHashes)") &&
			clearBody.find("detachedHashes.swap(m_frameRigidMeshCandidateHashes)") < clearLockScope.second,
			"A4 source-pin: Clear frame-hash swap/clear is inside the record-vector lock");
	}
	}
	Check(gpuBody.find(kRigidMeshCandidateRecordVectorBoundaryName) != std::string::npos &&
		pruneBody.find(kRigidMeshCandidateRecordVectorBoundaryName) != std::string::npos,
		"A4 source-pin: GPU commit and compact share the record-vector mutex");
	const std::string beginBody = extractFunctionBody(
		uniText, "void RtSmokeGeometryUniverse::BeginFrame(");
	const auto beginLockPos = beginBody.find(
		"std::lock_guard<std::mutex> lock(m_rigidMeshCandidateLookupMutex)");
	const auto beginHashClearPos = beginBody.find("m_frameRigidMeshCandidateHashes.clear");
	const auto beginRecordsPos = beginBody.find("m_rigidMeshCandidateRecords");
	const auto beginLockScope = innermostScopeContaining(beginBody, beginLockPos);
	Check(!beginBody.empty() &&
		beginLockPos != std::string::npos &&
		beginHashClearPos != std::string::npos &&
		beginRecordsPos != std::string::npos &&
		beginLockScope.first != std::string::npos &&
		beginLockPos < beginHashClearPos &&
		beginLockPos < beginRecordsPos &&
		beginHashClearPos < beginLockScope.second &&
		beginRecordsPos < beginLockScope.second,
		"A4 source-pin: BeginFrame hashes.clear and records range-for are after lock_guard");
	{
		size_t beginRecordsOutside = 0;
		for (size_t pos = beginBody.find("m_rigidMeshCandidateRecords");
			pos != std::string::npos;
			pos = beginBody.find("m_rigidMeshCandidateRecords", pos + 1))
		{
			if (beginLockPos == std::string::npos ||
				pos < beginLockPos ||
				pos > beginLockScope.second)
			{
				++beginRecordsOutside;
			}
		}
		Check(beginRecordsOutside == 0,
			"A4 source-pin: BeginFrame has no unlocked range-for on the live record vector");
	}

	{
		std::mutex seam;
		std::atomic<bool> snapshotHeld{ false };
		std::atomic<bool> writerTried{ false };
		std::atomic<bool> acquiredWhileHeld{ false };
		std::thread reader([&]() {
			std::lock_guard<std::mutex> lock(seam);
			snapshotHeld.store(true, std::memory_order_release);
			for (int spin = 0; spin < 10000000; ++spin)
			{
				if (writerTried.load(std::memory_order_acquire))
				{
					break;
				}
				std::this_thread::yield();
			}
		});
		std::thread writer([&]() {
			for (int spin = 0; spin < 10000000; ++spin)
			{
				if (snapshotHeld.load(std::memory_order_acquire))
				{
					break;
				}
				std::this_thread::yield();
			}
			if (seam.try_lock())
			{
				acquiredWhileHeld.store(true, std::memory_order_release);
				seam.unlock();
			}
			writerTried.store(true, std::memory_order_release);
		});
		reader.join();
		writer.join();
		Check(writerTried.load() &&
			!acquiredWhileHeld.load() &&
			RigidMeshRegistryRecordVectorBoundaryHolds(
				snapshotHeld.load(), acquiredWhileHeld.load()),
			"A4 seam: emit Snapshot mutex blocks unlocked record insert");
	}
}

void TestRegistryBuiltBlasSubmit()
{
	using namespace cpu_producer_publish;

	Check(!RigidBlasBuildStateIsSubmitted(true, false, false),
		"A4 unbuilt-BLAS: helper false for handle+created=false");
	Check(!RigidBlasBuildStateIsSubmitted(true, true, false),
		"A4 unbuilt-BLAS: helper false for handle+submitted=false");
	Check(!RigidBlasBuildStateIsSubmitted(true, false, true),
		"A4 unbuilt-BLAS: helper false for handle+created=false+submitted=true");
	Check(!RigidBlasBuildStateIsSubmitted(false, true, true),
		"A4 unbuilt-BLAS: helper false for handle-absent");
	Check(RigidBlasBuildStateIsSubmitted(true, true, true),
		"A4 unbuilt-BLAS: helper true only for handle+created+submitted");

	Check(!SmokeTlasKeepExtraInstance(false),
		"A4 unbuilt-BLAS: Keep rejects a null extra BLAS");
	Check(SmokeTlasKeepExtraInstance(true),
		"A4 unbuilt-BLAS: Keep accepts a non-null extra BLAS");

	const void* world = reinterpret_cast<const void*>(0x21);
	RigidRegistryInstanceKey key;
	key.world = world;
	key.worldGeneration = 1;
	key.index = 3;
	key.generation = 1;
	RegistryEligibleSurface unbuilt;
	unbuilt.instanceId = key;
	unbuilt.meshId = 0xA11u;
	unbuilt.blasToken = 0xB11u;
	unbuilt.presentRecorded = true;
	unbuilt.meshBlasReady = true;
	unbuilt.meshBlasBuilt = RigidBlasBuildStateIsSubmitted(true, true, false);
	Check(!unbuilt.meshBlasBuilt && !RegistrySurfaceMayEmitBuiltBlas(unbuilt),
		"A4 unbuilt-BLAS: helper-false surface must not emit");

	RegistryEligibleSurface built = unbuilt;
	built.meshBlasBuilt = RigidBlasBuildStateIsSubmitted(true, true, true);
	Check(built.meshBlasBuilt && RegistrySurfaceMayEmitBuiltBlas(built),
		"A4 unbuilt-BLAS: helper-true ready+token may emit");

	Check(!RigidPersistGpuPlanShouldProcess(true, false, false, 0, false),
		"A6 mode 0: unseen non-cached persist-only GPU plan stays skipped");
	Check(RigidPersistGpuPlanShouldProcess(true, false, false, 1, false),
		"A6 mode 1: valid unseen unbuilt persist-only GPU plan is included");
	Check(!RigidPersistGpuPlanShouldProcess(true, false, false, 1, true),
		"A6 mode 1: valid unseen already-submitted persist-only GPU plan stays skipped");
	Check(!RigidPersistGpuPlanShouldProcess(true, false, false, 2, false),
		"A6 forbidden mode 2 does not open the first-upload GPU arm");
	Check(RigidPersistGpuPlanPreferFirst(false, false, true, true) &&
		!RigidPersistGpuPlanPreferFirst(true, true, false, false),
		"A6 admission order: unseen unbuilt precedes already-built when limited");
	const bool afterCommitBuilt = RigidBlasBuildStateIsSubmitted(true, true, true);
	Check(afterCommitBuilt &&
		!RigidPersistGpuPlanShouldProcess(true, false, false, 1, afterCommitBuilt) &&
		RegistrySurfaceMayEmitBuiltBlas(built),
		"A6 successful commit snapshot built=true: no rebuild loop and emit Keep accepts");

	RigidSubmitBoundaryList extras;
	RegistryPublishCounters counters;
	Check(EmitCoalescedRegistryTlas(
		static_cast<int>(RegistryMode::DualWrite),
		std::vector<RegistryEligibleSurface>{ unbuilt },
		extras,
		counters) &&
		extras.records.empty() &&
		counters.registryEmitted == 0 &&
		counters.registrySuppressed >= 1,
		"A4 unbuilt-BLAS: emit produces no extraTlas InstanceDesc");
	RigidSubmitBoundaryList a6BuiltExtras;
	RegistryPublishCounters a6BuiltCounters;
	Check(EmitCoalescedRegistryTlas(
		static_cast<int>(RegistryMode::DualWrite),
		std::vector<RegistryEligibleSurface>{ built },
		a6BuiltExtras,
		a6BuiltCounters) &&
		a6BuiltExtras.records.size() == 1 &&
		a6BuiltCounters.registryEmitted == 1 &&
		RegistryGapSubmitBoundaryJoin(MakeRegistryGapProductFromSubmitBoundary(
			a6BuiltExtras.records[0], built.blasToken)),
		"A6 A5 extras: built-token eligible surface emits; unbuilt remains suppressed");

	std::vector<uint64_t> tokens = { 0xB1u, 0u, 0xB2u };
	const SmokeTlasExtraSanitizeStats sanitized = SanitizeSmokeTlasExtraBlasTokens(
		tokens, 0, 2, kPathTraceSmokeTlasMaxInstances);
	Check(tokens.size() == 2 && tokens[0] == 0xB1u && tokens[1] == 0xB2u &&
		sanitized.dropped >= 1 &&
		sanitized.built == 2 &&
		SmokeTlasSubmitAllowed(sanitized.submitted, sanitized.maxInstances),
		"A4 unbuilt-BLAS: Keep-based sanitize strips null extras");

	const fs::path sceneFromHarness =
		fs::path(__FILE__).parent_path() / ".." / "renderer" / "NVRHI" /
		"PathTraceSmokeSceneBuild.cpp";
	const fs::path sceneAbs(
		"E:/prog/rbdoom-3-bfg-rt-producer/neo/renderer/NVRHI/PathTraceSmokeSceneBuild.cpp");
	const fs::path accelFromHarness =
		fs::path(__FILE__).parent_path() / ".." / "renderer" / "NVRHI" /
		"PathTraceAcceleration.cpp";
	const fs::path accelAbs(
		"E:/prog/rbdoom-3-bfg-rt-producer/neo/renderer/NVRHI/PathTraceAcceleration.cpp");
	const fs::path uniFromHarness =
		fs::path(__FILE__).parent_path() / ".." / "renderer" / "NVRHI" /
		"PathTraceGeometryUniverse.cpp";
	const fs::path uniAbs(
		"E:/prog/rbdoom-3-bfg-rt-producer/neo/renderer/NVRHI/PathTraceGeometryUniverse.cpp");
	auto readText = [](const fs::path& a, const fs::path& b) -> std::string {
		for (const fs::path& candidate : { a, b })
		{
			std::ifstream in(candidate);
			if (!in)
			{
				continue;
			}
			return std::string(std::istreambuf_iterator<char>(in),
				std::istreambuf_iterator<char>());
		}
		return std::string();
	};
	auto extractFunctionBody = [](const std::string& text,
		const std::string& signature) -> std::string {
		const auto sigPos = text.find(signature);
		if (sigPos == std::string::npos)
		{
			return std::string();
		}
		const auto bracePos = text.find('{', sigPos);
		if (bracePos == std::string::npos)
		{
			return std::string();
		}
		int depth = 0;
		for (size_t index = bracePos; index < text.size(); ++index)
		{
			const char ch = text[index];
			if (ch == '{')
			{
				++depth;
			}
			else if (ch == '}')
			{
				--depth;
				if (depth == 0)
				{
					return text.substr(bracePos, index - bracePos + 1);
				}
			}
		}
		return std::string();
	};
	const std::string sceneText = readText(sceneFromHarness, sceneAbs);
	const std::string accelText = readText(accelFromHarness, accelAbs);
	const std::string uniText = readText(uniFromHarness, uniAbs);
	const std::string updateGpuBody = extractFunctionBody(
		uniText, "RtSmokeGeometryUniverse::UpdateRigidBlasGpuScaffold");
	const auto registryModePos = updateGpuBody.find("DecodeCpuProducerRegistryMode");
	const auto builtStatePos = updateGpuBody.find("RigidBlasBuildStateIsSubmitted");
	const auto preferPos = updateGpuBody.find("RigidPersistGpuPlanPreferFirst");
	const auto findMatchingDelimiter = [](const std::string& text,
		size_t openPos, char openChar, char closeChar) -> size_t
	{
		if (openPos == std::string::npos || openPos >= text.size() ||
			text[openPos] != openChar)
		{
			return std::string::npos;
		}
		int depth = 0;
		for (size_t index = openPos; index < text.size(); ++index)
		{
			if (text[index] == openChar)
			{
				++depth;
			}
			else if (text[index] == closeChar && --depth == 0)
			{
				return index;
			}
		}
		return std::string::npos;
	};
	const auto a6HelperControlsExactEligibilityGuard =
		[&](const std::string& functionBody) -> bool
	{
		const std::string helperName =
			"cpu_producer_publish::RigidPersistGpuPlanShouldProcess";
		const std::string exactCondition =
			"!cpu_producer_publish::RigidPersistGpuPlanShouldProcess("
			"gpuPlan.valid,gpuPlan.seenThisFrame,cachedRouteCandidate,"
			"registryMode,gpuPlanBuilt)";
		size_t searchPos = 0;
		size_t matchedGuards = 0;
		while ((searchPos = functionBody.find("if", searchPos)) != std::string::npos)
		{
			const bool identifierBefore = searchPos != 0 &&
				(std::isalnum(static_cast<unsigned char>(functionBody[searchPos - 1])) ||
					functionBody[searchPos - 1] == '_');
			const size_t afterIf = searchPos + 2;
			const bool identifierAfter = afterIf < functionBody.size() &&
				(std::isalnum(static_cast<unsigned char>(functionBody[afterIf])) ||
					functionBody[afterIf] == '_');
			if (identifierBefore || identifierAfter)
			{
				searchPos = afterIf;
				continue;
			}
			const size_t conditionOpen =
				functionBody.find_first_not_of(" \t\r\n", afterIf);
			if (conditionOpen == std::string::npos || functionBody[conditionOpen] != '(')
			{
				searchPos = afterIf;
				continue;
			}
			const size_t conditionClose = findMatchingDelimiter(
				functionBody, conditionOpen, '(', ')');
			if (conditionClose == std::string::npos)
			{
				return false;
			}
			const std::string condition = functionBody.substr(
				conditionOpen + 1, conditionClose - conditionOpen - 1);
			if (condition.find("!" + helperName) == std::string::npos)
			{
				searchPos = conditionClose + 1;
				continue;
			}
			std::string compactCondition;
			compactCondition.reserve(condition.size());
			for (const char ch : condition)
			{
				if (!std::isspace(static_cast<unsigned char>(ch)))
				{
					compactCondition.push_back(ch);
				}
			}
			if (compactCondition != exactCondition)
			{
				return false;
			}
			const size_t ifNegation = conditionOpen + 1 + condition.find('!');
			const size_t helperCall = functionBody.find(helperName, ifNegation);
			const size_t guardOpen = functionBody.find_first_not_of(
				" \t\r\n", conditionClose + 1);
			if (guardOpen == std::string::npos || functionBody[guardOpen] != '{')
			{
				return false;
			}
			const size_t guardClose = findMatchingDelimiter(
				functionBody, guardOpen, '{', '}');
			const size_t firstContinue = functionBody.find("continue", guardOpen + 1);
			const size_t meshRecords = guardClose == std::string::npos
				? std::string::npos
				: functionBody.find("++stats.meshRecords", guardClose + 1);
			const size_t additionalContinue = guardClose == std::string::npos
				? std::string::npos
				: functionBody.find("continue", guardClose + 1);
			if (ifNegation == std::string::npos || helperCall == std::string::npos ||
				guardClose == std::string::npos || firstContinue == std::string::npos ||
				meshRecords == std::string::npos ||
				!(ifNegation < helperCall && helperCall < conditionClose &&
					conditionClose < guardOpen && guardOpen < firstContinue &&
					firstContinue < guardClose && guardClose < meshRecords) ||
				(additionalContinue != std::string::npos &&
					additionalContinue < meshRecords))
			{
				return false;
			}
			++matchedGuards;
			searchPos = conditionClose + 1;
		}
		return matchedGuards == 1;
	};
	Check(!updateGpuBody.empty() &&
		registryModePos != std::string::npos &&
		builtStatePos != std::string::npos &&
		preferPos != std::string::npos &&
		a6HelperControlsExactEligibilityGuard(updateGpuBody),
		"A6 source-pin: the negated helper exclusively guards the eligibility continue before meshRecords");

	const std::string liveGuard =
		"if (!cpu_producer_publish::RigidPersistGpuPlanShouldProcess(\n"
		"                gpuPlan.valid,\n"
		"                gpuPlan.seenThisFrame,\n"
		"                cachedRouteCandidate,\n"
		"                registryMode,\n"
		"                gpuPlanBuilt))\n"
		"        {\n"
		"            continue;\n"
		"        }";
	const size_t liveGuardPos = updateGpuBody.find(liveGuard);
	Check(liveGuardPos != std::string::npos,
		"A6 mutation fixture locates the production eligibility guard");
	if (liveGuardPos != std::string::npos)
	{
		const auto replaceLiveGuard = [&](const std::string& replacement) {
			std::string mutation = updateGpuBody;
			mutation.replace(liveGuardPos, liveGuard.size(), replacement);
			return mutation;
		};
		const std::string helperIgnoredMutation = replaceLiveGuard(
			"cpu_producer_publish::RigidPersistGpuPlanShouldProcess(\n"
			"                gpuPlan.valid, gpuPlan.seenThisFrame,\n"
			"                cachedRouteCandidate, registryMode, gpuPlanBuilt);\n"
			"        if (!gpuPlan.valid ||\n"
			"            (!gpuPlan.seenThisFrame && !cachedRouteCandidate))\n"
			"        {\n"
			"            continue;\n"
			"        }");
		Check(!a6HelperControlsExactEligibilityGuard(helperIgnoredMutation),
			"A6 mutation: ignored helper plus restored legacy skip is rejected");

		const std::string continueOutsideMutation = replaceLiveGuard(
			"if (!cpu_producer_publish::RigidPersistGpuPlanShouldProcess(\n"
			"                gpuPlan.valid, gpuPlan.seenThisFrame,\n"
			"                cachedRouteCandidate, registryMode, gpuPlanBuilt))\n"
			"        {\n"
			"        }\n"
			"        continue;");
		Check(!a6HelperControlsExactEligibilityGuard(continueOutsideMutation),
			"A6 mutation: eligibility continue outside helper guard is rejected");

		const std::string helperRemovedMutation = replaceLiveGuard(
			"if (!gpuPlan.valid ||\n"
			"            (!gpuPlan.seenThisFrame && !cachedRouteCandidate))\n"
			"        {\n"
			"            continue;\n"
			"        }");
		Check(!a6HelperControlsExactEligibilityGuard(helperRemovedMutation),
			"A6 mutation: removed helper is rejected");

		const std::string nestedDuplicateMutation = replaceLiveGuard(
			"if (!cpu_producer_publish::RigidPersistGpuPlanShouldProcess(\n"
			"                gpuPlan.valid, gpuPlan.seenThisFrame,\n"
			"                cachedRouteCandidate, registryMode, gpuPlanBuilt))\n"
			"        {\n"
			"            if (!cpu_producer_publish::RigidPersistGpuPlanShouldProcess(\n"
			"                    gpuPlan.valid, gpuPlan.seenThisFrame,\n"
			"                    cachedRouteCandidate, registryMode, gpuPlanBuilt))\n"
			"            {\n"
			"                continue;\n"
			"            }\n"
			"        }");
		Check(!a6HelperControlsExactEligibilityGuard(nestedDuplicateMutation),
			"A6 mutation: nested duplicate exact helper guard is rejected");
	}

	const std::string wasBuiltBody = extractFunctionBody(
		uniText, "RtSmokeGeometryUniverse::RigidMeshCandidateBlasWasBuilt");
	const auto wasBuiltReturn = wasBuiltBody.rfind("return");
	const auto wasBuiltSemi =
		wasBuiltReturn == std::string::npos
			? std::string::npos
			: wasBuiltBody.find(';', wasBuiltReturn);
	const std::string wasBuiltReturnStmt =
		(wasBuiltReturn == std::string::npos ||
			wasBuiltSemi == std::string::npos)
			? std::string()
			: wasBuiltBody.substr(
				wasBuiltReturn, wasBuiltSemi - wasBuiltReturn + 1);
	Check(!wasBuiltBody.empty() &&
		wasBuiltBody.find("RigidBlasBuildStateIsSubmitted") != std::string::npos &&
		wasBuiltBody.find("record.rigidBlas") != std::string::npos &&
		wasBuiltBody.find("gpuBlasCreated") != std::string::npos &&
		wasBuiltBody.find("gpuBlasBuildSubmitted") != std::string::npos &&
		!wasBuiltReturnStmt.empty() &&
		wasBuiltReturnStmt.find("RigidBlasBuildStateIsSubmitted") != std::string::npos &&
		wasBuiltReturnStmt.find("gpuBlasCreated") != std::string::npos &&
		wasBuiltReturnStmt.find("gpuBlasBuildSubmitted") != std::string::npos &&
		wasBuiltReturnStmt.find("rigidBlas") != std::string::npos,
		"A4 source-pin: WasBuilt body is helper(handle, created, submitted); handle-only fails");

	const std::string builtBlasBody = extractFunctionBody(
		uniText, "RtSmokeGeometryUniverse::RigidMeshCandidateBuiltBlas");
	Check(!builtBlasBody.empty() &&
		builtBlasBody.find("RigidMeshCandidateBlasWasBuilt") != std::string::npos,
		"A4 source-pin: BuiltBlas returns null unless WasBuilt");

	const auto emitPos = sceneText.find("static void EmitCoalescedRegistryExtrasIntoLiveTlas");
	std::string emitFn;
	if (emitPos != std::string::npos)
	{
		emitFn = sceneText.substr(emitPos, 4500);
	}
	Check(!emitFn.empty() &&
		emitFn.find("SnapshotRigidMeshRegistryLookup") != std::string::npos &&
		emitFn.find("meshBlasBuilt") != std::string::npos &&
		emitFn.find("builtBlas") != std::string::npos &&
		emitFn.find("RigidMeshCandidateLookupFind") == std::string::npos,
		"A4 source-pin: emit requires a this-process built BLAS");

	const auto extrasPos = sceneText.find("const uint32_t extrasBefore");
	std::string extrasFn;
	if (extrasPos != std::string::npos)
	{
		extrasFn = sceneText.substr(extrasPos, 9000);
	}
	const auto extrasKeep = extrasFn.find("SmokeTlasKeepExtraInstance");
	const auto extrasBlas = extrasFn.find("bottomLevelAS");
	const auto extrasLog = extrasFn.find("extras=%u built=%u dropped=%u submitted=%u max=%u");
	Check(!extrasFn.empty() &&
		extrasKeep != std::string::npos &&
		extrasBlas != std::string::npos &&
		extrasFn.find("droppedNull") != std::string::npos &&
		extrasFn.find("continue") != std::string::npos &&
		extrasLog != std::string::npos &&
		extrasKeep < extrasLog &&
		extrasBlas < extrasLog,
		"A4 source-pin: SceneBuild extrasBefore Keep-skips !bottomLevelAS before extras= log");

	const std::string submitFn = extractFunctionBody(
		accelText, "bool SubmitSmokeAccelerationBuilds");
	const auto copyKeep = submitFn.find("SmokeTlasKeepExtraInstance");
	const auto copyKeep2 = (copyKeep == std::string::npos)
		? std::string::npos
		: submitFn.find("SmokeTlasKeepExtraInstance", copyKeep + 1);
	const auto leftoverKeep = (copyKeep2 == std::string::npos)
		? std::string::npos
		: submitFn.find("SmokeTlasKeepExtraInstance", copyKeep2 + 1);
	const auto copyBlas = submitFn.find("bottomLevelAS");
	const auto tlasNull = submitFn.find("if (!desc.tlas)");
	const auto allowPos = submitFn.find("SmokeTlasSubmitAllowed");
	const auto buildPos = submitFn.find("buildTopLevelAccelStruct");
	const auto leftoverFalse = (leftoverKeep == std::string::npos)
		? std::string::npos
		: submitFn.find("return false", leftoverKeep);
	Check(!submitFn.empty() &&
		copyKeep != std::string::npos &&
		copyKeep2 != std::string::npos &&
		leftoverKeep != std::string::npos &&
		copyBlas != std::string::npos &&
		tlasNull != std::string::npos &&
		allowPos != std::string::npos &&
		buildPos != std::string::npos &&
		leftoverFalse != std::string::npos &&
		copyKeep < leftoverKeep &&
		copyBlas < leftoverKeep &&
		leftoverKeep < leftoverFalse &&
		leftoverFalse < buildPos &&
		tlasNull < buildPos &&
		allowPos < buildPos,
		"A4 source-pin: Submit Keep-copy then leftover-null/null-tlas/!allowed before build");
}

void TestA8S1CanonicalIdentityObservation()
{
	PtCanonicalInstanceKey instance;
	instance.worldGeneration = 11;
	instance.renderDefIndex = 7;
	instance.renderDefGeneration = 3;
	instance.subInstanceKind = PtCanonicalSubInstanceKind::RigidSurface;
	instance.modelSurfaceIndex = 2;
	instance.jointSubmeshIndex = -1;

	PtCanonicalMeshKey mesh;
	mesh.sourceAssetId = 17;
	mesh.sourceAssetGeneration = 4;
	mesh.topologySignature = 23;
	mesh.sourceDomain = PtCanonicalMeshSourceDomain::RegisteredRenderModel;
	mesh.modelSurfaceIndex = 2;
	mesh.vertexFormat = 1;
	mesh.deformationClass = PtCanonicalDeformationClass::Rigid;
	mesh.vertexCount = 3;
	mesh.indexCount = 3;
	mesh.jointSubmeshIndex = -1;

	PtA8S1NormalizedRouteKey route;
	route.worldGeneration = instance.worldGeneration;
	route.renderDefIndex = instance.renderDefIndex;
	route.renderDefGeneration = instance.renderDefGeneration;
	route.modelName = "models/a8/rigid";
	route.modelSurfaceIndex = static_cast<int32_t>(instance.modelSurfaceIndex);
	PtA8S1PresentView present;
	present.instanceKey = instance;
	present.instanceHash = 0xA801;
	present.meshKey = mesh;
	present.meshHash = 0xA802;
	present.lastUpsertSequence = 16384;
	present.worldGeneration = 11;
	present.publicationGeneration = 5;
	present.modelName = route.modelName;
	Check(PtA8S1CompareRouteAssociation(route, present) == 0,
		"A8-S1 A: normalized W route agrees with P DTO tuple");

	const auto checkOneRouteMismatch = [&](uint32_t expected,
		const PtA8S1NormalizedRouteKey& changed, const char* name) {
		Check(PtA8S1CompareRouteAssociation(changed, present) == expected, name);
	};
	PtA8S1NormalizedRouteKey changed = route;
	++changed.worldGeneration;
	checkOneRouteMismatch(PT_A8_S1_ROUTE_WORLD_MISMATCH, changed,
		"A8-S1 A: world mismatch reports independently");
	changed = route; ++changed.renderDefIndex;
	checkOneRouteMismatch(PT_A8_S1_ROUTE_RENDER_DEF_INDEX_MISMATCH, changed,
		"A8-S1 A: renderDef index mismatch reports independently");
	changed = route; ++changed.renderDefGeneration;
	checkOneRouteMismatch(PT_A8_S1_ROUTE_RENDER_DEF_GENERATION_MISMATCH, changed,
		"A8-S1 A: renderDef generation mismatch reports independently");
	changed = route; changed.modelName += "_other";
	checkOneRouteMismatch(PT_A8_S1_ROUTE_MODEL_NAME_MISMATCH, changed,
		"A8-S1 A: model name mismatch reports independently");
	changed = route; ++changed.modelSurfaceIndex;
	checkOneRouteMismatch(PT_A8_S1_ROUTE_SURFACE_INDEX_MISMATCH, changed,
		"A8-S1 A: surface mismatch reports independently");

	PtA8S1BindingView binding;
	binding.found = true;
	binding.instanceKey = present.instanceKey;
	binding.instanceHash = present.instanceHash;
	binding.meshKey = present.meshKey;
	binding.meshHash = present.meshHash;
	binding.lastEventSequence = present.lastUpsertSequence;
	binding.worldGeneration = present.worldGeneration;
	binding.publicationGeneration = present.publicationGeneration;
	Check(PtA8S1CompareTransportIntegrity(present, binding).All(),
		"A8-S1 B: P canonical fields survive the R copy chain");
	PtA8S1BindingView changedBinding = binding;
	++changedBinding.meshHash;
	const PtA8S1TransportIntegrity changedIntegrity =
		PtA8S1CompareTransportIntegrity(present, changedBinding);
	Check(changedIntegrity.instanceKey && changedIntegrity.instanceHash &&
		changedIntegrity.meshKey && !changedIntegrity.meshHash,
		"A8-S1 B: mesh-hash transport disagreement is independent");

	Check(PtA8S1CompareFreshness(present, binding).Fresh(),
		"A8-S1 C: 16384-event boundary exact epoch and sequence is fresh");
	changedBinding = binding;
	++changedBinding.publicationGeneration;
	changedBinding.lastEventSequence = UINT64_MAX;
	const PtA8S1Freshness resetMismatch =
		PtA8S1CompareFreshness(present, changedBinding);
	Check(!resetMismatch.epochEqual && !resetMismatch.sequenceFresh,
		"A8-S1 C: publication reset checks epoch before sequence");
	changedBinding = binding;
	present.lastUpsertSequence = 3;
	changedBinding.lastEventSequence = 2;
	Check(!PtA8S1CompareFreshness(present, changedBinding).Fresh(),
		"A8-S1 C: multiple Upserts in one frame reject a partial prefix");
	changedBinding.lastEventSequence = 3;
	Check(PtA8S1CompareFreshness(present, changedBinding).Fresh(),
		"A8-S1 C: multiple Upserts accept the complete prefix");
	present.publicationGeneration = 6;
	present.lastUpsertSequence = 1;
	changedBinding.publicationGeneration = 5;
	changedBinding.lastEventSequence = 16384;
	Check(!PtA8S1CompareFreshness(present, changedBinding).Fresh(),
		"A8-S1 C: pre-reset R prefix cannot satisfy post-reset P");
	changedBinding.publicationGeneration = 6;
	changedBinding.lastEventSequence = 0;
	Check(!PtA8S1CompareFreshness(present, changedBinding).Fresh(),
		"A8-S1 C: post-reset partial R prefix is stale");
	changedBinding.lastEventSequence = 1;
	Check(PtA8S1CompareFreshness(present, changedBinding).Fresh(),
		"A8-S1 C: post-reset complete prefix is fresh");

	PtA8S1AliasSidecar alias;
	alias.BeginFrame(11, 1);
	const uint64_t legacyOpaqueKey = 0xA800000000000001ull;
	const uint64_t canonicalMeshHash = present.meshHash;
	const PtA8S1AliasSidecar::Update first =
		alias.Observe(legacyOpaqueKey, route, canonicalMeshHash);
	const PtA8S1AliasSidecar::Update second =
		alias.Observe(legacyOpaqueKey, route, canonicalMeshHash);
	Check(first.inserted && second.hit && alias.EntryCount() == 1 &&
		alias.Contains(legacyOpaqueKey, route, canonicalMeshHash),
		"A8-S1 alias: multiplicity classifies both occurrences without duplicating sidecar rows");
	const uint64_t measuredAliasEntries = alias.EntryCount();
	const uint64_t measuredAliasBytes = alias.ApproxBytes();
	present.worldGeneration = binding.worldGeneration;
	present.publicationGeneration = binding.publicationGeneration;
	present.lastUpsertSequence = binding.lastEventSequence;
	const auto buildLegacyProduct = [&](bool s1Enabled, int mutation) {
		PtA8S1LegacyProductSnapshot product;
		product.available = true;
		PtA8S1LegacyCandidateProductRecord candidateA;
		candidateA.legacyMeshHash = legacyOpaqueKey;
		candidateA.multiplicity = 3;
		candidateA.lookupMember = true;
		candidateA.blasToken = 0xB1A5;
		product.candidates.push_back(candidateA);
		PtA8S1LegacyCandidateProductRecord candidateB = candidateA;
		candidateB.legacyMeshHash = 0xA800000000000002ull;
		candidateB.multiplicity = 1;
		candidateB.blasToken = 0xB1A6;
		product.candidates.push_back(candidateB);
		PtA8S1LegacySubmittedProductRecord submitted;
		submitted.legacyMeshHash = legacyOpaqueKey;
		submitted.instanceMask = 0x02;
		for (uint32_t index = 0; index < submitted.transformBits.size(); ++index)
		{
			submitted.transformBits[index] = 0x3f000000u + index;
		}
		submitted.submittedBlasToken = 0xB1A5;
		submitted.selectedBlasToken = 0xB1A5;
		product.submitted.push_back(submitted);
		if (s1Enabled)
		{
			const uint32_t association =
				PtA8S1CompareRouteAssociation(route, present);
			const PtA8S1TransportIntegrity transport =
				PtA8S1CompareTransportIntegrity(present, binding);
			const PtA8S1Freshness freshness =
				PtA8S1CompareFreshness(present, binding);
			alias.Observe(legacyOpaqueKey, route, canonicalMeshHash);
			Check(association == 0 && transport.All() && freshness.Fresh(),
				"A8-S1 on/off: observation proof executed in enabled arm");
			// Mutation seam proves the normalized differential detects a
			// production-boundary violation rather than comparing a local to itself.
			if (mutation == 1)
			{
				++product.candidates[0].multiplicity;
			}
			else if (mutation == 2)
			{
				product.candidates[0].lookupMember = false;
			}
			else if (mutation == 3)
			{
				product.submitted[0].instanceMask = 0;
			}
			else if (mutation == 4)
			{
				++product.submitted[0].transformBits[0];
			}
			else if (mutation == 5)
			{
				++product.submitted[0].selectedBlasToken;
			}
		}
		PtA8S1NormalizeLegacyProductSnapshot(product);
		return product;
	};
	const PtA8S1LegacyProductSnapshot legacyProductOff =
		buildLegacyProduct(false, 0);
	const PtA8S1LegacyProductSnapshot legacyProductOn =
		buildLegacyProduct(true, 0);
	Check(PtA8S1LegacyProductDifferenceCount(
			legacyProductOff, legacyProductOn) == 0,
		"A8-S1 on/off: actual serialized candidate/lookup/final product differential is empty");
	for (int mutation = 1; mutation <= 5; ++mutation)
	{
		Check(PtA8S1LegacyProductDifferenceCount(
				legacyProductOff, buildLegacyProduct(true, mutation)) != 0,
			"A8-S1 on/off mutation: serializer fails on multiplicity/membership/mask/transform/BLAS changes");
	}
	Check(alias.Retire(instance) == 1 && alias.EntryCount() == 0,
		"A8-S1 alias: canonical Remove retires isolated diagnostic state");
	cpu_producer_publish::CompareFrameInput formatInput;
	formatInput.a8S1.enabled = true;
	formatInput.a8S1.available = true;
	formatInput.a8S1.reconciled = true;
	formatInput.a8S1.observedRouteProofReady = true;
	formatInput.a8S1.coverageComplete = true;
	formatInput.a8S1.buckets[8] = 1;
	formatInput.a8S1.normalizedRouteRequests = 1;
	formatInput.a8S1.presentBridgeAvailable = true;
	formatInput.a8S1.presentBridgeSampleCount = 1;
	PtA8S1PresentBridgeSample& bridgeSample =
		formatInput.a8S1.presentBridgeSamples[0];
	bridgeSample.presentTuple = route;
	bridgeSample.presentDto = true;
	bridgeSample.binding = true;
	bridgeSample.currentRoute = true;
	bridgeSample.everRoute = true;
	bridgeSample.legacyCandidateProbeAvailable = true;
	bridgeSample.routeLegacyKeys[0] = 0x1111222233334444ull;
	bridgeSample.routeLegacyKeyCount = 1;
	bridgeSample.candidateLegacyKeys[0] = 0x5555666677778888ull;
	bridgeSample.candidateLegacyKeyCount = 1;
	bridgeSample.outcome =
		PtA8S1PresentBridgeOutcome::AssociatedUnderThirdKey;
	formatInput.a8S1.presentBridgeOutcomes[
		static_cast<uint32_t>(
			PtA8S1PresentBridgeOutcome::AssociatedUnderThirdKey)] = 1;
	cpu_producer_publish::CompareFrameResult formatResult;
	const std::string formatted = cpu_producer_publish::FormatRegistryDumpText(
		formatInput, formatResult);
	Check(formatted.find("a8S1ProofA basis=W_route_tuple_vs_P_dto_tuple") !=
		std::string::npos &&
		formatted.find("a8S1ProofB basis=P_dto_canonical_vs_R_binding_copy_chain") !=
			std::string::npos &&
		formatted.find("a8S1ProofC basis=exact_world_publication_epoch_then_R_sequence_gte_P_lastUpsert") !=
			std::string::npos &&
		formatted.find("B9_associationAgreement=1") != std::string::npos &&
		formatted.find("steadyRouteTopologyPasses=0") != std::string::npos &&
		formatted.find("observedRouteProofReady=1") != std::string::npos &&
		formatted.find("s2GateReady") == std::string::npos &&
		formatted.find("coverageBasis=enumerated_call_site_opportunities_only_not_present_population_not_global_S2_gate") != std::string::npos &&
		formatted.find("a8S1PresentBridge sampleBasis=first8_P_dto_rigid_surface_records") != std::string::npos &&
		formatted.find("routeLegacyKeysBasis=current_or_sidecar_W_same_legacy_keyspace keys=0x1111222233334444") != std::string::npos &&
		formatted.find("candidateLegacyKeysBasis=direct_lookup_membership_post_health_guard keys=0x5555666677778888") != std::string::npos &&
		formatted.find("outcome=associated_under_third_key") != std::string::npos,
		"A8-S1 dump: clauses A/B, buckets and cost bases are separate and labelled");

	const fs::path sourceRoot = fs::path(__FILE__).parent_path() /
		".." / "renderer" / "NVRHI";
	auto readText = [](const fs::path& path) -> std::string {
		std::ifstream in(path);
		return in ? std::string(std::istreambuf_iterator<char>(in),
			std::istreambuf_iterator<char>()) : std::string();
	};
	auto countToken = [](const std::string& text, const std::string& token) {
		size_t count = 0;
		for (size_t pos = 0;
			(pos = text.find(token, pos)) != std::string::npos;
			pos += token.size())
		{
			++count;
		}
		return count;
	};
	const std::string drawText = readText(sourceRoot / "PathTraceDrawSurfCapture.cpp");
	const std::string entityText = readText(sourceRoot / "PathTraceEntityFeed.cpp");
	const std::string universeText = readText(sourceRoot / "PathTraceGeometryUniverse.cpp");
	const std::string lifecycleText = readText(sourceRoot / "PathTraceGeometryLifecycle.cpp");
	const std::string sceneText = readText(sourceRoot / "PathTraceSmokeSceneBuild.cpp");
	const std::string renderCommonText = readText(
		fs::path(__FILE__).parent_path() / ".." / "renderer" / "RenderCommon.h");
	Check(countToken(drawText, "BuildPathTraceRigidInstanceSnapshot(") == 4 &&
		countToken(entityText, "BuildPathTraceRigidInstanceSnapshot(") == 1 &&
		countToken(universeText, "BuildPathTraceRigidInstanceSnapshot(") == 1 &&
		countToken(drawText, "RecordA8S1RouteObservation(") == 4 &&
		countToken(entityText, "RecordA8S1RouteObservation(") == 1 &&
		countToken(universeText, "RecordA8S1RouteObservation(") >= 2,
		"A8-S1 producers: all six source call sites are enumerated and observed");
	Check(sceneText.find("route.route.modelName.empty()") !=
		std::string::npos,
		"A8-S1 B2 source-pin: reducer uses route.route.modelName before DTO lookup");
	const size_t compactPos = lifecycleText.find("MaybeCompactIdentityJournal();");
	const size_t dtoPos = lifecycleText.find(
		"CapturePresentIdentitySnapshot(presentSnapshot)", compactPos);
	const size_t planPos = lifecycleText.find(
		"PtPlanGeometryIdentityTransport(", dtoPos);
	Check(compactPos != std::string::npos && dtoPos != std::string::npos &&
		planPos != std::string::npos && compactPos < dtoPos && dtoPos < planPos &&
		lifecycleText.find("PtA8S1MaybeCompactIdentityJournal(") !=
			std::string::npos,
		"A8-S1 ordering: compaction rewrites survivors before DTO and immutable delta publication");
	Check(countToken(lifecycleText,
		"std::lock_guard<std::recursive_mutex> lock(shadowRecordsMutex)") >= 9 &&
		lifecycleText.find("mutable std::recursive_mutex shadowRecordsMutex") !=
			std::string::npos,
		"A8-S1 synchronization: one concrete mutex guards shadow writers and DTO capture");
	Check(renderCommonText.find("pathTraceGeometryIdentitySnapshot") != std::string::npos &&
		renderCommonText.find("pathTraceGeometryPresentIdentitySnapshot") != std::string::npos,
		"A8-S1 DTO: always-present Present witness is distinct from optional identity delta");
	const size_t diagnosticPos = sceneText.find(
		"static PtA8S1DiagnosticResult BuildA8S1CanonicalIdentityDiagnostic");
	const size_t fillPos = sceneText.find(
		"static void FillRegistryCompareFromFinalPhysical", diagnosticPos);
	const std::string diagnosticBody = diagnosticPos == std::string::npos ||
		fillPos == std::string::npos ? std::string() :
		sceneText.substr(diagnosticPos, fillPos - diagnosticPos);
	Check(!diagnosticBody.empty() &&
		diagnosticBody.find("const PtGeometryPresentIdentitySnapshot* dto") != std::string::npos &&
		diagnosticBody.find("FindCanonicalIdentityBinding") != std::string::npos &&
		diagnosticBody.find("route.legacyMeshHash,") != std::string::npos &&
		diagnosticBody.find("present->meshHash") != std::string::npos &&
		diagnosticBody.find("route.legacyMeshHash ==") == std::string::npos,
		"A8-S1 proof source-pin: W/P/R remain separate key spaces and alias source is P");
	Check(diagnosticBody.find("routeTopologyPasses = 0") != std::string::npos &&
		diagnosticBody.find("model->Surface") == std::string::npos,
		"A8-S1 cost: steady route diagnostic performs zero topology passes");
	const size_t productAssign = sceneText.find(
		"accelSubmitDesc.extraTlasInstances =");
	const size_t a8Call = sceneText.find(
		"FillRegistryCompareFromFinalPhysical(", productAssign);
	Check(productAssign != std::string::npos && a8Call != std::string::npos &&
		productAssign < a8Call &&
		sceneText.find("const std::vector<nvrhi::rt::InstanceDesc>& finalPhysical",
			diagnosticPos) != std::string::npos,
		"A8-S1 boundary: renderer product is fixed and const before observation");
	const size_t productCaptureBefore = sceneText.find(
		"legacyProductBefore = CaptureA8S1LegacyProductSnapshot(", fillPos);
	const size_t productObserver = sceneText.find(
		"compareIn.a8S1 = BuildA8S1CanonicalIdentityDiagnostic(",
		productCaptureBefore);
	const size_t productCaptureAfter = sceneText.find(
		"legacyProductAfter = CaptureA8S1LegacyProductSnapshot(",
		productObserver);
	const size_t productDifference = sceneText.find(
		"PtA8S1LegacyProductDifferenceCount(", productCaptureAfter);
	Check(productCaptureBefore != std::string::npos &&
		productObserver != std::string::npos &&
		productCaptureAfter != std::string::npos &&
		productDifference != std::string::npos &&
		productCaptureBefore < productObserver &&
		productObserver < productCaptureAfter &&
		productCaptureAfter < productDifference,
		"A8-S1 on/off: production serializer brackets the observer and checks the actual normalized product difference");
	Check(sceneText.find("s2GateReady") == std::string::npos,
		"A8-S1 naming: production dump cannot overclaim a global S2 gate");

	volatile uint64_t timingSink = 0;
	auto timeLoop = [&](bool withSequenceRewrite) {
		std::vector<uint64_t> sequences(16384, 0);
		const auto start = std::chrono::steady_clock::now();
		for (uint64_t repeat = 0; repeat < 64; ++repeat)
		{
			for (uint64_t index = 0; index < sequences.size(); ++index)
			{
				timingSink += index ^ repeat;
				if (withSequenceRewrite)
				{
					sequences[index] = index + 1;
				}
			}
		}
		return std::chrono::duration_cast<std::chrono::microseconds>(
			std::chrono::steady_clock::now() - start).count();
	};
	const int64_t baselineUs = timeLoop(false);
	const int64_t s1Us = timeLoop(true);
	const int64_t loadDeltaUs = std::max<int64_t>(0, s1Us - baselineUs);
	const uint64_t dtoBytes = sizeof(PtGeometryPresentIdentitySnapshot) +
		sizeof(PtGeometryPresentIdentityRecord) + route.modelName.size();
	std::cout << "A8S1_COST syntheticLoadDeltaUs=" << loadDeltaUs
		<< " stopThresholdUs=50000 steadyRouteTopologyPasses=0"
		<< " dtoInstances=1 dtoBytes=" << dtoBytes
		<< " aliasEntries=" << measuredAliasEntries
		<< " aliasBytes=" << measuredAliasBytes
		<< " timingSink=" << timingSink << "\n";
	Check(loadDeltaUs < 50000,
		"A8-S1 cost: synthetic 16384-event replay delta is below 50ms stop");
}

void TestA8S1LifecycleTransitions()
{
	struct Survivor
	{
		bool valid = true;
		PtCanonicalInstanceKey instanceKey;
		uint64_t instanceHash = 0;
		PtCanonicalMeshKey meshKey;
		uint64_t meshHash = 0;
		uint64_t lastUpsertSequence = 0;
	};

	std::vector<Survivor> survivors(3);
	for (uint32_t index = 0; index < survivors.size(); ++index)
	{
		Survivor& survivor = survivors[index];
		survivor.instanceKey.worldGeneration = 41;
		survivor.instanceKey.renderDefIndex = 100 + index;
		survivor.instanceKey.renderDefGeneration = 2;
		survivor.instanceKey.subInstanceKind =
			PtCanonicalSubInstanceKind::RigidSurface;
		survivor.instanceKey.modelSurfaceIndex = index;
		survivor.instanceKey.jointSubmeshIndex = -1;
		survivor.instanceHash = 0xA810 + index;
		survivor.meshKey.sourceAssetId = 70 + index;
		survivor.meshKey.sourceAssetGeneration = 1;
		survivor.meshKey.topologySignature = 900 + index;
		survivor.meshKey.sourceDomain =
			PtCanonicalMeshSourceDomain::RegisteredRenderModel;
		survivor.meshKey.modelSurfaceIndex = index;
		survivor.meshKey.vertexFormat = 1;
		survivor.meshKey.deformationClass =
			PtCanonicalDeformationClass::Rigid;
		survivor.meshKey.vertexCount = 3;
		survivor.meshKey.indexCount = 3;
		survivor.meshKey.jointSubmeshIndex = -1;
		survivor.meshHash = 0xA820 + index;
	}

	std::vector<PtGeometryIdentityTransportRecord> journal;
	size_t publishedRecordCount = 0;
	uint64_t publicationGeneration = 5;
	uint64_t publicationSequence = 9;
	const auto append = [&journal](const Survivor& survivor) {
		PtGeometryIdentityTransportRecord event;
		event.operation = PtGeometryIdentityOperation::Upsert;
		event.eventSequence = journal.size() + 1;
		event.instanceKey = survivor.instanceKey;
		event.instanceHash = survivor.instanceHash;
		event.meshKey = survivor.meshKey;
		event.meshHash = survivor.meshHash;
		journal.push_back(event);
		return event.eventSequence;
	};

	PtA8S1AppendUpsertAndStore(survivors[0], append);
	PtA8S1AppendUpsertAndStore(survivors[0], append);
	PtA8S1AppendUpsertAndStore(survivors[0], append);
	Check(survivors[0].lastUpsertSequence == 3 && journal.size() == 3,
		"A8-S1 lifecycle: multiple real Upserts in one frame store the latest sequence");
	while (journal.size() < PT_A8_S1_IDENTITY_JOURNAL_COMPACT_THRESHOLD)
	{
		PtA8S1AppendUpsertAndStore(
			survivors[journal.size() % survivors.size()], append);
	}
	publishedRecordCount = journal.size();
	const std::vector<PtGeometryIdentityTransportRecord> preCompactJournal = journal;
	const std::vector<Survivor> preCompactSurvivors = survivors;
	const uint64_t preCompactGeneration = publicationGeneration;
	Check(PtA8S1MaybeCompactIdentityJournal(
			journal,
			publishedRecordCount,
			publicationGeneration,
			publicationSequence,
			survivors,
			[](const Survivor& survivor) { return survivor.valid; },
			append),
		"A8-S1 lifecycle: actual 16384-event boundary invokes compaction");
	Check(publicationGeneration == preCompactGeneration + 1 &&
		publishedRecordCount == 0 && publicationSequence == 0 &&
		journal.size() == survivors.size(),
		"A8-S1 lifecycle: compaction resets publication and emits one replay Upsert per survivor");

	bool replayDtoAndPrefixFresh = true;
	std::array<PtGeometryPresentIdentityRecord, 3> dtoRecords = {};
	for (size_t index = 0; index < survivors.size(); ++index)
	{
		const Survivor& survivor = survivors[index];
		const PtGeometryIdentityTransportRecord& event = journal[index];
		dtoRecords[index].valid = 1;
		dtoRecords[index].instanceKey = survivor.instanceKey;
		dtoRecords[index].instanceHash = survivor.instanceHash;
		dtoRecords[index].meshKey = survivor.meshKey;
		dtoRecords[index].meshHash = survivor.meshHash;
		dtoRecords[index].lastUpsertSequence = survivor.lastUpsertSequence;
		PtA8S1PresentView p;
		p.instanceKey = dtoRecords[index].instanceKey;
		p.instanceHash = dtoRecords[index].instanceHash;
		p.meshKey = dtoRecords[index].meshKey;
		p.meshHash = dtoRecords[index].meshHash;
		p.lastUpsertSequence = dtoRecords[index].lastUpsertSequence;
		p.worldGeneration = 41;
		p.publicationGeneration = publicationGeneration;
		PtA8S1BindingView r;
		r.found = true;
		r.instanceKey = event.instanceKey;
		r.instanceHash = event.instanceHash;
		r.meshKey = event.meshKey;
		r.meshHash = event.meshHash;
		r.lastEventSequence = event.eventSequence;
		r.worldGeneration = 41;
		r.publicationGeneration = publicationGeneration;
		replayDtoAndPrefixFresh &=
			survivor.lastUpsertSequence == event.eventSequence &&
			PtA8S1CompareTransportIntegrity(p, r).All() &&
			PtA8S1CompareFreshness(p, r).Fresh();
	}
	Check(replayDtoAndPrefixFresh,
		"A8-S1 lifecycle: every replay survivor DTO sequence matches the transported prefix");

	std::vector<PtGeometryIdentityTransportRecord> mutatedJournal =
		preCompactJournal;
	std::vector<Survivor> mutatedSurvivors = preCompactSurvivors;
	size_t mutatedPublished = mutatedJournal.size();
	uint64_t mutatedGeneration = preCompactGeneration;
	uint64_t mutatedPublicationSequence = 9;
	const auto mutatedAppend = [&mutatedJournal](const Survivor& survivor) {
		PtGeometryIdentityTransportRecord event;
		event.operation = PtGeometryIdentityOperation::Upsert;
		event.eventSequence = mutatedJournal.size() + 1;
		event.instanceKey = survivor.instanceKey;
		event.instanceHash = survivor.instanceHash;
		event.meshKey = survivor.meshKey;
		event.meshHash = survivor.meshHash;
		mutatedJournal.push_back(event);
		return event.eventSequence;
	};
	PtA8S1MaybeCompactIdentityJournal(
		mutatedJournal,
		mutatedPublished,
		mutatedGeneration,
		mutatedPublicationSequence,
		mutatedSurvivors,
		[](const Survivor& survivor) { return survivor.valid; },
		mutatedAppend,
		false);
	bool mutationIncorrectlyFresh = true;
	for (size_t index = 0; index < mutatedSurvivors.size(); ++index)
	{
		mutationIncorrectlyFresh &=
			mutatedJournal[index].eventSequence >=
				mutatedSurvivors[index].lastUpsertSequence;
	}
	Check(!mutationIncorrectlyFresh,
		"A8-S1 lifecycle mutation proof: removing survivor rewrite makes the real transition test fail");

	const uint64_t generationBeforeExplicitReset = publicationGeneration;
	PtA8S1ResetIdentityPublication(
		journal,
		publishedRecordCount,
		publicationGeneration,
		publicationSequence);
	Check(journal.empty() && publishedRecordCount == 0 &&
		publicationGeneration == generationBeforeExplicitReset + 1 &&
		publicationSequence == 0,
		"A8-S1 lifecycle: explicit publication reset clears prefix and advances epoch");
	PtA8S1AppendUpsertAndStore(survivors[0], append);
	PtA8S1PresentView postResetP;
	postResetP.lastUpsertSequence = survivors[0].lastUpsertSequence;
	postResetP.worldGeneration = 41;
	postResetP.publicationGeneration = publicationGeneration;
	PtA8S1BindingView prefixR;
	prefixR.worldGeneration = 41;
	prefixR.publicationGeneration = generationBeforeExplicitReset;
	prefixR.lastEventSequence = PT_A8_S1_IDENTITY_JOURNAL_COMPACT_THRESHOLD;
	Check(!PtA8S1CompareFreshness(postResetP, prefixR).Fresh(),
		"A8-S1 lifecycle: complete pre-reset prefix cannot satisfy post-reset DTO");
	prefixR.publicationGeneration = publicationGeneration;
	prefixR.lastEventSequence = 0;
	Check(!PtA8S1CompareFreshness(postResetP, prefixR).Fresh(),
		"A8-S1 lifecycle: partial post-reset prefix is stale");
	prefixR.lastEventSequence = journal[0].eventSequence;
	Check(PtA8S1CompareFreshness(postResetP, prefixR).Fresh(),
		"A8-S1 lifecycle: complete post-reset prefix is fresh");

	for (uint32_t producer = 0;
		producer < static_cast<uint32_t>(PtA8S1RouteProducer::Count);
		++producer)
	{
		Check(PtA8S1ProducerCoverageRowReconciles(1, 1, 0) &&
			PtA8S1ProducerCoverageRowReconciles(0, 0, 0) &&
			!PtA8S1ProducerCoverageRowReconciles(1, 0, 0),
			"A8-S1 coverage matrix: each producer positive/zero-inapplicable row reconciles and a missed observation fails");
	}
}

void TestMaterialClassifyDeriveRingContract()
{
	bool emissiveTruthTableParity = true;
	for (int mask = 0; mask < 16; ++mask)
	{
		const bool emissive = (mask & 1) != 0;
		const bool hasImage = (mask & 2) != 0;
		const bool hasSafeTexture = (mask & 4) != 0;
		const bool skyEnvironment = (mask & 8) != 0;
		const bool expected = skyEnvironment ||
			(emissive && (hasSafeTexture || !hasImage));
		emissiveTruthTableParity &=
			RtSmokeMaterialEmissiveFactFromLocalFacts(
				emissive, hasImage, hasSafeTexture, skyEnvironment) == expected;
	}
	Check(emissiveTruthTableParity,
		"material emissive local-fact helper preserves universe decision truth table");
	Check(!RtSmokeMaterialEmissiveFactFromLocalFacts(true, true, false, false) &&
		RtSmokeMaterialEmissiveFactFromLocalFacts(true, true, true, false) &&
		RtSmokeMaterialEmissiveFactFromLocalFacts(true, false, false, false) &&
		RtSmokeMaterialEmissiveFactFromLocalFacts(false, false, false, true),
		"material emissive local facts cover unsafe image, safe image, authored no-image, and sky cases");

	RtPathTraceMaterialClassifySurface surfaces[3];
	RtSmokeTranslucentClassifierInfo classifiers[3];
	std::uint64_t materialIdentities[3] = { 0, 0x1000, 0x2000 };
	for (std::uint32_t ordinal = 0; ordinal < 3; ++ordinal)
	{
		surfaces[ordinal].ordinal = ordinal;
		surfaces[ordinal].materialSlot = ordinal;
		surfaces[ordinal].materialIdentity = materialIdentities[ordinal];
	}
	int viewToken = 0;
	RtPathTraceMaterialClassifyProduct product;
	product.viewIdentity = static_cast<std::uint64_t>(
		reinterpret_cast<std::uintptr_t>(&viewToken));
	product.surfaceCount = 3;
	product.classifierCount = 3;
	product.surfaces = surfaces;
	product.materialIdentities = materialIdentities;
	product.classifiers = classifiers;
	product.complete = true;
	Check(surfaces[0].materialSlot == 0 &&
		RtSmokeTranslucentClassifierInfoEqual(
			classifiers[0], RtSmokeTranslucentClassifierInfo()),
		"material classify lane gives absent material an explicit default product");

	RtPathTraceMaterialClassifyLaneContract lane;
	RtPathTraceMaterialClassifyBatchContract batch;
	Check(RtPathTraceMaterialClassifyBatchBegin(lane, batch, 3),
		"material classify lane begins the first root batch on its owner");
	lane.listIdentity = 0x1234;
	lane.allocations = 1;
	Check(RtPathTraceMaterialClassifyBatchTryQueue(batch) &&
		RtPathTraceMaterialClassifyBatchTryQueue(batch) &&
		RtPathTraceMaterialClassifyBatchTryQueue(batch) &&
		!RtPathTraceMaterialClassifyBatchTryQueue(batch) &&
		batch.queuedViews == 3,
		"material classify lane bounds three queued views and falls back before launch on exhaustion");
	Check(RtPathTraceMaterialClassifyBatchTryReserveOwnedBytes(
			batch, 10, 16) &&
		!RtPathTraceMaterialClassifyBatchTryReserveOwnedBytes(
			batch, 7, 16) &&
		batch.ownedBytes == 10 && batch.queuedViews == 3,
		"material classify lane enforces one cumulative batch byte cap without invalidating queued views");
	Check(RtPathTraceMaterialClassifyLaneCanHandoff(lane, batch) == false,
		"material classify lane refuses active-batch ownership handoff");
	RtPathTraceMaterialClassifyBatchNoteSubmitted(batch);
	Check(!RtPathTraceMaterialClassifyBatchMayRelease(batch),
		"material classify lane cannot reset or shut down while a submitted batch is live");
	Check(!RtPathTraceMaterialClassifyLaneCanHandoff(lane, batch),
		"material classify lane refuses submitted-list ownership handoff");
	RtPathTraceMaterialClassifyBatchNoteJoined(batch);
	Check(RtPathTraceMaterialClassifyBatchMayRelease(batch),
		"material classify lane releases only after the one owner-thread join");
	RtPathTraceMaterialClassifyLaneNoteBatchCompleted(lane, batch);
	const std::uintptr_t persistentListIdentity = lane.listIdentity;
	batch = RtPathTraceMaterialClassifyBatchContract();
	Check(RtPathTraceMaterialClassifyBatchBegin(lane, batch, 3) &&
		lane.listIdentity == persistentListIdentity && lane.reuses == 1,
		"material classify lane reuses one persistent list for a consecutive root batch");
	Check(!RtPathTraceMaterialClassifyBatchBegin(lane, batch, 3) &&
		!batch.accepting &&
		!RtPathTraceMaterialClassifyBatchTryQueue(batch),
		"material classify lane rejects stale Begin and cannot append current pointers to the old batch");
	batch = RtPathTraceMaterialClassifyBatchContract();
	Check(RtPathTraceMaterialClassifyLaneCanHandoff(lane, batch) &&
		RtPathTraceMaterialClassifyLaneNoteHandoff(lane, batch) &&
		!RtPathTraceMaterialClassifyLaneNoteHandoff(lane, batch),
		"material classify lane hands an idle post-Game/Draw list off exactly once");
	Check(RtPathTraceMaterialClassifyConfigurationNeedsSample(7, 8) &&
		!RtPathTraceMaterialClassifyConfigurationNeedsSample(8, 8),
		"material classify lane samples configuration once for all views in one frame");
	Check(RtPathTraceMaterialClassifyProductShapeValid(
			&product, reinterpret_cast<const viewDef_t*>(&viewToken), 3),
		"material classify lane accepts one exact-frame whole-view product");

	surfaces[1].ordinal = 2;
	Check(!RtPathTraceMaterialClassifyProductShapeValid(
			&product, reinterpret_cast<const viewDef_t*>(&viewToken), 3),
		"material classify lane rejects ordinal mutation");
	surfaces[1].ordinal = 1;
	Check(!RtPathTraceMaterialClassifyProductShapeValid(
			&product, reinterpret_cast<const viewDef_t*>(&viewToken), 2),
		"material classify lane rejects partial-view product");
	int otherViewToken = 0;
	Check(!RtPathTraceMaterialClassifyProductShapeValid(
			&product, reinterpret_cast<const viewDef_t*>(&otherViewToken), 3),
		"material classify lane keeps exact view and subview products non-aliased");
	product.complete = false;
	Check(!RtPathTraceMaterialClassifyProductShapeValid(
			&product, reinterpret_cast<const viewDef_t*>(&viewToken), 3),
		"material classify lane never publishes incomplete worker output");
	product.complete = true;

	std::uint64_t hashIdentities[8] = {};
	std::uint32_t hashSlots[8] = {};
	std::uint32_t slot = UINT32_MAX;
	bool inserted = false;
	Check(RtPathTraceMaterialClassifyFindOrInsert(
			0x1000, hashIdentities, hashSlots, 8, 0, slot, inserted) &&
		inserted && slot == 0,
		"material classify lane inserts first sealed-view material");
	Check(RtPathTraceMaterialClassifyFindOrInsert(
			0x1000, hashIdentities, hashSlots, 8, 1, slot, inserted) &&
		!inserted && slot == 0,
		"material classify lane deduplicates repeated material identity");
	Check(RtPathTraceMaterialClassifyFindOrInsert(
			0x2000, hashIdentities, hashSlots, 8, 1, slot, inserted) &&
		inserted && slot == 1,
		"material classify lane preserves first-seen material-slot order");
	Check(!RtPathTraceMaterialClassifyFindOrInsert(
			0, hashIdentities, hashSlots, 8, 2, slot, inserted) &&
		!RtPathTraceMaterialClassifyFindOrInsert(
				0x3000, hashIdentities, hashSlots, 3, 2, slot, inserted),
		"material classify lane rejects null identity and invalid bounded hash storage");

	RtSmokeTranslucentClassifierInput absentInput;
	absentInput.sortIsGuiOrSubview = true;
	absentInput.stageCount = 0;
	std::strcpy(absentInput.materialName, "guis/poison-null-material");
	const RtSmokeTranslucentClassifierInfo absentInfo =
		BuildSmokeTranslucentClassifierInfoFromSource(
			absentInput, absentInput.materialName, 0,
			[](std::int32_t) { return RtSmokeTranslucentClassifierStageInput(); });
	Check(RtSmokeTranslucentClassifierInfoEqual(
			absentInfo, RtSmokeTranslucentClassifierInfo()),
		"material classifier pure kernel closes null material to default record");

	RtSmokeTranslucentClassifierInput classifierInput;
	classifierInput.materialPresent = true;
	classifierInput.stageCount = 2;
	classifierInput.sortIsDecal = true;
	classifierInput.polygonOffsetDecal = true;
	std::strcpy(classifierInput.materialName, "textures/decals/panel");
	RtSmokeTranslucentClassifierStageInput classifierStages[2];
	classifierStages[0].valid = true;
	classifierStages[0].isDiffuseStage = true;
	classifierStages[1].valid = true;
	classifierStages[1].isAdditiveBlend = true;
	const RtSmokeTranslucentClassifierInfo boundedInfo =
		BuildSmokeTranslucentClassifierInfoFromSource(
			classifierInput, classifierInput.materialName, 2,
			[classifierStages](std::int32_t stageIndex) {
				return classifierStages[stageIndex];
			});
	const RtSmokeTranslucentClassifierInfo directInfo =
		BuildSmokeTranslucentClassifierInfoFromSource(
			classifierInput,
			classifierInput.materialName,
			2,
			[classifierStages](std::int32_t stageIndex) {
				return classifierStages[stageIndex];
			});
	Check(RtSmokeTranslucentClassifierInfoEqual(boundedInfo, directInfo) &&
		boundedInfo.nameLooksDecal && boundedInfo.hasDiffuseStage &&
		boundedInfo.hasAdditiveBlend,
		"material classifier bounded POD and direct semantic authority agree");

	std::string boundaryName(
		RT_SMOKE_TRANSLUCENT_CLASSIFIER_NAME_CAPACITY - 1, 'x');
	std::string overlongName(
		RT_SMOKE_TRANSLUCENT_CLASSIFIER_NAME_CAPACITY + 16, 'x');
	overlongName.replace(overlongName.size() - 3, 3, "GUI");
	Check(RtSmokeTranslucentClassifierCaptureFits(
			boundaryName.size(), 0, 0) &&
		!RtSmokeTranslucentClassifierCaptureFits(
			overlongName.size(), 0, 0),
		"material classifier bounded capture accepts exact name boundary and rejects overflow");
	classifierInput.stageCount = 0;
	const RtSmokeTranslucentClassifierInfo overlongDirect =
		BuildSmokeTranslucentClassifierInfoFromSource(
			classifierInput,
			overlongName.c_str(),
			0,
			[](std::int32_t) {
				return RtSmokeTranslucentClassifierStageInput();
			});
	Check(overlongDirect.nameLooksGui,
		"material classifier direct serial authority preserves overlong-name semantics after bounded capture rejects");

	classifierInput.stageCount = 2;
	Check(!RtSmokeTranslucentClassifierCaptureFits(
			std::strlen(classifierInput.materialName), 2, 1),
		"material classifier bounded capture rejects stage-capacity overflow");
	const RtSmokeTranslucentClassifierInfo overCapacityDirect =
		BuildSmokeTranslucentClassifierInfoFromSource(
			classifierInput,
			classifierInput.materialName,
			2,
			[classifierStages](std::int32_t stageIndex) {
				return classifierStages[stageIndex];
			});
	Check(overCapacityDirect.hasDiffuseStage &&
		overCapacityDirect.hasAdditiveBlend,
		"material classifier direct serial authority preserves over-capacity stage semantics");

	const fs::path ringFromHarness =
		fs::path(__FILE__).parent_path() / ".." / "renderer" / "NVRHI" /
		"PathTraceCaptureDeriveRing.cpp";
	const fs::path ringAbsolute(
		"E:/prog/rbdoom-3-bfg-rt-producer/neo/renderer/NVRHI/PathTraceCaptureDeriveRing.cpp");
	std::string ringSource;
	for (const fs::path& candidate : { ringFromHarness, ringAbsolute })
	{
		std::ifstream input(candidate);
		if (input)
		{
			ringSource.assign(std::istreambuf_iterator<char>(input),
				std::istreambuf_iterator<char>());
			break;
		}
	}
	const std::size_t captureBegin = ringSource.find(
		"void CapturePathTraceMaterialClassifyFrontendInput");
	const std::size_t joinBegin = ringSource.find(
		"void JoinPathTraceMaterialClassifyFrame");
	const std::string captureSource =
		captureBegin != std::string::npos && joinBegin != std::string::npos
			? ringSource.substr(captureBegin, joinBegin - captureBegin)
			: std::string();
	const std::size_t firstWait = ringSource.find("->Wait()");
	const std::size_t shutdownLaneBegin = ringSource.find(
		"void ShutdownPathTraceMaterialClassifyLane");
	const std::string joinSource =
		joinBegin != std::string::npos && shutdownLaneBegin != std::string::npos
			? ringSource.substr(joinBegin, shutdownLaneBegin - joinBegin)
			: std::string();
	const std::string shutdownLaneSource =
		shutdownLaneBegin != std::string::npos
			? ringSource.substr(shutdownLaneBegin)
			: std::string();
	const std::size_t jobBegin = ringSource.find(
		"struct RtPathTraceMaterialClassifyJob");
	const std::size_t ownerAssociationBegin = ringSource.find(
		"struct RtPathTraceMaterialClassifyOwnerAssociation");
	const std::string workerSchema =
		jobBegin != std::string::npos && ownerAssociationBegin != std::string::npos
			? ringSource.substr(jobBegin, ownerAssociationBegin - jobBegin)
			: std::string();
	Check(!ringSource.empty() &&
		ringSource.find("BeginPathTraceMaterialClassifyFrame") !=
			std::string::npos &&
		joinBegin != std::string::npos &&
		captureSource.find("->Submit") == std::string::npos &&
		captureSource.find("->Wait") == std::string::npos &&
		!joinSource.empty() &&
		joinSource.find("FreeJobList") == std::string::npos &&
		!shutdownLaneSource.empty() &&
		shutdownLaneSource.find("FreeJobList") != std::string::npos &&
		shutdownLaneSource.find("->AddJob(") == std::string::npos &&
		shutdownLaneSource.find("->Submit(") == std::string::npos &&
		shutdownLaneSource.find("->Wait(") == std::string::npos &&
		ringSource.find("AllocJobList") ==
			ringSource.rfind("AllocJobList") &&
		firstWait != std::string::npos &&
		ringSource.find("->Wait()", firstWait + 1) == std::string::npos &&
		!workerSchema.empty() &&
		workerSchema.find("viewDef_t") == std::string::npos &&
		workerSchema.find("idMaterial") == std::string::npos &&
		workerSchema.find("drawSurf") == std::string::npos &&
		ringSource.find("AcquirePathTraceMaterialClassifyResult") ==
			std::string::npos &&
		ringSource.find("RtPathTraceMaterialClassifySlotState") ==
			std::string::npos &&
		ringSource.find("compare_exchange") == std::string::npos &&
		ringSource.find("DropReady") == std::string::npos,
		"material classify lane source-pin: views queue without per-view waits, join once, and contain no detached ring transitions");

	const fs::path renderInitFromHarness =
		fs::path(__FILE__).parent_path() / ".." / "renderer" /
		"RenderSystem_init.cpp";
	const fs::path renderInitAbsolute(
		"E:/prog/rbdoom-3-bfg-rt-producer/neo/renderer/RenderSystem_init.cpp");
	std::string renderInitSource;
	for (const fs::path& candidate : { renderInitFromHarness, renderInitAbsolute })
	{
		std::ifstream input(candidate, std::ios::binary);
		if (input)
		{
			renderInitSource.assign(std::istreambuf_iterator<char>(input),
				std::istreambuf_iterator<char>());
			break;
		}
	}
	const std::size_t rendererShutdown = renderInitSource.find(
		"void idRenderSystemLocal::Shutdown()");
	const std::size_t gameDrain = renderInitSource.find(
		"commonLocal.WaitGameThread();", rendererShutdown);
	const std::size_t laneHandoff = renderInitSource.find(
		"ShutdownPathTraceMaterialClassifyLane();", rendererShutdown);
	Check(rendererShutdown != std::string::npos &&
		gameDrain != std::string::npos && laneHandoff != std::string::npos &&
		gameDrain < laneHandoff,
		"material classify lane source-pin: renderer shutdown drains Game/Draw before the one idle MainThread handoff/free");

	const fs::path commonFromHarness =
		fs::path(__FILE__).parent_path() / ".." / "framework" / "Common.cpp";
	const fs::path commonAbsolute(
		"E:/prog/rbdoom-3-bfg-rt-producer/neo/framework/Common.cpp");
	std::string commonSource;
	for (const fs::path& candidate : { commonFromHarness, commonAbsolute })
	{
		std::ifstream input(candidate, std::ios::binary);
		if (input)
		{
			commonSource.assign(std::istreambuf_iterator<char>(input),
				std::istreambuf_iterator<char>());
			break;
		}
	}
	const std::size_t commonShutdown = commonSource.find(
		"void idCommonLocal::Shutdown()");
	const std::size_t stopFutureFrames = commonSource.find(
		"com_shuttingDown = true;", commonShutdown);
	const std::size_t rendererShutdownCall = commonSource.find(
		"renderSystem->Shutdown();", commonShutdown);
	Check(commonShutdown != std::string::npos &&
		stopFutureFrames != std::string::npos &&
		rendererShutdownCall != std::string::npos &&
		stopFutureFrames < rendererShutdownCall,
		"material classify lane source-pin: common shutdown forbids future frames before renderer ownership handoff");

	const fs::path classifierFromHarness =
		fs::path(__FILE__).parent_path() / ".." / "renderer" / "NVRHI" /
		"PathTraceDoomMaterialClassifier.cpp";
	const fs::path classifierAbsolute(
		"E:/prog/rbdoom-3-bfg-rt-producer/neo/renderer/NVRHI/PathTraceDoomMaterialClassifier.cpp");
	std::string classifierSource;
	for (const fs::path& candidate : { classifierFromHarness, classifierAbsolute })
	{
		std::ifstream input(candidate);
		if (input)
		{
			classifierSource.assign(std::istreambuf_iterator<char>(input),
				std::istreambuf_iterator<char>());
			break;
		}
	}
	const std::size_t serialOverload = classifierSource.find(
		"BuildSmokeTranslucentClassifierInfo(const idMaterial* material)");
	const std::string serialBody = serialOverload == std::string::npos
		? std::string()
		: classifierSource.substr(serialOverload, 1400);
	Check(!classifierSource.empty() &&
		classifierSource.find("RtSmokeTranslucentClassifierCaptureFits(") !=
			std::string::npos &&
		serialBody.find("BuildSmokeTranslucentClassifierInfoFromSource(") !=
			std::string::npos &&
		serialBody.find("CaptureSmokeTranslucentClassifierInput(") ==
			std::string::npos,
		"material classifier source-pin: bounded capture rejects through shared bound while serial overload stays direct and unbounded");

	const fs::path nvrhiFromHarness =
		fs::path(__FILE__).parent_path() / ".." / "renderer" / "NVRHI";
	const fs::path nvrhiAbsolute(
		"E:/prog/rbdoom-3-bfg-rt-producer/neo/renderer/NVRHI");
	auto readNvrhiSource = [&](const char* fileName) {
		std::string source;
		for (const fs::path& root : { nvrhiFromHarness, nvrhiAbsolute })
		{
			std::ifstream input(root / fileName, std::ios::binary);
			if (input)
			{
				source.assign(std::istreambuf_iterator<char>(input),
					std::istreambuf_iterator<char>());
				break;
			}
		}
		return source;
	};
	const std::string entityFeedSource =
		readNvrhiSource("PathTraceEntityFeed.cpp");
	const std::string materialUniverseSource =
		readNvrhiSource("PathTraceMaterialUniverse.cpp");
	const std::string materialTableSource =
		readNvrhiSource("PathTraceDynamicMaterialState.cpp");
	Check(!entityFeedSource.empty() &&
		entityFeedSource.find("GetSmokeMaterialUniverseFacts") == std::string::npos &&
		entityFeedSource.find("PathTraceMaterialUniverse.h") == std::string::npos &&
		entityFeedSource.find("RtSmokeMaterialEmissiveFactFromLocalFacts(") !=
			std::string::npos &&
		materialUniverseSource.find(
			"RtSmokeMaterialEmissiveFactFromLocalFacts(") != std::string::npos,
		"M0 ownership: EntityFeed derives the shared emissive fact without reading or mutating the persistent universe");
	Check(!materialTableSource.empty() &&
		materialTableSource.find(
			"PT Material Table Signature Cache") != std::string::npos &&
		materialTableSource.find(
			"PT Material Table Universe Entries") != std::string::npos &&
		materialTableSource.find(
			"PT Material Table Populate Slots") != std::string::npos &&
		materialTableSource.find(
			"PT Material Table Dirty Rows") != std::string::npos &&
		materialTableSource.find(
			"PT Material Table Hit Remap") != std::string::npos,
		"M0 observation: all five material-table attribution families remain source-pinned");
}

void TestCommittedDynamicGeometryContract()
{
	std::uint64_t tokenCounter = 0;
	const std::uint64_t firstToken =
		RtPathTraceCommittedCaptureNextToken(tokenCounter);
	RtPathTraceCommittedCaptureBatchContract batch;
	Check(firstToken == 1 &&
		RtPathTraceCommittedCaptureBegin(batch, 3, firstToken) &&
		batch.active && batch.accepting && batch.captureToken == firstToken,
		"committed geometry lane begins an owner batch");
	Check(RtPathTraceCommittedCaptureReserve(batch, 12, 16) &&
		RtPathTraceCommittedCaptureQueue(batch) &&
		batch.ownedBytes == 12,
		"committed geometry lane reserves and queues while accepting");
	const std::uint32_t staleQueued = batch.queuedViews;
	const std::size_t staleOwned = batch.ownedBytes;
	Check(!RtPathTraceCommittedCaptureBegin(batch, 3, firstToken + 1) &&
		batch.active && !batch.accepting &&
		batch.captureToken == firstToken &&
		batch.queuedViews == staleQueued && batch.ownedBytes == staleOwned &&
		!RtPathTraceCommittedCaptureQueue(batch) &&
		!RtPathTraceCommittedCaptureReserve(batch, 1, 16) &&
		batch.queuedViews == staleQueued && batch.ownedBytes == staleOwned,
		"committed geometry stale Begin stops acceptance without discarding queued ownership");
	batch.submitted = true;
	Check(!RtPathTraceCommittedCaptureMayRelease(batch),
		"committed geometry lane cannot release launched arena readers");
	batch.joined = true;
	Check(RtPathTraceCommittedCaptureMayRelease(batch),
		"committed geometry lane releases only after owner join");
	RtPathTraceCommittedCaptureBatchContract capBatch;
	Check(RtPathTraceCommittedCaptureBegin(capBatch, 1, firstToken + 1) &&
		RtPathTraceCommittedCaptureReserve(capBatch, 8, 16) &&
		RtPathTraceCommittedCaptureQueue(capBatch),
		"committed geometry cap fixture preserves its first queued view");
	const std::uint32_t capQueued = capBatch.queuedViews;
	const std::size_t capOwned = capBatch.ownedBytes;
	Check(!RtPathTraceCommittedCaptureQueue(capBatch) &&
		capBatch.queuedViews == capQueued && capBatch.ownedBytes == capOwned,
		"committed geometry cap rejection cannot disturb earlier queued state");
	Check(RtPathTraceCommittedCaptureKeyMatches(
			0x1000, 7, firstToken, 0x1000, 7, firstToken) &&
		!RtPathTraceCommittedCaptureKeyMatches(
			0x1000, 7, firstToken, 0x1000, 7, firstToken + 1) &&
		!RtPathTraceCommittedCaptureKeyMatches(
			0x1000, 7, firstToken, 0x2000, 7, firstToken) &&
		!RtPathTraceCommittedCaptureKeyMatches(
			0x1000, 8, firstToken, 0x1000, 7, firstToken),
		"committed geometry product requires exact view/count/capture token");
	Check(RtPathTraceCommittedCaptureTelemetryDue(1) &&
		!RtPathTraceCommittedCaptureTelemetryDue(2) &&
		RtPathTraceCommittedCaptureTelemetryDue(120) &&
		RtPathTraceCommittedCaptureTelemetryDue(240) &&
		!RtPathTraceCommittedCaptureTelemetryDue(UINT64_MAX),
		"committed geometry telemetry is due at first token and bounded modulo only");
	tokenCounter = UINT64_MAX;
	Check(RtPathTraceCommittedCaptureNextToken(tokenCounter) == 1,
		"committed geometry capture token skips zero after wrap");

	RtPathTraceCommittedGeometrySurface surfaces[2];
	surfaces[0].ordinal = 0;
	surfaces[0].source = RtPathTraceCommittedGeometrySource::CpuTriArrays;
	surfaces[0].complete = true;
	surfaces[1].ordinal = 1;
	surfaces[1].source = RtPathTraceCommittedGeometrySource::CpuTriArrays;
	surfaces[1].complete = true;
	surfaces[0].vertexOffset = 1;
	surfaces[0].vertexCount = 2;
	surfaces[0].indexOffset = 2;
	surfaces[0].indexCount = 3;
	Check(RtPathTraceCommittedCaptureSurfaceShapeValid(surfaces[0], 0, 3, 5),
		"committed geometry surface shape accepts the exact CPU source and ranges");
	RtPathTraceCommittedGeometrySurface corrupted = surfaces[0];
	corrupted.source = static_cast<RtPathTraceCommittedGeometrySource>(0xff);
	Check(!RtPathTraceCommittedCaptureSurfaceShapeValid(corrupted, 0, 3, 5),
		"committed geometry shape rejects source corruption");
	corrupted = surfaces[0];
	corrupted.ordinal = 1;
	Check(!RtPathTraceCommittedCaptureSurfaceShapeValid(corrupted, 0, 3, 5),
		"committed geometry shape rejects ordinal corruption");
	corrupted = surfaces[0];
	corrupted.vertexCount = 3;
	Check(!RtPathTraceCommittedCaptureSurfaceShapeValid(corrupted, 0, 3, 5),
		"committed geometry shape rejects vertex-range corruption");
	corrupted = surfaces[0];
	corrupted.indexOffset = 6;
	Check(!RtPathTraceCommittedCaptureSurfaceShapeValid(corrupted, 0, 3, 5),
		"committed geometry shape rejects index-range corruption");

	RtPathTraceCommittedCaptureTelemetry telemetry;
	telemetry.observedViews = 4;
	telemetry.queuedViews = 2;
	telemetry.completeViews = 1;
	telemetry.fallbackViews = 3;
	telemetry.fallbackPreflight = 1;
	telemetry.fallbackNotAccepting = 1;
	telemetry.fallbackIncomplete = 1;
	RtPathTraceCommittedCaptureRecordCompare(
		telemetry, RtPathTraceCommittedCaptureCompareTerminal::SourceExcluded);
	RtPathTraceCommittedCaptureRecordCompare(
		telemetry, RtPathTraceCommittedCaptureCompareTerminal::Exact);
	RtPathTraceCommittedCaptureRecordCompare(
		telemetry,
		RtPathTraceCommittedCaptureCompareTerminal::Mismatch,
		false, true, true, false);
	Check(RtPathTraceCommittedCaptureViewTelemetryReconciles(telemetry) &&
		RtPathTraceCommittedCaptureCompareTelemetryReconciles(telemetry) &&
		telemetry.compareCalls == 3 && telemetry.compareSourceExcluded == 1 &&
		telemetry.compareExact == 1 && telemetry.compareMismatch == 1 &&
		telemetry.compareVertexMismatch == 1 &&
		telemetry.compareIndexMismatch == 1,
		"committed geometry telemetry reconciles exclusive view and compare terminals");
	RtPathTraceCommittedCaptureTelemetry invalidTelemetry = telemetry;
	++invalidTelemetry.fallbackCopy;
	Check(!RtPathTraceCommittedCaptureViewTelemetryReconciles(invalidTelemetry),
		"committed geometry telemetry rejects invalid fallback reconciliation");
	invalidTelemetry = telemetry;
	++invalidTelemetry.compareExact;
	Check(!RtPathTraceCommittedCaptureCompareTelemetryReconciles(invalidTelemetry),
		"committed geometry telemetry rejects invalid compare reconciliation");

	const fs::path committedFromHarness =
		fs::path(__FILE__).parent_path() / ".." / "renderer" / "NVRHI" /
		"PathTraceCommittedCapture.cpp";
	const fs::path committedAbsolute(
		"E:/prog/rbdoom-3-bfg-rt-producer/neo/renderer/NVRHI/PathTraceCommittedCapture.cpp");
	std::string committedSource;
	for (const fs::path& candidate : { committedFromHarness, committedAbsolute })
	{
		std::ifstream input(candidate);
		if (input)
		{
			committedSource.assign(std::istreambuf_iterator<char>(input),
				std::istreambuf_iterator<char>());
			break;
		}
	}
	Check(committedSource.find("Submit(nullptr, 1)") != std::string::npos &&
		committedSource.find("Submit(nullptr, 0)") == std::string::npos,
		"committed geometry lane dispatches on one JLProc worker, not synchronously");
	Check(committedSource.find("TryCopyCommittedCaptureMemory(\n                ownedVertices") != std::string::npos &&
		committedSource.find("TryCopyCommittedCaptureMemory(\n                ownedIndexes") != std::string::npos &&
		committedSource.find("TryCopyCommittedCaptureMemory(\n                    ownedJoints") != std::string::npos,
		"committed geometry lane guards all raw vertex/index/joint snapshots");

	const fs::path drawSurfFromHarness =
		fs::path(__FILE__).parent_path() / ".." / "renderer" / "NVRHI" /
		"PathTraceDrawSurfCapture.cpp";
	std::string drawSurfSource;
	std::ifstream drawSurfInput(drawSurfFromHarness);
	if (drawSurfInput)
	{
		drawSurfSource.assign(std::istreambuf_iterator<char>(drawSurfInput),
			std::istreambuf_iterator<char>());
	}
	const char* const identityScopeNames[] = {
		"PT Dynamic Identity Scalar Derive",
		"PT Dynamic Identity Static Membership",
		"PT Dynamic Identity Cache Record",
		"PT Dynamic Identity Instance Observation",
		"PT Dynamic Identity Static Decision",
		"PT Dynamic Identity Rigid Route Probe"
	};
	for (const char* scopeName : identityScopeNames)
	{
		const size_t first = drawSurfSource.find(scopeName);
		Check(first != std::string::npos &&
			drawSurfSource.find(scopeName, first + 1) == std::string::npos,
			"C1-M0 Route Identity retains each direct attribution child exactly once");
	}
}


void TestUniversePlanningSnapshotContract()
{
	RtPathTracePlanningSnapshotEpoch a;
	a.generation = 7;
	a.frameIndex = 42;
	a.mapTimeStamp = 11;
	a.mapLoadSerial = 3;
	RtPathTracePlanningCopyName(a.mapName, sizeof(a.mapName), "mars_city1");
	a.capturedAfterBeginFrame = true;
	a.capturedAfterStaticPreload = true;
	a.capturedBeforeSerialMutate = true;
	RtPathTracePlanningSnapshotEpoch b = a;
	struct PlanningHeaderFixture
	{
		RtPathTracePlanningSnapshotEpoch epoch;
		bool complete = false;
	};
	PlanningHeaderFixture left{a, true};
	PlanningHeaderFixture right{a, true};
	Check(RtPathTracePlanningSnapshotsCoherent(left, right),
		"universe planning snapshot headers accept a coherent pair");
	right.epoch.frameIndex++;
	Check(!RtPathTracePlanningSnapshotsCoherent(left, right),
		"universe planning snapshot headers reject mixed frames");
	Check(RtPathTracePlanningEpochsMatch(a, b),
		"universe planning epochs accept one coherent generation");
	b.generation++;
	Check(!RtPathTracePlanningEpochsMatch(a, b),
		"universe planning epochs reject mixed generation");
	b = a;
	b.mapLoadSerial++;
	Check(!RtPathTracePlanningEpochsMatch(a, b),
		"universe planning epochs reject mixed map identity");
	RtPathTraceInvalidatePlanningEpoch(b);
	Check(!RtPathTracePlanningEpochValid(b),
		"universe planning reset/shutdown invalidates epoch");

	std::size_t bytes = 0;
	Check(RtPathTracePlanningAccumulateBytes(1024, bytes) && bytes == 1024,
		"universe planning bytes share future product slot");
	Check(!RtPathTracePlanningAccumulateBytes(
		RT_PT_CAPTURE_PRODUCT_SLOT_MAX_BYTES, bytes),
		"universe planning enforces 16 MiB per-slot cap");
	bytes = 0;
	Check(!RtPathTracePlanningAccumulateArrayBytes(
		RT_PT_CAPTURE_PRODUCT_SLOT_MAX_BYTES, 2, bytes) && bytes == 0,
		"universe planning rejects array-byte multiplication beyond slot cap");
	Check(RT_PT_CAPTURE_PRODUCT_LANE_A_RING_MAX_BYTES ==
		3 * RT_PT_CAPTURE_PRODUCT_SLOT_MAX_BYTES,
		"universe planning pins three-slot 48 MiB maximum");

	RtPathTraceInstanceUniverseSnapshot instanceSnapshot;
	instanceSnapshot.epoch = a;
	instanceSnapshot.complete = true;
	RtPathTraceInstanceHistoryPod history;
	history.instanceId = 9;
	history.lastSeenFrame = 41;
	for (int element = 0; element < 16; ++element)
	{
		history.lastObjectToWorld[element] = static_cast<float>(element + 1);
	}
	instanceSnapshot.histories.push_back(history);
	float currentObjectToWorld[16] = {};
	const RtPathTraceInstanceHistoryApplication consecutive =
		ApplyInstanceHistoryFromPod(instanceSnapshot, 9, currentObjectToWorld, 42);
	Check(consecutive.found && consecutive.hasPreviousObjectToWorld &&
		consecutive.transformContinuous &&
		consecutive.previousObjectToWorld[15] == 16.0f,
		"production history kernel applies a consecutive prior transform");
	instanceSnapshot.histories[0].lastSeenFrame = 40;
	const RtPathTraceInstanceHistoryApplication nonconsecutive =
		ApplyInstanceHistoryFromPod(instanceSnapshot, 9, currentObjectToWorld, 42);
	Check(nonconsecutive.found && !nonconsecutive.hasPreviousObjectToWorld &&
		!nonconsecutive.transformContinuous,
		"production history kernel rejects nonconsecutive motion continuity");
	Check(!ApplyInstanceHistoryFromPod(
		instanceSnapshot, 7, currentObjectToWorld, 42).found,
		"production full-history kernel rejects a missing instance");
	instanceSnapshot.histories.push_back(instanceSnapshot.histories[0]);
	Check(!ApplyInstanceHistoryFromPod(
		instanceSnapshot, 9, currentObjectToWorld, 42).found,
		"production history kernel fails closed on duplicate history rows");
	instanceSnapshot.historyRows = instanceSnapshot.histories.size();
	instanceSnapshot.historyBytes = instanceSnapshot.histories.capacity() *
		sizeof(RtPathTraceInstanceHistoryPod);
	Check(instanceSnapshot.historyRows == 2 && instanceSnapshot.historyBytes != 0,
		"full instance-history snapshot accounts every owned row in the shared slot");

	RtSmokeGeometryUniverseSnapshot geometrySnapshot;
	geometrySnapshot.epoch = a;
	geometrySnapshot.complete = true;
	RtSmokeStaticSurfacePod staticValid;
	staticValid.valid = true;
	staticValid.key = 11;
	RtSmokeStaticSurfacePod staticInvalid;
	staticInvalid.valid = false;
	staticInvalid.key = 19;
	geometrySnapshot.staticSurfaces = {staticValid, staticInvalid};
	Check(HasStaticSurfaceFromPod(geometrySnapshot, 11) &&
		!HasStaticSurfaceFromPod(geometrySnapshot, 19) &&
		!HasStaticSurfaceFromPod(geometrySnapshot, 20),
		"production static kernel preserves valid/invalid/missing membership");
	geometrySnapshot.staticSurfaces.insert(
		geometrySnapshot.staticSurfaces.begin() + 1, staticValid);
	Check(!HasStaticSurfaceFromPod(geometrySnapshot, 11),
		"production static kernel fails closed on duplicate rows");
	geometrySnapshot.staticSurfaces = {staticValid, staticInvalid};

	const auto readyRoute = [](std::uint64_t meshHash)
	{
		RtSmokeRigidRouteReadyPod route;
		route.valid = true;
		route.meshHash = meshHash;
		route.vertexBufferIdentity = 101;
		route.indexBufferIdentity = 202;
		route.materialId = 303;
		route.sourceRange.vertices.count = 4;
		route.sourceRange.indexes.count = 6;
		route.sourceRange.triangles.count = 2;
		route.cachedRouteDataValid = true;
		route.localBoundsValid = true;
		route.cpuMeshContentSignature = 404;
		route.cachedVertexCount = 4;
		route.cachedIndexCount = 6;
		route.gpuBlasVertexCount = 4;
		route.gpuBlasIndexCount = 6;
		route.hasRigidVertexBuffer = true;
		route.hasRigidIndexBuffer = true;
		route.hasRigidBlas = true;
		route.gpuBuffersUploaded = true;
		route.gpuBlasCreated = true;
		route.gpuBlasBuildSubmitted = true;
		route.gpuUploadSignature = RtPathTraceRigidUploadSignatureFromPod(route);
		return route;
	};
	RtSmokeRigidRouteReadyPod routeA = readyRoute(1001);
	RtSmokeRigidRouteReadyPod routeB = readyRoute(2002);
	geometrySnapshot.rigidRoutes = {routeA, routeB};
	Check(IsRigidRouteReadyFromPod(geometrySnapshot, 1001) &&
		!IsRigidRouteReadyFromPod(geometrySnapshot, 9999),
		"production rigid kernel joins the exact mesh hash");
	geometrySnapshot.rigidRoutes[0].valid = false;
	Check(!IsRigidRouteReadyFromPod(geometrySnapshot, 1001),
		"production rigid kernel rejects an invalid route row");
	geometrySnapshot.rigidRoutes[0] = routeA;
	RtSmokeRigidRouteRawKeyPod rawKey;
	rawKey.vertexBufferIdentity = routeA.vertexBufferIdentity;
	rawKey.indexBufferIdentity = routeA.indexBufferIdentity;
	rawKey.vertexCount = routeA.sourceRange.vertices.count;
	rawKey.indexCount = routeA.sourceRange.indexes.count;
	rawKey.triangleCount = routeA.sourceRange.triangles.count;
	rawKey.materialId = routeA.materialId;
	Check(IsRigidRouteReadyFromPod(geometrySnapshot, rawKey),
		"production raw rigid join accepts differing class-signature hashes for one raw tuple");
	const auto rawMismatchRejected = [&](int field)
	{
		RtSmokeRigidRouteRawKeyPod changed = rawKey;
		switch (field)
		{
		case 0: ++changed.vertexBufferIdentity; break;
		case 1: ++changed.indexBufferIdentity; break;
		case 2: ++changed.vertexCount; break;
		case 3: changed.indexCount += 3; break;
		case 4: ++changed.triangleCount; break;
		default: ++changed.materialId; break;
		}
		return !IsRigidRouteReadyFromPod(geometrySnapshot, changed);
	};
	Check(rawMismatchRejected(0) && rawMismatchRejected(1) &&
		rawMismatchRejected(2) && rawMismatchRejected(3) &&
		rawMismatchRejected(4) && rawMismatchRejected(5),
		"production raw rigid join rejects every mismatched key field");
	geometrySnapshot.rigidResidents.push_back({77, 1001, 2, 4, 303, 41});
	Check(IsRigidRouteResidentReadyFromPod(geometrySnapshot, 2, 4, 303) &&
		!IsRigidRouteResidentReadyFromPod(geometrySnapshot, 2, 4, 304),
		"production resident kernel joins exact entity/material to ready route");
	geometrySnapshot.rigidRoutes.insert(
		geometrySnapshot.rigidRoutes.begin() + 1, routeA);
	Check(!IsRigidRouteReadyFromPod(geometrySnapshot, 1001) &&
		!IsRigidRouteResidentReadyFromPod(geometrySnapshot, 2, 4, 303),
		"production route and resident kernels fail closed on duplicate mesh rows");
	geometrySnapshot.rigidRoutes = {routeA, routeB};
	RtPathTraceInvalidatePlanningEpoch(geometrySnapshot.epoch);
	Check(!HasStaticSurfaceFromPod(geometrySnapshot, 11) &&
		!IsRigidRouteReadyFromPod(geometrySnapshot, 1001) &&
		!IsRigidRouteResidentReadyFromPod(geometrySnapshot, 2, 4, 303),
		"production geometry kernels reject reset/invalidated epoch state");
	instanceSnapshot.epoch = a;
	instanceSnapshot.complete = true;
	instanceSnapshot.ownerGeneration = 17;
	geometrySnapshot.epoch = a;
	geometrySnapshot.complete = true;
	geometrySnapshot.ownerGeneration = 29;
	Check(RtPathTracePlanningSnapshotsCoherent(
		instanceSnapshot, geometrySnapshot),
		"shared product generation matches independently revised universe owners");

	RtSmokeGeometryUniverseSnapshot transactionOutput;
	transactionOutput.epoch = a;
	transactionOutput.complete = true;
	transactionOutput.staticSurfaces.reserve(8);
	const std::size_t retainedBeforeFailure = transactionOutput.OwnedBytes();
	std::size_t nearlyFullSlot = RT_PT_CAPTURE_PRODUCT_SLOT_MAX_BYTES - 1;
	const std::size_t unchangedNearlyFullSlot = nearlyFullSlot;
	bool candidateAllocated = false;
	const bool charged = RtPathTraceBuildPlanningSnapshotTransaction(
		transactionOutput, nearlyFullSlot,
		[&](RtSmokeGeometryUniverseSnapshot& candidate)
		{
			candidateAllocated = true;
			candidate.epoch = a;
			candidate.staticSurfaces.reserve(4);
			candidate.staticSurfaces.push_back(staticValid);
			candidate.complete = true;
			return true;
		});
	Check(!charged && candidateAllocated &&
		nearlyFullSlot == unchangedNearlyFullSlot &&
		!transactionOutput.complete && transactionOutput.epoch.generation == 0 &&
		transactionOutput.OwnedBytes() == sizeof(transactionOutput) &&
		retainedBeforeFailure > transactionOutput.OwnedBytes(),
		"transaction releases failed actual-capacity backing and preserves prior shared charge");
#if defined(__cpp_exceptions) || defined(_CPPUNWIND)
	std::size_t exceptionSlotBytes = 0;
	const bool badAllocAccepted = RtPathTraceBuildPlanningSnapshotTransaction(
		transactionOutput, exceptionSlotBytes,
		[](RtSmokeGeometryUniverseSnapshot& candidate) -> bool
		{
			candidate.staticSurfaces.reserve(2);
			throw std::bad_alloc();
		});
	Check(!badAllocAccepted && exceptionSlotBytes == 0 &&
		transactionOutput.OwnedBytes() == sizeof(transactionOutput),
		"transaction catches allocation failure and releases unpublished backing");
	const bool lengthAccepted = RtPathTraceBuildPlanningSnapshotTransaction(
		transactionOutput, exceptionSlotBytes,
		[](RtSmokeGeometryUniverseSnapshot&) -> bool
		{
			throw std::length_error("forced planning snapshot length failure");
		});
	Check(!lengthAccepted && exceptionSlotBytes == 0 &&
		transactionOutput.OwnedBytes() == sizeof(transactionOutput),
		"transaction catches length failure and leaves no hidden allowance");
#endif
	std::size_t reuseSlotBytes = 0;
	const bool reused = RtPathTraceBuildPlanningSnapshotTransaction(
		transactionOutput, reuseSlotBytes,
		[&](RtSmokeGeometryUniverseSnapshot& candidate)
		{
			candidate.epoch = a;
			candidate.staticSurfaces.push_back(staticValid);
			candidate.complete = true;
			return true;
		});
	Check(reused && transactionOutput.complete && reuseSlotBytes ==
		transactionOutput.OwnedBytes(),
		"transaction can reuse output only after a complete capacity-accounted commit");
	transactionOutput.ResetAndRelease();
	Check(!transactionOutput.complete && transactionOutput.epoch.generation == 0 &&
		transactionOutput.OwnedBytes() == sizeof(transactionOutput),
		"planning lifecycle reset releases owned backing and invalidates identity");

	const fs::path root = fs::path(__FILE__).parent_path() / ".." /
		"renderer" / "NVRHI";
	std::ifstream instanceInput(root / "PathTraceInstanceUniverse.h");
	std::ifstream geometryInput(root / "PathTraceGeometryUniverse.h");
	std::string instanceSource{
		std::istreambuf_iterator<char>(instanceInput),
		std::istreambuf_iterator<char>()};
	std::string geometrySource{
		std::istreambuf_iterator<char>(geometryInput),
		std::istreambuf_iterator<char>()};
	Check(instanceSource.find("CaptureInstanceUniverseSnapshot") != std::string::npos &&
		instanceSource.find("captureProductSlotBytes") != std::string::npos,
		"instance universe exposes full history snapshot and shared pure lookup");
	Check(geometrySource.find("CaptureGeometryUniversePlanningSnapshot") != std::string::npos &&
		geometrySource.find("captureProductSlotBytes") != std::string::npos,
		"geometry universe exposes full static/rigid planning lookups");
	std::ifstream geometryCppInput(root / "PathTraceGeometryUniverse.cpp");
	std::ifstream instanceCppInput(root / "PathTraceInstanceUniverse.cpp");
	const std::string geometryCpp{
		std::istreambuf_iterator<char>(geometryCppInput),
		std::istreambuf_iterator<char>()};
	const std::string instanceCpp{
		std::istreambuf_iterator<char>(instanceCppInput),
		std::istreambuf_iterator<char>()};
	Check(geometryCpp.find("RtPathTraceBuildPlanningSnapshotTransaction") != std::string::npos &&
		geometryCpp.find("RtPathTraceRigidRouteReadyRecordFromPod") != std::string::npos &&
		instanceCpp.find("RtPathTraceBuildPlanningSnapshotTransaction") != std::string::npos,
		"live capture wrappers call the production-shared transaction and parity kernels");
	std::ifstream planningInput(root / "PathTraceUniversePlanningSnapshot.h");
	const std::string planningSource{
		std::istreambuf_iterator<char>(planningInput),
		std::istreambuf_iterator<char>()};
	const size_t geometryPodBegin = planningSource.find("struct RtSmokeStaticSurfacePod");
	const size_t geometryPodEnd = planningSource.find(
		"static_assert(std::is_trivially_copyable<RtPathTraceInstanceMeshRecordPod>");
	const std::string geometryPods =
		geometryPodBegin != std::string::npos &&
		geometryPodEnd != std::string::npos && geometryPodEnd > geometryPodBegin
			? planningSource.substr(geometryPodBegin, geometryPodEnd - geometryPodBegin)
			: std::string();
	Check(!geometryPods.empty() &&
		geometryPods.find("nvrhi::") == std::string::npos &&
		geometryPods.find("idMaterial*") == std::string::npos &&
		geometryPods.find("srfTriangles_t*") == std::string::npos,
		"geometry planning POD declarations contain no live or GPU handle fields");
	const size_t instancePodBegin = planningSource.find(
		"struct RtPathTraceInstanceMeshRecordPod");
	const size_t instancePodEnd = planningSource.find(
		"struct RtSmokeStaticSurfacePod");
	const std::string instancePods =
		instancePodBegin != std::string::npos &&
		instancePodEnd != std::string::npos && instancePodEnd > instancePodBegin
			? planningSource.substr(instancePodBegin, instancePodEnd - instancePodBegin)
			: std::string();
	Check(!instancePods.empty() &&
		instancePods.find("idMaterial*") == std::string::npos &&
		instancePods.find("srfTriangles_t*") == std::string::npos,
		"instance planning POD declarations contain no live pointer fields");
}

void TestProducerLaneContract()
{
	Check(RtPathTraceProducerModeImplemented(0) &&
		RtPathTraceProducerModeImplemented(1) &&
		RtPathTraceProducerModeImplemented(2) &&
		!RtPathTraceProducerModeImplemented(3),
		"producer lane mode ladder implements bounded A-only mode 2");
	Check(!RtPathTraceProducerConfigurationChanged(1, 1, 1, 1) &&
		RtPathTraceProducerConfigurationChanged(0, 1, 1, 1) &&
		RtPathTraceProducerConfigurationChanged(1, 1, 1, 3),
		"producer configuration change predicate tracks mode or normalized mask");
	Check(RT_PT_PRODUCER_LANE_COUNT == 3 &&
		RT_PT_PRODUCER_LANE_A_SLOT_COUNT == 3,
		"producer host pins exactly three lanes and triple Lane A slots");
	{
		// Dependency-light executable model of the production staging aggregate.
		// Every fallible callback sees only candidate state; live state changes
		// through the same shared transaction helper plus one noexcept commit.
		struct OwnerFrameProbe
		{
			std::vector<int> vertices, indexes, triangleClasses,
				triangleMaterials, triangleInstances, triangleIdentities;
			std::array<int, 4> bucketRanges = {};
			std::array<int, 3> sourceCounts = {};
			std::array<int, 4> stats = {};
			std::vector<int> materialIds;
			std::string materialName;
			int captureTiming = 0;
			std::vector<std::uint64_t> rigidWalked;
			std::vector<std::uint32_t> rigidWalkedTriangles;
			std::vector<int> mergedWalkedRanges;

			void CommitTo(OwnerFrameProbe& live) noexcept
			{
				using std::swap;
				swap(*this, live);
			}

			bool operator==(const OwnerFrameProbe& rhs) const
			{
				return vertices == rhs.vertices && indexes == rhs.indexes &&
					triangleClasses == rhs.triangleClasses &&
					triangleMaterials == rhs.triangleMaterials &&
					triangleInstances == rhs.triangleInstances &&
					triangleIdentities == rhs.triangleIdentities &&
					bucketRanges == rhs.bucketRanges &&
					sourceCounts == rhs.sourceCounts && stats == rhs.stats &&
					materialIds == rhs.materialIds &&
					materialName == rhs.materialName &&
					captureTiming == rhs.captureTiming &&
					rigidWalked == rhs.rigidWalked &&
					rigidWalkedTriangles == rhs.rigidWalkedTriangles &&
					mergedWalkedRanges == rhs.mergedWalkedRanges;
			}
		};
		static_assert(std::is_nothrow_swappable<OwnerFrameProbe>::value,
			"owner transaction probe commit must be non-throwing");
		auto seed = []()
		{
			OwnerFrameProbe value;
			value.vertices = { 1 };
			value.indexes = { 2 };
			value.triangleClasses = { 3 };
			value.triangleMaterials = { 4 };
			value.triangleInstances = { 5 };
			value.triangleIdentities = { 6 };
			value.bucketRanges = { 7, 8, 9, 10 };
			value.sourceCounts = { 11, 12, 13 };
			value.stats = { 14, 15, 16, 17 };
			value.materialIds = { 18 };
			value.materialName = "before";
			value.captureTiming = 19;
			value.rigidWalked = { 20 };
			value.rigidWalkedTriangles = { 21 };
			value.mergedWalkedRanges = { 22 };
			return value;
		};
		auto applyGeometry = [](OwnerFrameProbe& value)
		{
			value.vertices.push_back(101);
			value.indexes.push_back(102);
			value.triangleClasses.push_back(103);
			value.triangleMaterials.push_back(104);
			value.triangleInstances.push_back(105);
			value.triangleIdentities.push_back(106);
			value.bucketRanges[0] = 107;
			value.sourceCounts[0] = 108;
			return true;
		};
		auto finalizeOwner = [](OwnerFrameProbe& value)
		{
			value.stats = { 201, 202, 203, 204 };
			value.materialIds.push_back(205);
			value.materialName = "staged-finalize";
			value.captureTiming = 206;
			value.rigidWalked.push_back(207);
			value.rigidWalkedTriangles.push_back(208);
			value.mergedWalkedRanges.push_back(209);
			return true;
		};
		auto appendHarvestedFallback = [](OwnerFrameProbe& value)
		{
			value.vertices.push_back(301);
			value.indexes.push_back(302);
			value.stats[0] += 1;
			value.rigidWalked.push_back(303);
		};

		for (RtPathTraceOwnerFrameFailurePoint seam : {
				RtPathTraceOwnerFrameFailurePoint::AfterGeometry,
				RtPathTraceOwnerFrameFailurePoint::AfterFinalize })
		{
			OwnerFrameProbe live = seed();
			const OwnerFrameProbe before = live;
			OwnerFrameProbe candidate = live;
			const bool built = BuildPathTraceOwnerFrameStagedTransaction(
				candidate, applyGeometry, finalizeOwner, seam);
			Check(!built && live == before,
				seam == RtPathTraceOwnerFrameFailurePoint::AfterGeometry
					? "failure after staged geometry leaves every live owner output untouched"
					: "failure after staged finalize leaves every live owner output untouched");
			OwnerFrameProbe fallbackOracle = before;
			appendHarvestedFallback(fallbackOracle);
			appendHarvestedFallback(live);
			Check(live == fallbackOracle,
				"discarded staging falls back once from the pristine owner frame");
		}

		OwnerFrameProbe live = seed();
		OwnerFrameProbe candidate = live;
		Check(BuildPathTraceOwnerFrameStagedTransaction(candidate,
				applyGeometry, finalizeOwner) && !(live == candidate),
			"happy owner transaction builds entirely off to the side");
		const OwnerFrameProbe expected = candidate;
		candidate.CommitTo(live);
		Check(live == expected,
			"happy owner transaction commits geometry, stats, strings, timing and rows once");
	}
	RtPathTraceProducerSlotContract slot;
	Check(RtPathTraceProducerMayDispatch(slot, 7),
		"free producer slot accepts a nonzero generation");
	slot.generation = 7;
	slot.state = RtPathTraceProducerSlotState::Queued;
	Check(!RtPathTraceProducerMayDispatch(slot, 8) &&
		RtPathTraceProducerGenerationMatches(slot, 7) &&
		!RtPathTraceProducerGenerationMatches(slot, 8),
		"queued producer slot rejects reuse and pins its generation");
	slot.state = RtPathTraceProducerSlotState::Ready;
	Check(!RtPathTraceProducerReadyToCompare(slot),
		"late product cannot compare without the actual serial oracle");
	slot.oracleReady = true;
	Check(RtPathTraceProducerReadyToCompare(slot),
		"matching product waits only for nonblocking oracle publication");
	Check(RtPathTraceProducerSlotBytesFit(12, 4, 16) &&
		!RtPathTraceProducerSlotBytesFit(12, 5, 16) &&
		!RtPathTraceProducerSlotBytesFit(17, 0, 16),
		"producer product shares one checked per-slot cap with its snapshot");
	{
		RtPathTraceCompleteSlotCardinality counts;
		counts.surfaces = 4;
		counts.stages = 7;
		counts.registers = 11;
		counts.vertices = 9;
		counts.indexes = 12;
		counts.joints = 3;
		counts.variantBases = 2;
		counts.registryMaterials = 5;
		counts.modelTables = 2;
		counts.modelSurfaceTokens = 9;
		counts.applyGateKeys = 2;
		counts.instanceMeshes = 3;
		counts.instanceHistories = 4;
		counts.geometryStatic = 5;
		counts.geometryRoutes = 6;
		counts.geometryResidents = 7;
		counts.receiptSurfaces = 4;
		counts.materialInfoIntents = 4;
		counts.materialVariants = 4;
		counts.instanceObservations = 4;
		counts.rigidCandidates = 4;
		RtPathTraceCompleteSlotLayout layout;
		layout.snapshotFixed = 100;
		layout.rawSurface = 3;
		layout.ownerDecision = 4;
		layout.stage = 5;
		layout.registerValue = 7;
		layout.rawVertex = 11;
		layout.rawIndex = 13;
		layout.joint = 17;
		layout.variantBase = 19;
		layout.registryMaterial = 23;
		layout.modelTable = 27;
		layout.modelSurfaceToken = 8;
		layout.applyGateKey = 29;
		layout.instanceMesh = 31;
		layout.instanceHistory = 37;
		layout.geometryStatic = 41;
		layout.geometryRoute = 43;
		layout.geometryResident = 47;
		layout.productFixed = 53;
		layout.productSurface = 59;
		layout.productVertex = 61;
		layout.productIndex = 67;
		layout.triangleClass = 71;
		layout.triangleMaterial = 73;
		layout.triangleInstance = 79;
		layout.triangleIdentity = 83;
		layout.materialIntent = 89;
		layout.materialVariant = 97;
		layout.instanceObservation = 101;
		layout.rigidCandidate = 103;
		layout.receiptSurface = 105;
		layout.membershipOrdinal = 107;
		layout.oracleFixed = 109;
		layout.oracleSurface = 113;
		RtPathTraceCompleteSlotPlan exactPlan;
		Check(RtPathTracePlanCompleteSlot(counts, layout,
			RT_PT_CAPTURE_PRODUCT_SLOT_MAX_BYTES, exactPlan) &&
			exactPlan.snapshotBytes > layout.snapshotFixed &&
			exactPlan.finalProductBytes == 0 &&
			exactPlan.peakBytes == exactPlan.snapshotBytes +
				exactPlan.candidateBytes + exactPlan.oracleBytes,
			"shared complete-slot authority charges one final product plus snapshot/oracle cardinality");
		RtPathTraceCompleteSlotPlan underPlan;
		RtPathTraceCompleteSlotLayout boundary = layout;
		boundary.snapshotFixed = RT_PT_CAPTURE_PRODUCT_SLOT_MAX_BYTES - 1;
		RtPathTraceCompleteSlotCardinality emptyCounts;
		boundary.productFixed = boundary.oracleFixed = 0;
		Check(RtPathTracePlanCompleteSlot(emptyCounts, boundary,
				RT_PT_CAPTURE_PRODUCT_SLOT_MAX_BYTES, underPlan) &&
			underPlan.peakBytes == RT_PT_CAPTURE_PRODUCT_SLOT_MAX_BYTES - 1,
			"allocation-free preflight accepts a just-under complete slot");
		boundary.snapshotFixed = RT_PT_CAPTURE_PRODUCT_SLOT_MAX_BYTES + 1;
		Check(!RtPathTracePlanCompleteSlot(emptyCounts, boundary,
				RT_PT_CAPTURE_PRODUCT_SLOT_MAX_BYTES, underPlan),
			"allocation-free preflight rejects over-cap before reserve");
		std::vector<std::uint8_t> actualCapacity;
		actualCapacity.reserve(257);
		Check(RtPathTraceReconcileReservedCapacity(100,
				actualCapacity.capacity(), 200, 1000) &&
			!RtPathTraceReconcileReservedCapacity(800,
				actualCapacity.capacity(), 200, 1000),
			"shared reserve authority reconciles allocator capacity before the next allocation");

		const int buckets[] = { 2, 0, 2, 1 };
		std::vector<int> order;
		for (int bucket = 0; bucket < 3; ++bucket)
		{
			for (int ordinal = 0; ordinal < 4; ++ordinal)
			{
				if (RtPathTraceProducerPartitionSelects(
						bucket, buckets[ordinal], true))
				{
					order.push_back(ordinal);
				}
			}
		}
		Check(order == std::vector<int>({ 1, 3, 0, 2 }),
			"one-candidate partition preserves bucket then source order deterministically");
		const std::uint32_t candidateIndexes[] = { 10, 11, 12, 20, 21, 22 };
		const std::uint32_t sourceVertexOffsets[] = { 10, 20 };
		const std::uint32_t finalVertexOffsets[] = { 0, 3 };
		std::vector<std::uint32_t> rebased;
		for (std::size_t surface = 0; surface < 2; ++surface)
		{
			for (std::size_t local = 0; local < 3; ++local)
			{
				std::uint32_t destination = UINT32_MAX;
				Check(RtPathTraceProducerRebaseIndex(finalVertexOffsets[surface],
					candidateIndexes[surface * 3 + local],
					sourceVertexOffsets[surface], destination),
					"shared final partition rebase accepts an in-range source index");
				rebased.push_back(destination);
			}
		}
		Check(rebased == std::vector<std::uint32_t>({ 0, 1, 2, 3, 4, 5 }),
			"deterministic final partition rebases indexes with metadata-aligned surface order");
		std::uint32_t rejectedRebase = 0;
		Check(!RtPathTraceProducerRebaseIndex(0, 9, 10, rejectedRebase),
			"shared final partition rebase rejects an index before its source span");
		std::size_t charged = 12;
		Check(RtPathTraceProducerCheckedArrayBytes(1, 4, charged, 16) &&
			charged == 16 &&
			!RtPathTraceProducerCheckedArrayBytes(1, 1, charged, 16),
			"capacity charge rejects overflow before reserve or copy");
	}
	{
		struct HandoffCounters
		{
			int ownerMutations = 0;
			int dispatches = 0;
			int harvestAppends = 0;
			int fullCaptures = 0;
		};
		const auto exerciseFailureBoundary = [](
			bool harvestSucceeded, bool attachmentSucceeded)
		{
			HandoffCounters counters;
			if (harvestSucceeded)
			{
				++counters.ownerMutations;
			}
			const RtPathTraceOwnerDecisionHandoffState state = {
				harvestSucceeded, attachmentSucceeded
			};
			if (RtPathTraceOwnerDecisionHandoffCanDispatch(state))
			{
				++counters.dispatches;
			}
			else if (RtPathTraceOwnerDecisionHandoffUsesHarvestFallback(state))
			{
				++counters.harvestAppends;
			}
			else if (RtPathTraceOwnerDecisionHandoffUsesFullCapture(state))
			{
				++counters.fullCaptures;
				++counters.ownerMutations;
			}
			return counters;
		};
		for (const char* failureFamily : {
			"decision-cardinality attachment failure",
			"membership-receipt attachment failure" })
		{
			const HandoffCounters counters =
				exerciseFailureBoundary(true, false);
			Check(counters.ownerMutations == 1 && counters.dispatches == 0 &&
					counters.harvestAppends == 1 && counters.fullCaptures == 0,
				failureFamily);
		}
		const HandoffCounters harvestFailure =
			exerciseFailureBoundary(false, false);
		Check(harvestFailure.ownerMutations == 1 &&
			harvestFailure.dispatches == 0 &&
			harvestFailure.harvestAppends == 0 &&
			harvestFailure.fullCaptures == 1,
			"genuine Harvest failure retains the full serial capture path exactly once");
	}
	{
		const fs::path scenePath = fs::path(__FILE__).parent_path() / ".." /
			"renderer" / "NVRHI" / "PathTraceSmokeSceneBuild.cpp";
		std::ifstream input(scenePath);
		const std::string source{std::istreambuf_iterator<char>(input),
			std::istreambuf_iterator<char>()};
		const std::size_t predicate = source.find(
			"RtPathTraceProducerConfigurationChanged(");
		const std::string wiring = predicate != std::string::npos
			? source.substr(predicate, 900) : std::string();
		Check(!wiring.empty() &&
			wiring.find("g_pathTraceProducerLaneConfiguredMask") != std::string::npos &&
			wiring.find("producerLaneRequestedMask") != std::string::npos &&
			wiring.find("ConfigurePathTraceProducerLanes(") != std::string::npos &&
			wiring.find("producerLaneMode = PathTraceProducerLaneEffectiveMode()") !=
				std::string::npos &&
			wiring.find("producerLaneShadow = producerLaneRequestedMode") ==
				std::string::npos,
			"production caller routes normalized mode-or-mask changes into Configure");
		const std::size_t snapshotFill = source.find(
			"CapturePathTraceOwnerSnapshot(");
		const std::size_t harvest = source.find(
			"producerOwnerHarvested =", snapshotFill);
		const std::size_t dispatch = source.find(
			"DispatchPathTraceProducerLaneA(", harvest);
		const std::size_t decisionHandoff = source.find(
			"CopyPathTraceOwnerHarvestDecisionsToSnapshot(", harvest);
		const std::size_t attachmentState = source.find(
			"producerOwnerDecisionsAttached =", harvest);
		const std::size_t latestConsume = source.find(
			"TryConsumeLatestPathTraceProducerLaneA(", dispatch);
		const std::size_t fallbackAppend = source.find(
			"AppendPathTraceOwnerHarvestGeometry(", latestConsume);
		Check(snapshotFill != std::string::npos &&
			harvest != std::string::npos && attachmentState != std::string::npos &&
			decisionHandoff != std::string::npos && dispatch != std::string::npos &&
			latestConsume != std::string::npos && fallbackAppend != std::string::npos &&
			snapshotFill < harvest && harvest < attachmentState &&
			attachmentState < decisionHandoff &&
			decisionHandoff < dispatch &&
			dispatch < latestConsume && latestConsume < fallbackAppend,
			"production snapshots raw bytes before Harvest and dispatches only after the authoritative decision handoff");
		Check(source.find(
				"producerOwnerHarvested = producerOwnerHarvested &&") ==
				std::string::npos &&
			source.find("RtPathTraceOwnerDecisionHandoffUsesHarvestFallback(",
				latestConsume) != std::string::npos &&
			source.find("RtPathTraceOwnerDecisionHandoffUsesFullCapture(",
				latestConsume) != std::string::npos,
			"attachment failure preserves Harvest and cannot replay the full owner mutation loop");
		const std::string nonblockingConsume = dispatch != std::string::npos &&
			latestConsume != std::string::npos
			? source.substr(dispatch, latestConsume - dispatch) : std::string();
		Check(!nonblockingConsume.empty() &&
			nonblockingConsume.find("Wait(") == std::string::npos &&
			nonblockingConsume.find("condition.wait") == std::string::npos,
			"production age-1 selection has zero exact-frame waits between dispatch and consume");
		const std::size_t stagedBuild = source.find(
			"BuildPathTraceOwnerFrameStagedTransaction(", latestConsume);
		const std::size_t stagedCommit = source.find(
			"staging.CommitTo(", stagedBuild);
		Check(stagedBuild != std::string::npos &&
			stagedCommit != std::string::npos &&
			stagedBuild < stagedCommit && stagedCommit < fallbackAppend,
			"production mode 2 applies and finalizes only in staging before one commit or pristine fallback");
		const fs::path drawHeaderPath = fs::path(__FILE__).parent_path() / ".." /
			"renderer" / "NVRHI" / "PathTraceDrawSurfCapture.h";
		std::ifstream drawHeaderInput(drawHeaderPath);
		const std::string drawHeader{
			std::istreambuf_iterator<char>(drawHeaderInput),
			std::istreambuf_iterator<char>()};
		const std::size_t stagingStruct = drawHeader.find(
			"struct RtPathTraceOwnerFrameStaging");
		const std::size_t stagingEnd = drawHeader.find(
			"static_assert", stagingStruct);
		const std::string stagingInventory = stagingStruct != std::string::npos &&
			stagingEnd != std::string::npos
			? drawHeader.substr(stagingStruct, stagingEnd - stagingStruct)
			: std::string();
		const char* requiredStagedTargets[] = {
			"vertices;", "indexes;", "triangleClasses;",
			"triangleMaterials;", "triangleInstances;",
			"triangleIdentities;", "bucketRanges;", "sourceSurfaces",
			"sourceVerts", "sourceIndexes", "classStats;", "skipStats;",
			"dynamicStats;", "materialStats;", "captureTiming;",
			"rigidCaptureWalked;", "rigidCaptureWalkedTriangles;",
			"mergedWalkedRanges;" };
		bool completeStagingInventory = !stagingInventory.empty();
		for (const char* target : requiredStagedTargets)
		{
			completeStagingInventory = completeStagingInventory &&
				stagingInventory.find(target) != std::string::npos;
		}
		Check(completeStagingInventory &&
			stagingInventory.find("CommitTo(") != std::string::npos,
			"owner staging inventory binds every Apply and Finalize output to one commit aggregate");
		const fs::path drawPath = fs::path(__FILE__).parent_path() / ".." /
			"renderer" / "NVRHI" / "PathTraceDrawSurfCapture.cpp";
		std::ifstream drawInput(drawPath);
		const std::string drawSource{std::istreambuf_iterator<char>(drawInput),
			std::istreambuf_iterator<char>()};
		const std::size_t appendHelper = drawSource.find(
			"bool AppendPathTraceOwnerHarvestGeometry(");
		const std::size_t eligibleHelper = drawSource.find(
			"bool PathTraceOwnerHarvestProductEligible(", appendHelper);
		const std::string appendBody = appendHelper != std::string::npos &&
			eligibleHelper != std::string::npos
			? drawSource.substr(appendHelper, eligibleHelper - appendHelper)
			: std::string();
		Check(!appendBody.empty() &&
			appendBody.find("AppendSmokeSurfaceGeometry(") != std::string::npos &&
			appendBody.find("RecordObservation(") == std::string::npos &&
			appendBody.find("RecordRigidMeshCandidate(") == std::string::npos &&
			appendBody.find("RecordA8S1RouteObservation(") == std::string::npos,
			"harvested fallback appends geometry without replaying owner mutations");
	}


	auto epoch = [](std::uint64_t generation) {
		RtPathTracePlanningSnapshotEpoch value;
		value.generation = generation;
		value.frameIndex = generation;
		value.mapTimeStamp = 17;
		value.mapLoadSerial = 3;
		RtPathTracePlanningCopyName(value.mapName, sizeof(value.mapName), "r1");
		value.capturedAfterBeginFrame = true;
		value.capturedAfterStaticPreload = true;
		value.capturedBeforeSerialMutate = true;
		return value;
	};
	auto snapshot = [&epoch](std::uint64_t generation, std::uint32_t delayUs = 0,
		std::size_t ownedBytes = 64, std::uint32_t failureKind = 0) {
		RtPathTraceCaptureOwnerSnapshot value;
		value.epoch = epoch(generation);
		value.lateConsumeToken.mapTimeStamp = value.epoch.mapTimeStamp;
		value.lateConsumeToken.mapLoadSerial = value.epoch.mapLoadSerial;
		RtPathTracePlanningCopyName(value.lateConsumeToken.mapName,
			sizeof(value.lateConsumeToken.mapName), value.epoch.mapName);
		value.lateConsumeToken.registryGeneration = 11;
		value.lateConsumeToken.instanceUniverseGeneration = 12;
		value.lateConsumeToken.geometryUniverseGeneration = 13;
		value.lateConsumeToken.configFingerprint = 14;
		value.lateConsumeToken.capturedAfterBeginFrame = true;
		value.lateConsumeToken.capturedAfterStaticPreload = true;
		value.buildMembershipReceipt.hash = 0x12345678ull;
		value.buildMembershipReceipt.surfaceCount = 2;
		value.buildMembershipReceipt.complete = true;
		value.viewIdentity = generation;
		value.complete = true;
		value.ownedBytes = ownedBytes;
		value.buildDelayUs = delayUs;
		value.buildFailureKind = failureKind;
		return value;
	};
	auto oracle = [&epoch](std::uint64_t generation, std::size_t ownedBytes = 64,
		std::uint64_t acceptedSurfaces = 0,
		std::uint64_t acceptedVertices = 0,
		RtPathTraceCaptureLiveCardinalityAttribution liveCardinality = {}) {
		RtPathTraceCaptureOracle value;
		value.epoch = epoch(generation);
		value.ownedBytes = ownedBytes;
		value.complete = true;
		value.exact = true;
		value.acceptedSurfaces = acceptedSurfaces;
		value.acceptedVertices = acceptedVertices;
		value.liveCardinality = liveCardinality;
		return value;
	};
	auto waitCompleted = [](std::uint64_t completed) {
		const auto deadline = std::chrono::steady_clock::now() +
			std::chrono::seconds(2);
		while (PathTraceProducerLanesTelemetry().completed < completed &&
			std::chrono::steady_clock::now() < deadline)
		{
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		return PathTraceProducerLanesTelemetry().completed >= completed;
	};
	auto laneBSnapshot = [&epoch](std::uint64_t generation,
		std::uint32_t delayUs = 0, std::size_t ownedBytes = 64,
		std::uint32_t failureKind = 0) {
		RtPathTraceAccelCpuSnapshot value;
		value.epoch = epoch(generation);
		value.epoch.capturedBeforeSerialMutate = false;
		value.compatibility.mapTimeStamp = value.epoch.mapTimeStamp;
		value.compatibility.mapLoadSerial = value.epoch.mapLoadSerial;
		RtPathTracePlanningCopyName(value.compatibility.mapName,
			sizeof(value.compatibility.mapName), value.epoch.mapName);
		value.compatibility.lifecycleEpoch = 9;
		value.compatibility.configFingerprint = 10;
		value.inputReceipt = 0xabcddcbaull;
		value.ownedBytes = ownedBytes;
		value.buildDelayUs = delayUs;
		value.buildFailureKind = failureKind;
		value.rigidSignature = 101;
		value.accelerationSignature = 202;
		value.staticSignature = 303;
		value.complete = true;
		return value;
	};
	auto laneBOracle = [&epoch](std::uint64_t generation) {
		RtPathTraceAccelCpuOracle value;
		value.epoch = epoch(generation);
		value.epoch.capturedBeforeSerialMutate = false;
		value.inputReceipt = 0xabcddcbaull;
		value.rigidSignature = 101;
		value.accelerationSignature = 202;
		value.staticSignature = 303;
		value.complete = true;
		return value;
	};
	auto waitLaneBCompleted = [](std::uint64_t completed) {
		const auto deadline = std::chrono::steady_clock::now() +
			std::chrono::seconds(2);
		while (PathTraceProducerLanesTelemetry().laneBCompleted < completed &&
			std::chrono::steady_clock::now() < deadline)
		{
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		return PathTraceProducerLanesTelemetry().laneBCompleted >= completed;
	};

	ShutdownPathTraceProducerLanes();
	PathTraceProducerLanesTestSetStartFailureAfter(-1);
	PathTraceProducerLanesTestSetRequiredLaneMaskOverride(-1);
	Check(ConfigurePathTraceProducerLanes(0, 1) &&
		!PathTraceProducerLanesTestState().running &&
		!PathTraceProducerLaneAActive() &&
		ConfigurePathTraceProducerLanes(2, 1) &&
		PathTraceProducerLanesTestState().running &&
		PathTraceProducerLaneAActive() &&
		PathTraceProducerLaneEffectiveMode() == 2 &&
		ConfigurePathTraceProducerLanes(0, 1) &&
		!PathTraceProducerLanesTestState().running &&
		!PathTraceProducerLaneAActive(),
		"actual host keeps mode 0 idle and implements A-only mode 2");
	Check(ConfigurePathTraceProducerLanes(1, 1) &&
		PathTraceProducerLanesTestState().running &&
		PathTraceProducerLanesTestState().startedMask == 1 &&
		PathTraceProducerLaneAActive(),
		"current bit0 configuration starts Lane A only");

	std::uint64_t completed = PathTraceProducerLanesTelemetry().completed;
	std::uint64_t compared = PathTraceProducerLanesTelemetry().compared;
	Check(DispatchPathTraceProducerLaneA(snapshot(1)),
		"actual host dispatches a valid Lane A snapshot");
	Check(waitCompleted(++completed), "product-first Lane A product completes");
	PollPathTraceProducerLanes(1, false);
	Check(PathTraceProducerLanesTelemetry().compared == compared &&
		RecordPathTraceProducerSerialOracle(oracle(1)),
		"product-first slot waits for its matching serial oracle");
	PollPathTraceProducerLanes(1, false);
	Check(PathTraceProducerLanesTelemetry().compared == ++compared,
		"product-first pair compares and releases its slot");

	Check(DispatchPathTraceProducerLaneA(snapshot(2, 3000)) &&
		RecordPathTraceProducerSerialOracle(oracle(2)),
		"oracle-first publication is accepted while Lane A runs");
	Check(waitCompleted(++completed), "oracle-first Lane A product completes");
	PollPathTraceProducerLanes(2, false);
	Check(PathTraceProducerLanesTelemetry().compared == ++compared,
		"oracle-first pair compares and releases its slot");

	Check(DispatchPathTraceProducerLaneA(snapshot(3, 3000)) &&
		DispatchPathTraceProducerLaneA(snapshot(4, 3000)) &&
		DispatchPathTraceProducerLaneA(snapshot(5, 3000)),
		"all three bounded mailbox slots accept distinct generations");
	Check(waitCompleted(completed += 3), "all three mailbox slots complete");
	Check(RecordPathTraceProducerSerialOracle(oracle(3)) &&
		RecordPathTraceProducerSerialOracle(oracle(4)) &&
		RecordPathTraceProducerSerialOracle(oracle(5)),
		"all three slots accept their exact-generation oracles");
	PollPathTraceProducerLanes(5, false);
	Check(PathTraceProducerLanesTelemetry().compared == (compared += 3),
		"all three mailbox slots compare in bounded storage");

	PathTraceProducerLanesTestPauseAfterCommit(true);
	Check(DispatchPathTraceProducerLaneA(snapshot(18)),
		"telemetry interleaving fixture dispatches Lane A");
	{
		const auto deadline = std::chrono::steady_clock::now() +
			std::chrono::seconds(2);
		while (!PathTraceProducerLanesTestCommitPaused() &&
			std::chrono::steady_clock::now() < deadline)
		{
			std::this_thread::yield();
		}
	}
	Check(PathTraceProducerLanesTestCommitPaused() &&
		RecordPathTraceProducerSerialOracle(oracle(18)),
		"oracle publishes between build commit and resident accounting");
	PathTraceProducerLanesTestPauseAfterCommit(false);
	Check(waitCompleted(++completed) &&
		PathTraceProducerLanesTelemetry().laneARingResidentBytes >= 192,
		"resident telemetry reacquires ownership and includes snapshot product oracle");
	PollPathTraceProducerLanes(18, false);
	++compared;

	PathTraceProducerLanesTestPauseDuringAccounting(true);
	Check(DispatchPathTraceProducerLaneA(snapshot(19)),
		"ownership/telemetry atomicity fixture dispatches Lane A");
	{
		const auto deadline = std::chrono::steady_clock::now() +
			std::chrono::seconds(2);
		while (!PathTraceProducerLanesTestAccountingPaused() &&
			std::chrono::steady_clock::now() < deadline)
		{
			std::this_thread::yield();
		}
	}
	std::atomic<bool> oracleAttempted{false};
	bool accountingOracleAccepted = false;
	std::thread oraclePublisher([&]() {
		oracleAttempted.store(true, std::memory_order_release);
		accountingOracleAccepted = RecordPathTraceProducerSerialOracle(oracle(19));
	});
	while (!oracleAttempted.load(std::memory_order_acquire))
	{
		std::this_thread::yield();
	}
	PathTraceProducerLanesTestPauseDuringAccounting(false);
	oraclePublisher.join();
	Check(waitCompleted(++completed) && accountingOracleAccepted &&
		PathTraceProducerLanesTelemetry().laneARingResidentBytes >= 192,
		"ownership-to-telemetry lock order prevents stale lower resident overwrite");
	PollPathTraceProducerLanes(19, false);
	++compared;

	const std::size_t slotCap = RT_PT_CAPTURE_PRODUCT_SLOT_MAX_BYTES;
	const std::size_t justUnderCandidateCap = slotCap / 3 - 1024;
	Check(DispatchPathTraceProducerLaneA(
			snapshot(12, 0, justUnderCandidateCap)) &&
		waitCompleted(++completed) &&
		RecordPathTraceProducerSerialOracle(oracle(12)),
		"complete-slot transaction accepts snapshot plus candidate just under cap");
	PollPathTraceProducerLanes(12, false);
	Check(PathTraceProducerLanesTelemetry().compared == ++compared,
		"just-under complete-slot transaction publishes and compares");

	const RtPathTraceProducerLaneTelemetry attributionBefore =
		PathTraceProducerLanesTelemetry();
	RtPathTraceCaptureOwnerSnapshot geo08Snapshot = snapshot(21);
	geo08Snapshot.buildExact = false;
	geo08Snapshot.buildAcceptedSurfaces = 1;
	geo08Snapshot.buildAcceptedVertices = 5325;
	geo08Snapshot.buildAcceptedSetDiff.available = true;
	geo08Snapshot.buildAcceptedSetDiff.found = true;
	geo08Snapshot.buildAcceptedSetDiff.ordinal = 7;
	geo08Snapshot.buildAcceptedSetDiff.product.present = true;
	geo08Snapshot.buildAcceptedSetDiff.product.terminal =
		RT_PT_CAPTURE_TERMINAL_ACCEPTED_SCALAR;
	geo08Snapshot.buildAcceptedSetDiff.product.surfaceClass = 3;
	geo08Snapshot.buildAcceptedSetDiff.product.sourceFlags = 17;
	geo08Snapshot.buildAcceptedSetDiff.product.vertexCount = 224;
	geo08Snapshot.buildAcceptedSetDiff.product.indexCount = 336;
	geo08Snapshot.buildAcceptedSetDiff.product.rigidReadyByMesh = true;
	geo08Snapshot.buildAcceptedSetDiff.product.skinnedAdmission = 1;
	geo08Snapshot.buildAcceptedSetDiff.oracle.present = true;
	geo08Snapshot.buildAcceptedSetDiff.oracle.terminal = 5;
	geo08Snapshot.buildAcceptedSetDiff.oracle.surfaceClass = 3;
	geo08Snapshot.buildAcceptedSetDiff.oracle.skinnedCaptureOmitted = true;
	geo08Snapshot.buildAcceptedSetDiff.oracle.skinnedAdmission = 0;
	RtPathTraceCaptureLiveCardinalityAttribution geo08Live;
	geo08Live.skinnedOmittedSurfaces = 1;
	geo08Live.skinnedOmittedVertices = 1427;
	geo08Live.skinnedOmittedIndexes = 2142;
	Check(DispatchPathTraceProducerLaneA(std::move(geo08Snapshot)) &&
		waitCompleted(++completed) &&
		RecordPathTraceProducerSerialOracle(oracle(21, 64, 1, 3898, geo08Live)),
		"synthetic GEO08 attribution pair reaches the actual mode-1 host");
	PollPathTraceProducerLanes(21, true);
	++compared;
	const RtPathTraceProducerLaneTelemetry geo08Telemetry =
		PathTraceProducerLanesTelemetry();
	Check(geo08Telemetry.cardinalitySamples ==
			attributionBefore.cardinalitySamples + 1 &&
		geo08Telemetry.cardinalitySums.productAcceptedVertices ==
			attributionBefore.cardinalitySums.productAcceptedVertices + 5325 &&
		geo08Telemetry.cardinalitySums.oracleAcceptedVertices ==
			attributionBefore.cardinalitySums.oracleAcceptedVertices + 3898 &&
		geo08Telemetry.cardinalitySums.live.skinnedOmittedVertices ==
			attributionBefore.cardinalitySums.live.skinnedOmittedVertices + 1427 &&
		geo08Telemetry.cardinalitySums.afterSkinnedVertexDelta ==
			attributionBefore.cardinalitySums.afterSkinnedVertexDelta &&
		geo08Telemetry.cardinalitySums.unattributedVertexDelta ==
			attributionBefore.cardinalitySums.unattributedVertexDelta &&
		geo08Telemetry.firstMismatchGeneration == 21 &&
		geo08Telemetry.firstMismatchCardinality.productAcceptedVertices == 5325 &&
		geo08Telemetry.firstMismatchCardinality.oracleAcceptedVertices == 3898 &&
		geo08Telemetry.firstMismatchCardinality.unattributedVertexDelta == 0 &&
		geo08Telemetry.acceptedSetDiffs == attributionBefore.acceptedSetDiffs + 1 &&
		geo08Telemetry.productOnlyAccepted ==
			attributionBefore.productOnlyAccepted + 1 &&
		geo08Telemetry.firstMismatchAcceptedSetDiff.found &&
		geo08Telemetry.firstMismatchAcceptedSetDiff.ordinal == 7 &&
		geo08Telemetry.firstMismatchAcceptedSetDiff.product.vertexCount == 224 &&
		geo08Telemetry.firstMismatchAcceptedSetDiff.oracle.skinnedCaptureOmitted &&
		geo08Telemetry.shadowSummaryCadences ==
			attributionBefore.shadowSummaryCadences + 1,
		"mode-1 host attributes and publishes the sticky synthetic GEO08 cardinality");

	RtPathTraceCaptureOwnerSnapshot unexplainedSnapshot = snapshot(22);
	unexplainedSnapshot.buildExact = false;
	unexplainedSnapshot.buildAcceptedSurfaces = 1;
	unexplainedSnapshot.buildAcceptedVertices = 5000;
	RtPathTraceCaptureLiveCardinalityAttribution unexplainedLive;
	unexplainedLive.skinnedOmittedSurfaces = 1;
	unexplainedLive.skinnedOmittedVertices = 1000;
	Check(DispatchPathTraceProducerLaneA(std::move(unexplainedSnapshot)) &&
		waitCompleted(++completed) &&
		RecordPathTraceProducerSerialOracle(
			oracle(22, 64, 1, 3900, unexplainedLive)),
		"nonmatching cardinality pair reaches the actual mode-1 host");
	PollPathTraceProducerLanes(22, true);
	++compared;
	const RtPathTraceProducerLaneTelemetry unexplainedTelemetry =
		PathTraceProducerLanesTelemetry();
	Check(unexplainedTelemetry.cardinalitySamples ==
			geo08Telemetry.cardinalitySamples + 1 &&
		unexplainedTelemetry.cardinalitySums.unattributedVertexDelta ==
			geo08Telemetry.cardinalitySums.unattributedVertexDelta + 100 &&
		unexplainedTelemetry.firstMismatchGeneration == 21 &&
		unexplainedTelemetry.firstMismatchCardinality.unattributedVertexDelta == 0 &&
		unexplainedTelemetry.shadowSummaryCadences ==
			geo08Telemetry.shadowSummaryCadences + 1,
		"summary accumulates unexplained delta while the first mismatch stays sticky");

	auto failedBuild = [&](std::uint64_t generation, std::size_t ownedBytes,
		std::uint32_t failureKind, const char* name) {
		const std::uint64_t capBefore =
			PathTraceProducerLanesTelemetry().capFallback;
		const bool dispatched = DispatchPathTraceProducerLaneA(
			snapshot(generation, 0, ownedBytes, failureKind));
		const bool completedNow = dispatched && waitCompleted(++completed);
		const bool oracleAccepted = completedNow &&
			RecordPathTraceProducerSerialOracle(oracle(generation));
		PollPathTraceProducerLanes(generation, false);
		if (oracleAccepted)
		{
			++compared;
		}
		Check(dispatched && completedNow && oracleAccepted &&
			PathTraceProducerLanesTelemetry().capFallback == capBefore + 1,
			name);
	};
	const RtPathTraceProducerLaneTelemetry failureTelemetryBefore =
		PathTraceProducerLanesTelemetry();
	failedBuild(13, slotCap / 2 + 1, 0,
		"single final product plus snapshot over cap fails closed before publication");
	failedBuild(14, 64, 1,
		"injected Lane A bad_alloc is caught and publishes only fail-closed state");
	failedBuild(15, 64, 2,
		"injected Lane A length_error is caught and publishes only fail-closed state");
	const RtPathTraceProducerLaneTelemetry failureTelemetryAfter =
		PathTraceProducerLanesTelemetry();
	Check(failureTelemetryAfter.jobsEarly ==
			failureTelemetryBefore.jobsEarly + 3 &&
		failureTelemetryAfter.firstFailHistogram[
			static_cast<std::size_t>(RtPathTraceCaptureFirstFailure::Incomplete)] ==
			failureTelemetryBefore.firstFailHistogram[
				static_cast<std::size_t>(RtPathTraceCaptureFirstFailure::Incomplete)] + 3,
		"mode-1 host telemetry classifies incomplete fail-closed products as early failures");

	const std::uint64_t oracleCapBefore =
		PathTraceProducerLanesTelemetry().capFallback;
	Check(DispatchPathTraceProducerLaneA(snapshot(16)) && waitCompleted(++completed) &&
		!RecordPathTraceProducerSerialOracle(oracle(16, slotCap)),
		"oracle publication rejects capacity that would overflow the complete slot");
	Check(PathTraceProducerLanesTelemetry().capFallback == oracleCapBefore + 1,
		"oracle overflow increments capFallback without adopting its capacity");
	PollPathTraceProducerLanes(20, false);

	RtPathTraceCaptureOwnerSnapshot releaseSnapshot = snapshot(30, 0, 4096);
	RtPathTraceCaptureProduct releaseProduct;
	releaseProduct.ownedBytes = 4096;
	RtPathTraceCaptureOracle releaseOracle = oracle(30, 4096);
	releaseSnapshot.ResetAndRelease();
	releaseProduct.ResetAndRelease();
	releaseOracle.ResetAndRelease();
	Check(releaseSnapshot.OwnedBytes() == 0 &&
		releaseProduct.OwnedBytes() == 0 && releaseOracle.OwnedBytes() == 64,
		"ResetAndRelease drops all dynamic test backing and restores empty oracle base");
	const RtPathTraceProducerLaneTelemetry capTelemetry =
		PathTraceProducerLanesTelemetry();
	Check(capTelemetry.slotHighWater[0] <= slotCap &&
		capTelemetry.slotHighWater[1] <= slotCap &&
		capTelemetry.slotHighWater[2] <= slotCap,
		"every observed complete-slot high-water remains within 16 MiB");

	const std::uint64_t busyBefore = PathTraceProducerLanesTelemetry().busyFallback;
	Check(DispatchPathTraceProducerLaneA(snapshot(6, 20000)) &&
		!DispatchPathTraceProducerLaneA(snapshot(9)),
		"same modulo slot coalesces by rejecting later busy generation");
	Check(PathTraceProducerLanesTelemetry().busyFallback == busyBefore + 1,
		"occupied mailbox slot records busyFallback without queue growth");
	Check(waitCompleted(++completed) &&
		RecordPathTraceProducerSerialOracle(oracle(6)),
		"coalesced slot retains and completes the original generation");
	PollPathTraceProducerLanes(6, false);
	++compared;

	const std::uint64_t retiredBefore = PathTraceProducerLanesTelemetry().retired;
	Check(DispatchPathTraceProducerLaneA(snapshot(7)) && waitCompleted(++completed),
		"late-retirement fixture produces without an oracle");
	PollPathTraceProducerLanes(11, false);
	Check(PathTraceProducerLanesTelemetry().retired == retiredBefore + 1 &&
		PathTraceProducerLanesTestState().slots[1].state ==
			RtPathTraceProducerSlotState::Free,
		"ready product retires after the bounded late window");

	const RtPathTraceProducerLaneTelemetry lifecycleTelemetryBefore =
		PathTraceProducerLanesTelemetry();
	Check(DispatchPathTraceProducerLaneA(snapshot(8, 5000)),
		"reset fixture launches an in-flight generation");
	ResetPathTraceProducerLanes();
	auto state = PathTraceProducerLanesTestState();
	Check(!state.running && state.startedMask == 0 &&
		!PathTraceProducerLaneAActive() &&
		state.slots[0].state == RtPathTraceProducerSlotState::Free &&
		state.slots[1].state == RtPathTraceProducerSlotState::Free &&
		state.slots[2].state == RtPathTraceProducerSlotState::Free,
		"reset joins Lane A and invalidates every generation slot");
	const RtPathTraceProducerLaneTelemetry resetTelemetry =
		PathTraceProducerLanesTelemetry();
	Check(resetTelemetry.cardinalitySamples ==
			lifecycleTelemetryBefore.cardinalitySamples &&
		resetTelemetry.firstMismatchGeneration == 21 &&
		resetTelemetry.firstMismatchCardinality.unattributedVertexDelta == 0 &&
		resetTelemetry.firstMismatchAcceptedSetDiff.ordinal == 7,
		"lifecycle reset preserves process-scoped cardinality diagnostics while releasing slots");

	Check(ConfigurePathTraceProducerLanes(1, 1) &&
		ConfigurePathTraceProducerLanes(1, 0) &&
		PathTraceProducerLanesTestState().startedMask == 0 &&
		!PathTraceProducerLaneAActive() &&
		ConfigurePathTraceProducerLanes(1, 1) &&
		PathTraceProducerLanesTestState().startedMask == 1 &&
		PathTraceProducerLaneAActive(),
		"lane-mask changes stop reset and transactionally restart Lane A");

	RtPathTraceCaptureProduct consumedProduct;
	std::uint32_t consumedAge = UINT32_MAX;
	Check(ConfigurePathTraceProducerLanes(2, 1) &&
		PathTraceProducerLaneEffectiveMode() == 2 &&
		DispatchPathTraceProducerLaneA(snapshot(30, 3000)) &&
		!TryConsumeLatestPathTraceProducerLaneA(30,
			snapshot(30).lateConsumeToken,
			snapshot(30).buildMembershipReceipt,
			consumedProduct, consumedAge) &&
		waitCompleted(++completed) &&
		DispatchPathTraceProducerLaneA(snapshot(31, 3000)) &&
		TryConsumeLatestPathTraceProducerLaneA(31,
			snapshot(31).lateConsumeToken,
			snapshot(31).buildMembershipReceipt,
			consumedProduct, consumedAge) &&
		consumedProduct.complete && consumedProduct.epoch.generation == 30 &&
		consumedAge == 1,
		"mode 2 misses without waiting on frame 1 then consumes generation 1 at age 1 on frame 2");
	Check(waitCompleted(++completed) &&
		TryConsumeLatestPathTraceProducerLaneA(32,
			snapshot(32).lateConsumeToken,
			snapshot(32).buildMembershipReceipt,
			consumedProduct, consumedAge) &&
		consumedProduct.epoch.generation == 31 && consumedAge == 1,
		"latest-complete mailbox repeats age-1 consumption without poisoning modulo slots");
	Check(DispatchPathTraceProducerLaneA(snapshot(33)) && waitCompleted(++completed),
		"compatibility rejection fixture publishes a ready product");
	RtPathTraceLateConsumeToken wrongToken = snapshot(34).lateConsumeToken;
	++wrongToken.registryGeneration;
	const RtPathTraceProducerLaneTelemetry rejectBefore =
		PathTraceProducerLanesTelemetry();
	Check(TryConsumeLatestPathTraceProducerLaneA(34, wrongToken,
			snapshot(34).buildMembershipReceipt, consumedProduct, consumedAge) &&
		PathTraceProducerLanesTelemetry().staleReject ==
			rejectBefore.staleReject && consumedAge == 1,
		"latest-complete ignores a compatible per-frame registry owner revision");
	RtPathTraceCaptureOwnerSnapshot receiptMismatch = snapshot(34);
	++receiptMismatch.buildMembershipReceipt.hash;
	Check(DispatchPathTraceProducerLaneA(std::move(receiptMismatch)) &&
		waitCompleted(++completed),
		"receipt mismatch fixture publishes a ready product");
	const RtPathTraceProducerLaneTelemetry receiptBefore =
		PathTraceProducerLanesTelemetry();
	Check(!TryConsumeLatestPathTraceProducerLaneA(34,
		snapshot(34).lateConsumeToken,
		snapshot(34).buildMembershipReceipt,
		consumedProduct, consumedAge) &&
		PathTraceProducerLanesTelemetry().receiptMismatch ==
			receiptBefore.receiptMismatch + 1,
		"latest-complete rejects a membership receipt mismatch before consumption");
	RtPathTraceCaptureOwnerSnapshot cpuSkinned = snapshot(35);
	cpuSkinned.buildMembershipReceipt.cpuSkinnedAcceptedCount = 1;
	Check(DispatchPathTraceProducerLaneA(std::move(cpuSkinned)) &&
		waitCompleted(++completed),
		"CPU-skinned eligibility fixture publishes a ready product");
	RtPathTraceCaptureMembershipReceipt currentCpuSkinned =
		snapshot(35).buildMembershipReceipt;
	currentCpuSkinned.cpuSkinnedAcceptedCount = 1;
	const RtPathTraceProducerLaneTelemetry skinnedBefore =
		PathTraceProducerLanesTelemetry();
	Check(!TryConsumeLatestPathTraceProducerLaneA(35,
		snapshot(35).lateConsumeToken, currentCpuSkinned,
		consumedProduct, consumedAge) &&
		PathTraceProducerLanesTelemetry().skinnedEligibilityFail ==
			skinnedBefore.skinnedEligibilityFail + 1,
		"latest-complete rejects current CPU-skinned accepted membership");
	PollPathTraceProducerLanes(37, false);

	std::uint64_t laneBCompleted =
		PathTraceProducerLanesTelemetry().laneBCompleted;
	RtPathTraceAccelCpuProduct consumedAccel;
	std::uint32_t consumedAccelAge = UINT32_MAX;
	const RtPathTraceProducerLaneBOwnershipDecision laneBEnter =
		RtPathTraceResolveProducerLaneBOwnership(false, true);
	const RtPathTraceProducerLaneBOwnershipDecision laneBStay =
		RtPathTraceResolveProducerLaneBOwnership(true, true);
	const RtPathTraceProducerLaneBOwnershipDecision laneBLeave =
		RtPathTraceResolveProducerLaneBOwnership(true, false);
	Check(laneBEnter.entering && !laneBEnter.leaving &&
		!laneBEnter.legacyWorkersMayStart &&
		!laneBStay.entering && !laneBStay.leaving &&
		!laneBStay.legacyWorkersMayStart &&
		laneBLeave.leaving && laneBLeave.legacyWorkersMayStart,
		"Lane B ownership transition drains once, gates legacy starts, and re-enables them on leave");
	Check(!RtPathTraceProducerLaneBShouldArmBootstrap(
			true, 2, false, true) &&
		!RtPathTraceProducerLaneBShouldArmBootstrap(
			true, 2, true, false) &&
		RtPathTraceProducerLaneBShouldArmBootstrap(
			true, 2, true, true),
		"Lane B bootstrap requires a consumed complete load-bearing product, never cached-plan validity alone");
	{
		const fs::path scenePath = fs::path(__FILE__).parent_path() / ".." /
			"renderer" / "NVRHI" / "PathTraceSmokeSceneBuild.cpp";
		std::ifstream sceneInput(scenePath);
		const std::string source{std::istreambuf_iterator<char>(sceneInput),
			std::istreambuf_iterator<char>()};
		const std::size_t disabled = source.find("if (!frame.enabled)");
		const std::size_t disabledReturn = source.find("return frame;", disabled);
		const std::size_t laneBResolve = source.find(
			"static bool ResolvePathTraceLaneBAccelCpuProduct(");
		Check(disabled != std::string::npos &&
			laneBResolve != std::string::npos && laneBResolve < disabledReturn,
			"Lane B rigid/accel dispatch remains reachable when static buckets are disabled");
		const std::size_t ownershipResolve = source.find(
			"RtPathTraceResolveProducerLaneBOwnership(");
		const std::size_t accelerationStop = source.find(
			"m_smokeAccelerationPlanFuture.Stop()", ownershipResolve);
		const std::size_t tlasStop = source.find(
			"m_smokeRigidTlasPlanFuture.Stop()", ownershipResolve);
		const std::size_t routeStop = source.find(
			"m_smokeRigidRouteBuildFuture.Stop()", ownershipResolve);
		const std::size_t legacyGate = source.find(
			"laneBOwnership.legacyWorkersMayStart", ownershipResolve);
		Check(ownershipResolve != std::string::npos &&
			accelerationStop != std::string::npos &&
			tlasStop != std::string::npos && routeStop != std::string::npos &&
			legacyGate != std::string::npos,
			"production Lane B ownership enter drains all three dedicated futures and binds legacy start gates");

		const fs::path lanesPath = fs::path(__FILE__).parent_path() / ".." /
			"renderer" / "NVRHI" / "PathTraceProducerLanes.cpp";
		std::ifstream lanesInput(lanesPath);
		const std::string lanesSource{std::istreambuf_iterator<char>(lanesInput),
			std::istreambuf_iterator<char>()};
		Check(lanesSource.find("lanes=A(%s),B(%s),C(not-started)") !=
				std::string::npos &&
			lanesSource.find("producerLaneB mode=shadow") != std::string::npos &&
			lanesSource.find("producerLaneB mode=consume") != std::string::npos,
			"producer telemetry derives A/B state from effective mask and reports bounded Lane B shadow/consume summaries");
		const std::size_t runLaneB = lanesSource.find("void RunLaneB(");
		const std::size_t runIdleLane = lanesSource.find("void RunIdleLane(", runLaneB);
		const std::string laneBWorkerSource =
			(runLaneB != std::string::npos && runIdleLane != std::string::npos)
				? lanesSource.substr(runLaneB, runIdleLane - runLaneB) : std::string();
		Check(!laneBWorkerSource.empty() &&
			laneBWorkerSource.find("RT_PT_ACCEL_CPU_SLOT_MAX_BYTES") != std::string::npos &&
			laneBWorkerSource.find("RT_PT_CAPTURE_PRODUCT_SLOT_MAX_BYTES") == std::string::npos,
			"production Lane B worker uses only the AccelCpuPack slot cap");

		const std::size_t laneBRigidApply = source.find(
			"OPTICK_EVENT(\"PT Lane B Rigid Route Apply\")");
		const std::size_t laneBCompleteGuard = source.find(
			"if (producerLaneBFrameProduct.complete)", laneBRigidApply);
		const std::size_t laneBRefresh = source.find(
			"OPTICK_EVENT(\"PT Rigid Route Refresh Transforms\")", laneBCompleteGuard);
		const std::string laneBRigidApplySource =
			(laneBCompleteGuard != std::string::npos && laneBRefresh != std::string::npos)
				? source.substr(laneBCompleteGuard, laneBRefresh - laneBCompleteGuard)
				: std::string();
		Check(!laneBRigidApplySource.empty() &&
			laneBRigidApplySource.find("rigidRouteBuild = std::move(producerLaneBFrameProduct.rigidBuild)") != std::string::npos &&
			laneBRigidApplySource.find("BuildRigidRouteBuffersTimedResult(") == std::string::npos &&
			laneBRigidApplySource.find("RemapRigidRouteMaterialIndexes(") != std::string::npos,
			"Lane B rigid apply requires a complete product, preserves serial build on miss, and remaps live materials");
		Check(source.find("CapturePathTraceLaneBAccelCpuSnapshotPreflighted") ==
				std::string::npos &&
			source.find("OPTICK_EVENT(\"PT Lane B Publish\")") !=
				std::string::npos,
			"production Lane B owner path publishes resident input without per-frame Count+Fill snapshot construction");
		const std::size_t heavySignatureBegin = source.find(
			"uint64_t BuildSmokeLaneBAccelerationPublishSignature(");
		const std::size_t heavySignatureEnd = source.find(
			"RtSmokeStaticBucketFramePublication BuildSmokeStaticBucketFramePublication(",
			heavySignatureBegin);
		const std::string heavySignatureSource =
			heavySignatureBegin != std::string::npos &&
			heavySignatureEnd != std::string::npos
				? source.substr(heavySignatureBegin,
					heavySignatureEnd - heavySignatureBegin)
				: std::string();
		Check(!heavySignatureSource.empty() &&
			heavySignatureSource.find("dynamicVertexCount") == std::string::npos &&
			heavySignatureSource.find("dynamicIndexCount") == std::string::npos &&
			heavySignatureSource.find("staticCache.") == std::string::npos &&
			heavySignatureSource.find("totalVertexCount") == std::string::npos &&
			heavySignatureSource.find("totalIndexCount") == std::string::npos &&
			heavySignatureSource.find("totalTriangleCount") == std::string::npos &&
			heavySignatureSource.find("vertexOffset") == std::string::npos &&
			heavySignatureSource.find("indexOffset") == std::string::npos &&
			heavySignatureSource.find("triangleOffset") == std::string::npos &&
			source.find("StaticResidentPayloadGeneration()") != std::string::npos,
			"Lane B heavy resident identity excludes per-frame ticket facts and includes mutation-complete static generation");
		const std::size_t staticPublicationBegin = heavySignatureEnd;
		const std::size_t staticPublicationEnd = source.find(
			"static void BuildRegistryAffineFromObjectToWorld(",
			staticPublicationBegin);
		const std::string staticPublicationSource =
			staticPublicationBegin != std::string::npos &&
			staticPublicationEnd != std::string::npos
				? source.substr(staticPublicationBegin,
					staticPublicationEnd - staticPublicationBegin)
				: std::string();
		Check(!staticPublicationSource.empty() &&
			staticPublicationSource.find(
				"m_smokeProducerLaneBBootstrapComplete = false") ==
				std::string::npos,
			"resident publication rejection cannot re-arm same-epoch serial bootstrap");
		Check(source.find(
				"RtPathTraceProducerLaneBShouldArmBootstrap(") !=
				std::string::npos &&
			source.find(
				"PathTraceProducerLaneBEffectiveMode() == 2 &&\n        m_smokeAccelerationPlanAsyncCachedPlanValid") ==
				std::string::npos &&
			source.find("OverlaySmokeCurrentDynamicAccelerationPlan(") !=
				std::string::npos,
			"production bootstrap uses consumed-product authority and overlays current dynamic counts");

		const fs::path geometryPath = fs::path(__FILE__).parent_path() / ".." /
			"renderer" / "NVRHI" / "PathTraceGeometryUniverse.cpp";
		std::ifstream geometryInput(geometryPath);
		const std::string geometrySource{
			std::istreambuf_iterator<char>(geometryInput),
			std::istreambuf_iterator<char>()};
		const std::size_t portalMutator = geometrySource.find(
			"bool RtSmokeGeometryUniverse::RefreshStaticSurfacePortalArea(");
		const std::size_t bucketMutator = geometrySource.find(
			"bool RtSmokeGeometryUniverse::RefreshStaticSurfaceBucketKey(");
		const std::size_t staticLookup = geometrySource.find(
			"bool RtSmokeGeometryUniverse::HasStaticSurface(", bucketMutator);
		const std::string portalMutatorSource =
			portalMutator != std::string::npos && bucketMutator != std::string::npos
				? geometrySource.substr(portalMutator, bucketMutator - portalMutator)
				: std::string();
		const std::string bucketMutatorSource =
			bucketMutator != std::string::npos && staticLookup != std::string::npos
				? geometrySource.substr(bucketMutator, staticLookup - bucketMutator)
				: std::string();
		Check(portalMutatorSource.find(
				"AdvancePathTraceResidentPayloadGeneration") != std::string::npos &&
			bucketMutatorSource.find(
				"AdvancePathTraceResidentPayloadGeneration") != std::string::npos,
			"real portal-area and bucket-key mutators advance shared resident payload identity authority");
		const fs::path residentPath = fs::path(__FILE__).parent_path() / ".." /
			"renderer" / "NVRHI" / "PathTraceAccelCpuResident.cpp";
		std::ifstream residentInput(residentPath);
		const std::string residentSource{
			std::istreambuf_iterator<char>(residentInput),
			std::istreambuf_iterator<char>()};
		Check(residentSource.find("CountRigidRouteResidentMeshPayload(") !=
				std::string::npos &&
			residentSource.find("FillRigidRouteResidentMeshPayloadPreReserved(") !=
				std::string::npos &&
			residentSource.find("CountRigidRouteBuildSnapshot(") ==
				std::string::npos &&
			residentSource.find("FillRigidRouteBuildSnapshotPreReserved(") ==
				std::string::npos &&
			residentSource.find("candidate.rigidRoute.plan =") ==
				std::string::npos &&
			residentSource.find("candidate.rigidRoute.materialTableIds =") ==
				std::string::npos &&
			residentSource.find("candidate.rigidRoute.instanceEligibility =") ==
				std::string::npos,
			"production resident publisher counts and fills only rigid mesh payload without clear-after-copy ticket rows");
		const std::size_t laneBResolveEnd = source.find(
			"uint64_t BuildSmokeLaneBAccelerationPublishSignature", laneBResolve);
		const std::string laneBResolveSource =
			laneBResolve != std::string::npos && laneBResolveEnd != std::string::npos
				? source.substr(laneBResolve, laneBResolveEnd - laneBResolve)
				: std::string();
		Check(!laneBResolveSource.empty() &&
			laneBResolveSource.find("BuildPathTraceAccelCpuProduct(snapshot, frameProduct)") ==
				std::string::npos,
			"Lane B mode-2 miss cannot invoke synchronous AccelCpuPack fallback on MainThread");
	}
	Check(ConfigurePathTraceProducerLanes(2, 3) &&
		PathTraceProducerLaneAActive() && PathTraceProducerLaneBActive() &&
		PathTraceProducerLanesTestState().startedMask == 3,
		"mask bits 0/1 start independent persistent Lane A and Lane B workers");
	const RtPathTraceAccelCpuSnapshot b50 = laneBSnapshot(50, 5000);
	Check(DispatchPathTraceProducerLaneB(laneBSnapshot(50, 5000)) &&
		!TryConsumeLatestPathTraceProducerLaneB(50,
			b50.compatibility, b50.inputReceipt,
			consumedAccel, consumedAccelAge) &&
		waitLaneBCompleted(++laneBCompleted),
		"Lane B frame 1 dispatch has an immediate no-wait miss then publishes");
	const RtPathTraceAccelCpuSnapshot b51 = laneBSnapshot(51, 5000);
	Check(DispatchPathTraceProducerLaneB(laneBSnapshot(51, 5000)) &&
		TryConsumeLatestPathTraceProducerLaneB(51,
			b51.compatibility, b51.inputReceipt,
			consumedAccel, consumedAccelAge) &&
		consumedAccel.complete && consumedAccel.epoch.generation == 50 &&
		consumedAccelAge == 1 && consumedAccel.rigidSignature == 101 &&
		consumedAccel.accelerationSignature == 202 &&
		consumedAccel.staticSignature == 303 &&
		PathTraceProducerLanesTestState().laneBSlots[50 % 3].state ==
			RtPathTraceProducerSlotState::Consuming,
		"Lane B consumes all three CPU components atomically at age 1 without waiting");
	ReleasePathTraceProducerLaneBProduct(consumedAccel);
	Check(PathTraceProducerLanesTestState().laneBSlots[50 % 3].state ==
		RtPathTraceProducerSlotState::Free,
		"Lane B selected slot remains pinned until the GPU-reader lease releases");
	Check(waitLaneBCompleted(++laneBCompleted) &&
		TryConsumeLatestPathTraceProducerLaneB(52,
			b51.compatibility, b51.inputReceipt,
			consumedAccel, consumedAccelAge) &&
		consumedAccel.epoch.generation == 51 && consumedAccelAge == 1,
		"Lane B triple mailbox repeats age-1 consume without slot poisoning");
	ReleasePathTraceProducerLaneBProduct(consumedAccel);

	Check(DispatchPathTraceProducerLaneB(laneBSnapshot(53)) &&
		waitLaneBCompleted(++laneBCompleted),
		"Lane B compatibility fixture publishes");
	RtPathTraceAccelCpuCompatibilityToken badBToken =
		laneBSnapshot(54).compatibility;
	++badBToken.configFingerprint;
	const RtPathTraceProducerLaneTelemetry laneBRejectBefore =
		PathTraceProducerLanesTelemetry();
	Check(!TryConsumeLatestPathTraceProducerLaneB(54, badBToken,
			laneBSnapshot(54).inputReceipt, consumedAccel, consumedAccelAge) &&
		PathTraceProducerLanesTelemetry().laneBStaleReject ==
			laneBRejectBefore.laneBStaleReject + 1,
		"Lane B rejects incompatible map/config/lifecycle token before install");
	Check(DispatchPathTraceProducerLaneB(laneBSnapshot(54)) &&
		waitLaneBCompleted(++laneBCompleted),
		"Lane B receipt fixture publishes");
	const RtPathTraceProducerLaneTelemetry laneBReceiptBefore =
		PathTraceProducerLanesTelemetry();
	Check(!TryConsumeLatestPathTraceProducerLaneB(55,
			laneBSnapshot(55).compatibility,
			laneBSnapshot(55).inputReceipt + 1,
			consumedAccel, consumedAccelAge) &&
		PathTraceProducerLanesTelemetry().laneBReceiptReject ==
			laneBReceiptBefore.laneBReceiptReject + 1,
		"Lane B rejects rigid/static/acceleration receipt changes atomically");

	const std::uint64_t laneBCapBefore =
		PathTraceProducerLanesTelemetry().laneBCapFallback;
	Check(!DispatchPathTraceProducerLaneB(laneBSnapshot(56, 0,
			RT_PT_ACCEL_CPU_SLOT_MAX_BYTES + 1)) &&
		PathTraceProducerLanesTelemetry().laneBCapFallback == laneBCapBefore + 1 &&
		PathTraceProducerLanesTestState().laneBSlots[56 % 3].state ==
			RtPathTraceProducerSlotState::Free,
		"Lane B preflight cap failure publishes no partial product");
	Check(DispatchPathTraceProducerLaneB(laneBSnapshot(57, 0,
			RT_PT_ACCEL_CPU_SLOT_MAX_BYTES / 2u + 1u)) &&
		waitLaneBCompleted(++laneBCompleted) &&
		!TryConsumeLatestPathTraceProducerLaneB(57,
			laneBSnapshot(57).compatibility,
			laneBSnapshot(57).inputReceipt,
			consumedAccel, consumedAccelAge),
		"Lane B combined snapshot/product cap failure stays incomplete");
	Check(DispatchPathTraceProducerLaneB(laneBSnapshot(58, 0, 64, 1)) &&
		waitLaneBCompleted(++laneBCompleted) &&
		!TryConsumeLatestPathTraceProducerLaneB(58,
			laneBSnapshot(58).compatibility,
			laneBSnapshot(58).inputReceipt,
			consumedAccel, consumedAccelAge),
		"Lane B bad_alloc boundary publishes no partial product");
	Check(DispatchPathTraceProducerLaneB(laneBSnapshot(59, 0, 64, 2)) &&
		waitLaneBCompleted(++laneBCompleted) &&
		!TryConsumeLatestPathTraceProducerLaneB(59,
			laneBSnapshot(59).compatibility,
			laneBSnapshot(59).inputReceipt,
			consumedAccel, consumedAccelAge) &&
		PathTraceProducerLanesTelemetry().laneBSlotHighWater[0] <=
			RT_PT_ACCEL_CPU_SLOT_MAX_BYTES &&
		PathTraceProducerLanesTelemetry().laneBSlotHighWater[1] <=
			RT_PT_ACCEL_CPU_SLOT_MAX_BYTES &&
		PathTraceProducerLanesTelemetry().laneBSlotHighWater[2] <=
			RT_PT_ACCEL_CPU_SLOT_MAX_BYTES,
		"Lane B length_error boundary releases backing and respects slot high-water");
	const RtPathTraceAccelCpuSnapshot b61 = laneBSnapshot(61);
	Check(DispatchPathTraceProducerLaneB(laneBSnapshot(61)) &&
		waitLaneBCompleted(++laneBCompleted) &&
		TryConsumeLatestPathTraceProducerLaneB(65,
			b61.compatibility, b61.inputReceipt,
			consumedAccel, consumedAccelAge) &&
		consumedAccel.epoch.generation == 61 && consumedAccelAge == 4,
		"Lane B consumes an older resident-compatible product without an exact-frame wait");
	ReleasePathTraceProducerLaneBProduct(consumedAccel);
	Check(ConfigurePathTraceProducerLanes(2, 2) &&
		!PathTraceProducerLaneAActive() && PathTraceProducerLaneBActive(),
		"mask 2 isolates Lane A while keeping Lane B active");

	Check(ConfigurePathTraceProducerLanes(1, 2) &&
		!PathTraceProducerLaneAActive() && PathTraceProducerLaneBActive() &&
		DispatchPathTraceProducerLaneB(laneBSnapshot(60)) &&
		waitLaneBCompleted(++laneBCompleted) &&
		RecordPathTraceProducerLaneBSerialOracle(laneBOracle(60)),
		"Lane B shadow records a serial oracle from the same owned input");
	const std::uint64_t laneBComparedBefore =
		PathTraceProducerLanesTelemetry().laneBCompared;
	PollPathTraceProducerLanes(60, false);
	Check(PathTraceProducerLanesTelemetry().laneBCompared ==
			laneBComparedBefore + 1 &&
		PathTraceProducerLanesTelemetry().laneBExact > 0,
		"Lane B shadow compares rigid, acceleration and static products exactly");

	Check(ConfigurePathTraceProducerLanes(2, 3) &&
		DispatchPathTraceProducerLaneB(laneBSnapshot(70, 30000)) &&
		DispatchPathTraceProducerLaneA(snapshot(70)) &&
		waitCompleted(++completed),
		"a delayed Lane B neither waits nor suppresses independent Lane A completion");
	ResetPathTraceProducerLanes();
	Check(!PathTraceProducerLaneAActive() && !PathTraceProducerLaneBActive() &&
		PathTraceProducerLanesTestState().laneBSlots[0].state ==
			RtPathTraceProducerSlotState::Free &&
		PathTraceProducerLanesTestState().laneBSlots[1].state ==
			RtPathTraceProducerSlotState::Free &&
		PathTraceProducerLanesTestState().laneBSlots[2].state ==
			RtPathTraceProducerSlotState::Free,
		"reset joins in-flight Lane B and prevents stale Ready publication");

	ConfigurePathTraceProducerLanes(0, 0);
	const RtPathTraceProducerLaneTelemetry modeZeroBefore =
		PathTraceProducerLanesTelemetry();
	PollPathTraceProducerLanes(100, true);
	const RtPathTraceProducerLaneTelemetry modeZeroAfter =
		PathTraceProducerLanesTelemetry();
	Check(modeZeroAfter.cardinalitySamples == modeZeroBefore.cardinalitySamples &&
		modeZeroAfter.acceptedSetDiffs == modeZeroBefore.acceptedSetDiffs &&
		modeZeroAfter.shadowSummaryCadences == modeZeroBefore.shadowSummaryCadences &&
		modeZeroAfter.firstMismatchGeneration ==
			modeZeroBefore.firstMismatchGeneration,
		"mode 0 does not mutate or emit cardinality diagnostics");
	PathTraceProducerLanesTestSetStartFailureAfter(0);
	Check(!ConfigurePathTraceProducerLanes(1, 1) &&
		!PathTraceProducerLanesTestState().running &&
		PathTraceProducerLanesTestState().startedMask == 0 &&
		!PathTraceProducerLaneAActive(),
		"injected Lane-A start failure leaves the host stopped");
	PathTraceProducerLanesTestSetStartFailureAfter(1);
	PathTraceProducerLanesTestSetRequiredLaneMaskOverride(7);
	Check(!ConfigurePathTraceProducerLanes(1, 7) &&
		!PathTraceProducerLanesTestState().running &&
		PathTraceProducerLanesTestState().startedMask == 0 &&
		!PathTraceProducerLaneAActive(),
		"future partial multi-lane start failure rolls back the started Lane A");
	PathTraceProducerLanesTestSetStartFailureAfter(-1);
	PathTraceProducerLanesTestSetRequiredLaneMaskOverride(-1);
	ShutdownPathTraceProducerLanes();
}

void TestCaptureProductR3Bindings()
{
	const fs::path sourceRoot = fs::path(__FILE__).parent_path().parent_path() /
		"renderer" / "NVRHI";
	const auto readText = [](const fs::path& path)
	{
		std::ifstream input(path);
		return std::string(std::istreambuf_iterator<char>(input),
			std::istreambuf_iterator<char>());
	};
	const std::string scene = readText(sourceRoot / "PathTraceSceneCapture.cpp");
	const std::string product = readText(sourceRoot / "PathTraceCaptureProduct.cpp");
	const std::string runtimeKernel = readText(
		sourceRoot / "PathTraceRuntimeMaterialEvalKernel.h");
	const std::string rigidIdentity = readText(
		sourceRoot / "PathTraceRigidIdentity.cpp");
	const std::string semanticKernel = readText(
		sourceRoot / "PathTraceOwnerSemanticKernel.cpp");
	Check(!scene.empty() && !product.empty() &&
		scene.find("BuildPathTraceRuntimeMaterialEvalFromPod") != std::string::npos &&
		scene.find("SelectPathTraceRuntimeMaterialVariant") != std::string::npos &&
		product.find("BuildPathTraceRuntimeMaterialEvalFromPod") != std::string::npos &&
		product.find("BuildPathTraceRuntimeMaterialDecisionFromPod") != std::string::npos &&
		runtimeKernel.find("return SelectPathTraceRuntimeMaterialVariant(") != std::string::npos,
		"R3 live wrapper and Lane A bind to the same runtime material kernel");
	Check(product.find("snapshot.registryMaterials.data()") != std::string::npos &&
		product.find("snapshot.variantBases.data()") != std::string::npos,
		"R3 Lane A consumes both registry snapshot families");
	const std::size_t snapshotBegin = scene.find(
		"static bool CapturePathTraceOwnerSnapshotInternal(");
	const std::size_t snapshotEnd = scene.find(
		"bool CapturePathTraceOwnerSnapshot(", snapshotBegin);
	const std::string snapshotBody = snapshotBegin != std::string::npos &&
		snapshotEnd != std::string::npos
		? scene.substr(snapshotBegin, snapshotEnd - snapshotBegin) : std::string();
	const std::size_t stageTwoGuard = snapshotBody.find("if (!sourceOnly)");
	const std::size_t finalizeBegin = semanticKernel.find(
		"bool FinalizePathTraceOwnerSemanticSnapshot(");
	const std::size_t finalizeEnd = semanticKernel.size();
	const std::string finalizeBody = finalizeBegin != std::string::npos &&
		finalizeEnd != std::string::npos
		? semanticKernel.substr(finalizeBegin, finalizeEnd - finalizeBegin) : std::string();
	const std::size_t readinessCall = finalizeBody.find(
		"FillPathTraceCaptureRigidReadyFacts(");
	const std::size_t derivedMark = finalizeBody.find(
		"raw.semanticFactsDerived = true");
	Check(!snapshotBody.empty() && stageTwoGuard != std::string::npos &&
		readinessCall != std::string::npos && derivedMark != std::string::npos &&
		snapshotBody.find("FinalizePathTraceOwnerSemanticSnapshot(",
			stageTwoGuard) != std::string::npos &&
		snapshotBody.find("raw.requestedModelSurfaceIndex") != std::string::npos &&
		snapshotBody.find("raw.currentTriToken") != std::string::npos &&
		snapshotBody.find("raw.modelTableIndex") != std::string::npos &&
		snapshotBody.find("raw.canonicalInstance") != std::string::npos &&
		snapshotBody.find("raw.jointSource") != std::string::npos,
		"shared stage 1 marshals source inputs and stage 2 alone derives readiness");
	const std::size_t sourceWrapper = scene.find(
		"bool CapturePathTraceOwnerSourceSnapshot(");
	const std::size_t sourceWrapperEnd = scene.find(
		"bool SmokeSkinnedCaptureSplitGateEnabled", sourceWrapper);
	const std::string sourceWrapperBody = sourceWrapper != std::string::npos &&
		sourceWrapperEnd != std::string::npos
		? scene.substr(sourceWrapper, sourceWrapperEnd - sourceWrapper)
		: std::string();
	Check(sourceWrapperBody.find("nullptr, nullptr") != std::string::npos &&
		sourceWrapperBody.find("false, true") != std::string::npos,
		"frontend stage 1 supplies no live facts provider and cannot query readiness");
	const std::size_t gateBeforeClassify = finalizeBody.find(
		"if (skinnedCaptureSplitGate &&");
	const std::size_t canonicalLookup = finalizeBody.find(
		"facts.FindCanonicalIdentityBinding", gateBeforeClassify);
	Check(gateBeforeClassify != std::string::npos &&
		canonicalLookup != std::string::npos && gateBeforeClassify < canonicalLookup,
		"gate-off admission returns without canonical provider calls");
	const std::size_t decisionInputBegin = scene.find(
		"static bool BuildSmokeRuntimeMaterialDecisionInput(");
	const std::size_t decisionInputEnd = scene.find(
		"static RtPathTraceRuntimeMaterialDecisionPod BuildSmokeRuntimeMaterialDecision(",
		decisionInputBegin);
	const std::string decisionInputBody = decisionInputBegin != std::string::npos &&
		decisionInputEnd != std::string::npos
		? scene.substr(decisionInputBegin, decisionInputEnd - decisionInputBegin)
		: std::string();
	const std::size_t serialPodKey = decisionInputBody.find(
		"BuildPathTraceCaptureSerialRuntimeMaterialVariantKey");
	const std::size_t activeFailClosed = decisionInputBody.find(
		"if (snapshotOracleActive)", serialPodKey);
	const std::size_t liveFallback = decisionInputBody.find(
		"ResolvePathTraceRigidModelSurfaceIndex", activeFailClosed);
	Check(serialPodKey != std::string::npos &&
		activeFailClosed != std::string::npos && liveFallback != std::string::npos &&
		serialPodKey < activeFailClosed && activeFailClosed < liveFallback,
		"R3 serial oracle consumes the slot token table and never re-walks the model on an active snapshot");
	Check(product.find("BuildPathTraceRuntimeMaterialVariantKeyFromPod") != std::string::npos &&
		product.find("meshIdentity.modelSurfaceIndex = rigidIdentityModelSurfaceIndex") != std::string::npos &&
		product.find("instanceIdentity.modelSurfaceIndex = rigidIdentityModelSurfaceIndex") != std::string::npos &&
		rigidIdentity.find("ResolvePathTraceModelSurfaceIndexFromPod") != std::string::npos &&
		rigidIdentity.find(
			"ResolvePathTraceCaptureSerialRigidIdentitySurfaceFromSnapshot") !=
			std::string::npos,
		"R3 variant, rigid mesh and instance identity bind to the shared POD resolver");
	const std::size_t serialRigidBegin = product.find(
		"bool ResolvePathTraceCaptureSerialRigidIdentitySurfaceFromSnapshot(");
	const std::size_t serialRigidEnd = product.find(
		"std::uint32_t PathTraceCaptureSerialOracleSourceState(", serialRigidBegin);
	const std::string serialRigidBody = serialRigidBegin != std::string::npos &&
		serialRigidEnd != std::string::npos
		? product.substr(serialRigidBegin, serialRigidEnd - serialRigidBegin)
		: std::string();
	const std::size_t activeRow = serialRigidBody.find(
		"const RtPathTraceCaptureRawSurface& raw");
	const std::size_t comparedMarker = serialRigidBody.find(
		"comparedCall = true", activeRow);
	const std::size_t tokenSpan = serialRigidBody.find(
		"CaptureModelTokenSpan", comparedMarker);
	const std::size_t sharedTransition = serialRigidBody.find(
		"PlanPathTraceSerialRigidSnapshotUseFromPod", tokenSpan);
	Check(!serialRigidBody.empty() && activeRow != std::string::npos &&
		comparedMarker != std::string::npos && tokenSpan != std::string::npos &&
		sharedTransition != std::string::npos && activeRow < comparedMarker &&
		comparedMarker < tokenSpan && tokenSpan < sharedTransition,
		"R3 active serial rigid rows mark compared before validation and use the shared fail-closed transition");
	const std::size_t evalBegin = scene.find(
		"RtSmokeDynamicEvalBuildResult BuildSmokeDynamicMaterialEvalSampleForId(");
	const std::size_t evalEnd = scene.find(
		"void AddSmokeDynamicMaterialEvalStatsInternal", evalBegin);
	const std::string evalBody = evalBegin != std::string::npos &&
		evalEnd != std::string::npos
		? scene.substr(evalBegin, evalEnd - evalBegin) : std::string();
	Check(!evalBody.empty() &&
		evalBody.find("NotePathTraceCaptureSerialRuntimeMaterial") == std::string::npos,
		"R3 generic material evaluator and stats path cannot mutate the serial oracle");
	const std::size_t tableBegin = scene.find(
		"static uint32_t SmokeRuntimeMaterialTableIdPrepared(");
	const std::size_t tableEnd = scene.find(
		"static bool CapturePathTraceRuntimeMaterialStages", tableBegin);
	const std::string tableBody = tableBegin != std::string::npos &&
		tableEnd != std::string::npos
		? scene.substr(tableBegin, tableEnd - tableBegin) : std::string();
	Check(!tableBody.empty() &&
		tableBody.find("SelectPathTraceRuntimeMaterialVariant") != std::string::npos &&
		tableBody.find("NotePathTraceCaptureSerialRuntimeMaterial") != std::string::npos,
		"R3 authoritative registration route selects and records one final shared decision");
}

void TestR3005SkinnedBlasBufferDescriptorSourceContract()
{
	const fs::path candidates[] = {
		fs::path(__FILE__).parent_path() / ".." / "renderer" / "NVRHI" /
			"PathTraceCpuProducerRewriteSkinned.cpp",
		fs::path("E:/prog/rbdoom-3-bfg-rt-producer/neo/renderer/NVRHI/PathTraceCpuProducerRewriteSkinned.cpp")
	};
	std::string source;
	for (const fs::path& candidate : candidates)
	{
		std::ifstream input(candidate);
		if (input)
		{
			source.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
			if (!source.empty())
			{
				break;
			}
		}
	}
	Check(!source.empty(), "R3-005 skinned commit source loaded and is non-empty");
	if (source.empty())
	{
		return;
	}
	const char* accelUsage = "RewriteStructuredBufferUsage::AccelStructBuildInput";
	const std::size_t helperAnchor = source.find(
		"nvrhi::BufferHandle RewriteCreateStructuredBuffer(");
	const std::size_t blasInputAnchor = source.find("meshBlasDesc.indexBuffer");
	Check(helperAnchor != std::string::npos,
		"R3-005 skinned commit source has structured-buffer helper anchor");
	Check(blasInputAnchor != std::string::npos,
		"R3-005 skinned commit source has BLAS index-input anchor");
	if (helperAnchor == std::string::npos || blasInputAnchor == std::string::npos)
	{
		return;
	}

	const std::size_t commitBegin = source.find(
		"bool PathTracePrimaryPass::CommitRewriteSkinnedGpuSkinAndHitRoute(");
	const std::string commit = commitBegin != std::string::npos
		? source.substr(commitBegin) : std::string();
	Check(!commit.empty(), "R3-005 skinned commit function region is anchored");
	if (commit.empty())
	{
		return;
	}
	const std::string helper = helperAnchor < commitBegin
		? source.substr(helperAnchor, commitBegin - helperAnchor) : std::string();
	Check(!helper.empty() &&
		helper.find("desc.isAccelStructBuildInput") != std::string::npos &&
		helper.find(accelUsage) != std::string::npos,
		"R3-005 structured-buffer helper maps explicit acceleration-input usage");
	const auto allocationSlice = [&](const char* debugName)
	{
		const std::string quotedName = std::string("\"") + debugName + "\"";
		const std::size_t namePos = commit.find(quotedName);
		if (namePos == std::string::npos)
		{
			return std::string();
		}
		const std::size_t begin = commit.rfind("RewriteCreateStructuredBuffer(", namePos);
		const std::size_t end = commit.find(");", namePos);
		return begin != std::string::npos && end != std::string::npos && begin < namePos
			? commit.substr(begin, end + 2 - begin) : std::string();
	};
	const std::string packedIndexes = allocationSlice("PathTraceRewriteSkinnedIndexes");
	const std::string skinnedOutput = allocationSlice("PathTraceRewriteSkinnedOutput");
	const std::string blasIndexes = allocationSlice("PathTraceRewriteSkinnedBLASIndexes");
	Check(!packedIndexes.empty() && !skinnedOutput.empty() && !blasIndexes.empty(),
		"R3-005 all three named skinned allocation slices exist");

	std::size_t explicitUsageCount = 0;
	for (std::size_t pos = commit.find(accelUsage); pos != std::string::npos;
		pos = commit.find(accelUsage, pos + 1))
	{
		++explicitUsageCount;
	}
	Check(explicitUsageCount == 2,
		"R3-005 skinned commit has exactly two explicit acceleration-input usage requests");
	Check(!skinnedOutput.empty() && skinnedOutput.find(accelUsage) != std::string::npos,
		"R3-005 PathTraceRewriteSkinnedOutput requests acceleration-input usage");
	Check(!blasIndexes.empty() && blasIndexes.find(accelUsage) != std::string::npos,
		"R3-005 PathTraceRewriteSkinnedBLASIndexes requests acceleration-input usage");
	Check(!packedIndexes.empty() && packedIndexes.find(accelUsage) == std::string::npos,
		"R3-005 PathTraceRewriteSkinnedIndexes remains unflagged");
}

void TestR3006SkinnedWorldSpaceAndProfileSourceContract()
{
	const auto readSource = [](const fs::path* candidates, std::size_t count)
	{
		for (std::size_t i = 0; i < count; ++i)
		{
			std::ifstream input(candidates[i]);
			if (input)
			{
				std::string text{ std::istreambuf_iterator<char>(input),
					std::istreambuf_iterator<char>() };
				if (!text.empty()) return text;
			}
		}
		return std::string();
	};
	const fs::path rewriteCandidates[] = {
		fs::path(__FILE__).parent_path() / ".." / "renderer" / "NVRHI" /
			"PathTraceCpuProducerRewriteSkinned.cpp",
		fs::path("E:/prog/rbdoom-3-bfg-rt-producer/neo/renderer/NVRHI/PathTraceCpuProducerRewriteSkinned.cpp")
	};
	const fs::path shaderCandidates[] = {
		fs::path(__FILE__).parent_path() / ".." / "shaders" / "builtin" /
			"pathtracing" / "pathtrace_skinning.cs.hlsl",
		fs::path("E:/prog/rbdoom-3-bfg-rt-producer/neo/shaders/builtin/pathtracing/pathtrace_skinning.cs.hlsl")
	};
	const std::string rewrite = readSource(rewriteCandidates, 2);
	const std::string shader = readSource(shaderCandidates, 2);
	Check(!rewrite.empty(), "R3-006 rewrite skinned source loaded and is non-empty");
	Check(!shader.empty(), "R3-006 skinning shader source loaded and is non-empty");
	if (rewrite.empty() || shader.empty()) return;

	const std::size_t transformCall = shader.find(
		"const float3 worldPosition = TransformObjectPosition(");
	const std::size_t currentTransform = shader.find(
		"dispatchRecord.currentObjectToWorld0", transformCall);
	const std::size_t outputPosition = shader.find(
		"outputVertex.position = float4(worldPosition, 1.0);", currentTransform);
	Check(transformCall != std::string::npos,
		"R3-006 shader has anchored world-position transform call");
	Check(currentTransform != std::string::npos,
		"R3-006 shader world-position transform uses currentObjectToWorld0");
	Check(outputPosition != std::string::npos,
		"R3-006 shader assigns the transformed result to outputVertex.position");

	const std::size_t commitBegin = rewrite.find(
		"bool PathTracePrimaryPass::CommitRewriteSkinnedGpuSkinAndHitRoute(");
	Check(commitBegin != std::string::npos,
		"R3-006 rewrite skinned commit function is anchored");
	if (commitBegin == std::string::npos) return;
	const std::string commit = rewrite.substr(commitBegin);
	const std::size_t extraBegin = commit.find("nvrhi::rt::InstanceDesc extra;");
	const std::size_t extraEnd = commit.find("extraTlas.push_back(extra);", extraBegin);
	const std::string extra = extraBegin != std::string::npos && extraEnd != std::string::npos
		? commit.substr(extraBegin, extraEnd + std::strlen("extraTlas.push_back(extra);") - extraBegin)
		: std::string();
	Check(!extra.empty(), "R3-006 skinned TLAS-extra construction is anchored");
	Check(!extra.empty() &&
		extra.find("setTransform(nvrhi::rt::c_IdentityTransform)") != std::string::npos &&
		extra.find("setInstanceID(firstSkinned + si)") != std::string::npos &&
		extra.find("setInstanceMask(0x02)") != std::string::npos &&
		extra.find("setInstanceContributionToHitGroupIndex(PT_PATH_TRACE_SBT_SKINNED_INSTANCE_CONTRIBUTION)") != std::string::npos &&
		extra.find("setFlags(nvrhi::rt::InstanceFlags::TriangleCullDisable)") != std::string::npos &&
		extra.find("setBLAS(mesh.blas)") != std::string::npos,
		"R3-006 skinned TLAS extra uses identity while retaining ID/mask/SBT/cull/BLAS contract");
	Check(rewrite.find("void AffineFromObjectToWorld(") == std::string::npos &&
		commit.find("AffineFromObjectToWorld(js.currentObjectToWorld, transform)") == std::string::npos &&
		rewrite.find("#include <cstring>") == std::string::npos,
		"R3-006 removes the dead skinned-extra affine helper and include");
	Check(commit.find("RtCpuRewriteBuildAffineFromObjectToWorld(js.currentObjectToWorld, d.currentObjectToWorld)") != std::string::npos &&
		commit.find("RtCpuRewriteBuildAffineFromObjectToWorld(history.objectToWorld, d.previousObjectToWorld)") != std::string::npos &&
		commit.find("RtCpuRewriteCommittedPoseValid(history.rootFrame, overlay->rootFrame,") != std::string::npos &&
		commit.find("RtCpuRewriteSkinnedHistoryIdentityEqual(history.layout, row), service->LastCommittedRootFrame())") != std::string::npos,
		"R3-010 world-space dispatch uses exact current and coherent committed previous pose");
	const char* stages[] = { "PT CPU Commit Skinned Prepare", "PT CPU Commit Skinned Resolve",
		"PT CPU Commit Skinned Record", "PT CPU Commit Skinned Finalize" };
	std::size_t last = 0;
	bool ordered = commit.find("OPTICK_EVENT(\"PT CPU Commit Skinned\")") != std::string::npos;
	for (const char* stage : stages)
	{
		const std::size_t pos = commit.find(std::string("OPTICK_EVENT(\"") + stage + "\")");
		ordered = ordered && pos != std::string::npos && pos > last;
		last = pos;
	}
	Check(ordered, "R3-010 Prepare Resolve Record Finalize replace historical stage/return-count pins");
}

void TestR3008ARigidBoundaryAttributionSourceContract()
{
	const fs::path candidates[] = {
		fs::path(__FILE__).parent_path() / ".." / "renderer" / "NVRHI" /
			"PathTraceSmokeSceneBuild.cpp",
		fs::path("E:/prog/rbdoom-3-bfg-rt-producer/neo/renderer/NVRHI/PathTraceSmokeSceneBuild.cpp")
	};
	std::string source;
	for (const fs::path& candidate : candidates)
	{
		std::ifstream input(candidate);
		if (!input) continue;
		source = std::string{ std::istreambuf_iterator<char>(input),
			std::istreambuf_iterator<char>() };
		if (!source.empty()) break;
	}
	Check(!source.empty(), "R3-008A scene-build source loaded and is non-empty");
	if (source.empty()) return;

	const std::size_t functionBegin = source.find(
		"bool PathTracePrimaryPass::TryBuildCpuProducerRewriteScene(");
	const std::size_t functionEnd = source.find(
		"void PathTracePrimaryPass::BuildRayTracingSmokeTestScene", functionBegin);
	Check(functionBegin != std::string::npos && functionEnd != std::string::npos && functionBegin < functionEnd,
		"R3-008A complete rewrite scene-build function is anchored");
	if (functionBegin == std::string::npos || functionEnd == std::string::npos || functionBegin >= functionEnd) return;
	const std::string function = source.substr(functionBegin, functionEnd - functionBegin);
	const auto countToken = [](const std::string& text, const std::string& token)
	{
		std::size_t count = 0;
		for (std::size_t pos = text.find(token); pos != std::string::npos;
			pos = text.find(token, pos + 1)) ++count;
		return count;
	};

	const char* stageNames[] = { "Acquire", "Overlay", "Resolve", "Derive", "Upload", "Submit" };
	const char* scopeCountFields[] = {
		"rigidAcquireScopeCount", "rigidOverlayScopeCount", "rigidResolveScopeCount",
		"rigidDeriveScopeCount", "rigidUploadScopeCount", "rigidSubmitScopeCount"
	};
	const std::size_t expectedOccurrences[] = { 1, 3, 4, 4, 4, 7 };
	for (std::size_t i = 0; i < 6; ++i)
	{
		const std::string event = std::string("OPTICK_EVENT(\"PT CPU Commit Rigid ") + stageNames[i] + "\")";
		const std::string increment = std::string("++rigidAttribution.") + scopeCountFields[i];
        if (i == 2)
        {
            Check(countToken(function, event) == 1 &&
                countToken(function, "OPTICK_EVENT(\"PT CPU Rigid Resolve Apply\")") == 1 &&
                countToken(function, increment) == 2,
                "R4-023 owner resolution retains only prepared-index apply and candidate copy scopes");
            continue;
        }
        const size_t earlyCount = i < 2 ? 1 : 0;
        const bool earlyCounter = i == 0 ? function.find("rigidAttribution.rigidAcquireScopeCount = 1") != std::string::npos :
            i == 1 ? function.find("rigidAttribution.rigidOverlayScopeCount = overlayScopeCount") != std::string::npos &&
                countToken(function, "++overlayScopeCount") == 1 : true;
        Check(countToken(function, event) == expectedOccurrences[i] && earlyCounter &&
            countToken(function, increment) + earlyCount == expectedOccurrences[i],
            (std::string("R4-008 ") + stageNames[i] + " stage records early preflight and GPU tail occurrences").c_str());
	}

	const char* requiredTags[] = {
		"rigidOutcome", "rigidJoinedCount", "rigidRetainedHitCount", "rigidPackedFillSkipped",
		"rigidRoutePhysicalBufferCreateCount", "rigidBindingCreateCount", "rigidUploadBytes",
		"rigidColdBlasCount", "rigidBlasUpdateCount", "rigidTlasCreateCount", "rigidTlasBuildCount",
		"rigidMeshMissCount", "staticResizeCreateCount", "staticUploadCount", "packedUploadCount",
		"rigidWorkerProductConsumed", "rigidProductGeneration", "rigidProductAge",
		"rigidAcquireScopeCount", "rigidOverlayScopeCount", "rigidResolveScopeCount",
		"rigidDeriveScopeCount", "rigidUploadScopeCount", "rigidSubmitScopeCount"
	};
	bool completeTagSet = function.find("struct RigidCommitAttribution") != std::string::npos &&
		function.find("struct RigidCommitAttributionReporter") != std::string::npos;
	for (const char* tag : requiredTags)
	{
		completeTagSet = completeTagSet &&
			countToken(function, std::string("OPTICK_TAG(\"") + tag + "\"") == 1;
	}
	Check(completeTagSet, "R3-008A complete primitive local attribution tag set is emitted once");

	Check(countToken(function, "OPTICK_EVENT(\"PT CPU GPU Geometry Commit\")") == 1,
		"R3-008A unchanged outer geometry commit event remains exact");
	Check(countToken(function, "OPTICK_EVENT(transformOnly ? \"PT CPU Commit Transform Only\" : \"PT CPU Commit Full\")") == 1 &&
		countToken(function, "OPTICK_EVENT(\"PT CPU Commit Static BLAS\")") == 1 &&
		countToken(function, "OPTICK_EVENT(\"PT CPU Mesh GPU Hit\")") == 1 &&
		countToken(function, "OPTICK_EVENT(\"PT CPU Mesh GPU Miss\")") == 1 &&
		countToken(function, "OPTICK_EVENT(\"PT CPU Commit TLAS Create\")") == 1 &&
		countToken(function, "OPTICK_EVENT(\"PT CPU Commit Packed Upload\")") == 1 &&
		countToken(function, "OPTICK_EVENT(\"PT CPU Commit Binding\")") == 1,
		"R3-008A all seven existing child-event anchors remain exact");
	Check(countToken(function, "CommitRewriteSkinnedGpuSkinAndHitRoute(") == 2,
		"R3-008A both skinned commit call sites remain exact");
    Check(function.find("~OverlayFrameCleanup() { service->ReleaseConsumedOverlay(); service->DiscardOverlaysThrough(rootFrame); }") != std::string::npos &&
        function.find("overlayFrameCleanup { service, viewDef->pathTraceRewriteRootFrame }") < function.find("r_pathTracingUnifiedPtFrozenScene") &&
        countToken(function, "service->ReleaseConsumedProduct(true)") == 3,
        "R4-008 every scene exit releases consuming and stale overlays, with retained product release preserved");
    Check(function.find("TryAcquireExactProduct(viewRootFrame, worldGen, mapGen, 8)") < function.find("PT CPU Material Evaluate") &&
        function.find("overlayAcquired && dependencyReason &&") != std::string::npos &&
        function.find("view->staticContentSignature") != std::string::npos &&
        function.find("if (view->StaticSources()[i].sourceClass != kRtCpuRewriteClassStaticWorld) continue;") != std::string::npos &&
        function.find("RtCpuRewriteFitsResidentRigidBudget(newRetainBytes, packedBytes)") != std::string::npos &&
        function.find("!retainUnreferencedDedicated && !rec.referenced") != std::string::npos,
        "R4-008 changed-frame dependency precedes evaluation and GPU work; static receipts and capped retention are independent");
    Check(function.find("FinishRewriteLighting(device") < function.find("CreateSmokeBindingResources(bindingBuildDesc") &&
        function.find("buffers.emissiveTriangleBuffer = lighting.inputs.emissiveTriangleBuffer") != std::string::npos &&
        function.find("buffers.doomAnalyticLightBuffer = lighting.inputs.doomAnalyticLightBuffer") != std::string::npos &&
        function.find("buffers.restirLightManagerCurrentToPreviousBuffer = lighting.gpu.buffers[18]") != std::string::npos &&
        function.find("sceneInputs.lights = lighting.inputs") != std::string::npos,
        "R4-010 live light handles replace frozen bindings and scene inputs before commit");
    Check(function.find("CommitRayTracingSmokeSceneResources(resourceCommitDesc)") <
            function.find("m_rewritePreviousEmissives = std::move(lighting.emissives)") &&
        function.find("m_remixLightManager.ApplyPrepareResult(std::move(lighting.manager))") != std::string::npos &&
        function.find("m_rewritePreviousEmissives.clear()") != std::string::npos &&
        function.find("for (auto& slot : m_rewriteLightGpuSlots) slot = RtCpuRewriteLightGpuSlot()") != std::string::npos,
        "R4-010 committed light history and idle-slot buffers publish atomically and drain");
    const auto lightsBegin = source.find("struct PathTracePrimaryPass::RtCpuRewriteLightWork");
    const std::string lights = source.substr(lightsBegin, functionBegin - lightsBegin);
    Check(lights.find("if (!emissive(instance.materialIndex)) continue") < lights.find("RtCpuRewriteEvaluateEmissiveVertex") &&
        lights.find("m_rewriteLightHistoryValid = false") < lights.find("PublishPathTraceDoomAnalyticLightsFromCollection") &&
        lights.find(".BuildPrepareResult(remixLightPrepareDesc)") != std::string::npos &&
        lights.find("128ull * 1024 * 1024 - capacityBytes") != std::string::npos &&
        lights.find("emissiveSourceToDense[entry.emissiveTriangleIndex]") != std::string::npos,
        "R4-010 sparse emissive admission, failed-attempt history, bounded resources and dense CDF mapping");
    Check(function.find("service->SubmitMaterialRecords(viewRootFrame") < function.find("BeginRewriteLighting(viewDef") &&
        function.find("BeginRewriteLighting(viewDef") < function.find("service->FinishMaterialRecords(materialRecordJob,viewRootFrame)") &&
        function.find("service->FinishMaterialRecords(materialRecordJob,viewRootFrame)") < function.find("nvrhi::BufferHandle materialBuffers[4]") &&
        function.find("frameDynamicMaterials=std::move(materialRecordWork->records)") != std::string::npos &&
        function.find("BuildSmokeDynamicMaterialRecords(") == std::string::npos,
        "R4-021 record worker is authoritative and overlaps lighting before GPU material writes");
    const auto snapshotBegin = lights.find("bool PathTracePrimaryPass::BeginRewriteLighting(");
    const auto finishBegin = lights.find("bool PathTracePrimaryPass::FinishRewriteLighting(");
    const auto analyticBegin = lights.find("bool PathTracePrimaryPass::PrepareRewriteAnalyticLighting(");
    const std::string numericLights = lights.substr(0, analyticBegin);
    const auto managerSubmit = numericLights.find("service.SubmitLightManagerPreparation(rootFrame, charge,");
    const auto unifiedBuild = numericLights.find("unifiedLights = [&]()", managerSubmit);
    const auto managerJoin = numericLights.find("const bool managerReady = service.FinishLightManagerPreparation", unifiedBuild);
    const auto denseUse = numericLights.find("manager.currentLightPayloads", managerJoin);
    Check(managerSubmit != std::string::npos && unifiedBuild != std::string::npos &&
        managerJoin != std::string::npos && denseUse != std::string::npos &&
        managerSubmit < unifiedBuild && unifiedBuild < managerJoin && managerJoin < denseUse &&
        numericLights.find("~ManagerJoinGuard() { if (!joined) service.FinishLightManagerPreparation(job, root); }") != std::string::npos &&
        countToken(numericLights,"managerBase.BuildPrepareResult(") == 1 &&
        numericLights.find("[owned] { owned->PrepareManager(); return true; }") != std::string::npos,
        "R4-027 manager and unified producers fork before joining for dense consumers; exceptions drain child and no manager replay");
    Check(numericLights.find("nvrhi::") == std::string::npos && numericLights.find("r_pathTracing") == std::string::npos &&
        numericLights.find("ResolveSmokeMaterialTextureInfo") == std::string::npos &&
        numericLights.find("maxRecords, &facts, &emissiveInventoryStats, uniformMixture)") != std::string::npos &&
        lights.find("service.SubmitLightPreparation(frameIndex, charge,") != std::string::npos &&
        lights.find("return owned->Prepare(*servicePtr, frameIndex, charge, owned);") != std::string::npos &&
        function.find("BeginRewriteLighting(viewDef") < function.find("nvrhi::BufferHandle materialBuffers[4]") &&
        function.find("nvrhi::BufferHandle materialBuffers[4]") < function.find("FinishRewriteLighting(device") &&
        lights.find("service.FinishLightPreparation(work->job, rootFrame)", finishBegin) < lights.find("commandList->writeBuffer", finishBegin) &&
        lights.substr(snapshotBegin).find("BuildSmokeCanonicalSkinnedEmissiveAuditInventory(") == std::string::npos &&
        lights.substr(snapshotBegin).find("BuildPathTraceUnifiedLights(") == std::string::npos,
        "R4-020 authoritative owned numeric lighting executes only in worker with independent owner work before join");
    Check(lights.find("BuildPathTraceDoomAnalyticLightCandidates(") == std::string::npos &&
        lights.find("overlay->rootFrame == viewDef->pathTraceRewriteRootFrame") != std::string::npos &&
        lights.find("if (!exactLightInput) return false;") < lights.find("PublishPathTraceDoomAnalyticLightsFromCollection"),
        "R4-011 live light collection replaced by mandatory matching-frame snapshot");
    Check(function.find("const bool rewriteHistoryReset = route == RtCpuProducerRewriteRoute::RewriteWarmup;") != std::string::npos &&
        function.find("if (rewriteHistoryReset) m_frameResources.MarkResetReason(RT_FRAME_RESET_SCENE_RESOURCES);") != std::string::npos,
        "R4-011 ordinary rewrite commits preserve RR history and first handoff resets");
    Check(countToken(function, "viewDef->pathTraceRewriteSurfaces") == 0 &&
        countToken(function, "CaptureSmokeRewriteMaterialMembership(viewDef, materialMembership)") == 1 &&
        function.find("lightWork); }, materialMembership, commandList)") != std::string::npos &&
        function.find("materialFrame.Find(") != std::string::npos &&
        function.find("viewDef->drawSurfs[i]") == std::string::npos &&
        function.find("ds->space->entityDef->index") == std::string::npos,
        "R4-035 shared frame membership feeds compatibility and material capture with prepared worker lookup");
    {
        const fs::path renderer = fs::path(__FILE__).parent_path() / ".." / "renderer";
        auto read = [&](const fs::path& path) {
            std::ifstream input(path);
            return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
        };
        const std::string producer = read(renderer / "NVRHI" / "PathTraceCpuProducerRewrite.cpp");
        const std::string capture = read(renderer / "NVRHI" / "PathTraceSceneCapture.cpp");
        const auto materialSubmit = capture.find("service.SubmitMaterials(std::move(input))");
        const auto bindingCapture = capture.find("SnapshotSmokeMaterialBindingFrame(bindingWork->input", materialSubmit);
        const auto bindingNeed = capture.find("const bool prepareBindings=SmokeMaterialBindingNeedsPreparation(*bindings);",bindingCapture);
        const auto bindingBranch = capture.find("if (prepareBindings) {",bindingNeed);
        const auto bindingPublish = capture.find("service.SubmitMaterialBindingPreparation(", bindingCapture);
        const auto bindingResources = capture.find("ResolveSmokeMaterialBindingResources(*bindings, commandList)",bindingPublish);
        const auto materialJoin = capture.find("const bool numericComplete=service.FinishMaterials(job)", bindingPublish);
        const auto guarded = capture.find("} jobsGuard{service,job,bindingJob,viewDef->pathTraceRewriteRootFrame};", materialSubmit);
        const auto earlyInput = capture.find("if (!prepareOwnerLightInput || !prepareOwnerLightInput()) return false;",guarded);
        const std::string analytic = lights.substr(analyticBegin,snapshotBegin-analyticBegin);
        const std::string lateInput = lights.substr(snapshotBegin,finishBegin-snapshotBegin);
        Check(guarded != std::string::npos && earlyInput != std::string::npos &&
            materialSubmit < guarded && guarded < earlyInput && earlyInput < bindingCapture && earlyInput < materialJoin &&
            function.find("return PrepareRewriteAnalyticLighting(viewDef, *view, service->OverlayView(),") != std::string::npos &&
            analytic.find("m_rewriteLightHistoryValid = false;") < analytic.find("PublishPathTraceDoomAnalyticLightsFromCollection") &&
            analytic.find("work->analyticReady = true;") > analytic.find("work->frame.BeginFrame(frameDesc)") &&
            lateInput.find("!work->analyticReady") != std::string::npos &&
            lateInput.find("work->analyticRoot != frameIndex") != std::string::npos &&
            lateInput.find("work->analyticWorld != product.worldGeneration") != std::string::npos &&
            lateInput.find("work->analyticMap != product.mapGeneration") != std::string::npos &&
            lateInput.find("work->analyticReady = false;") < lateInput.find("service.SubmitLightPreparation(") &&
            lateInput.find("PublishPathTraceDoomAnalyticLightsFromCollection") == std::string::npos &&
            lateInput.find("work->managerBase = m_remixLightManager") == std::string::npos,
            "R4-032 owner analytic input overlaps numeric job under drain guard and is consumed once with exact provenance");
        const auto samplesApply = capture.find("PT CPU Material Emissive Samples Apply",materialJoin);
        const auto bindingJoin = capture.find("const bool bindingComplete=!prepareBindings || service.FinishMaterialBindingPreparation(",samplesApply);
        const auto bindingApply = capture.find("CompleteSmokeMaterialBindingFrame(*bindings,std::move(bindingWork->output))",bindingJoin);
        Check(producer.find("std::thread(&RtCpuProducerRewriteService::ShadingWorkerMain, this)") != std::string::npos &&
            producer.find("job->output[i] = BuildPathTraceRuntimeMaterialEvalFromPod") != std::string::npos &&
            producer.find("job->cv.wait(lock, [&] { return job->done; })") != std::string::npos &&
            materialSubmit != std::string::npos && bindingCapture != std::string::npos &&
            bindingPublish != std::string::npos && materialJoin != std::string::npos &&
            materialSubmit < bindingCapture && bindingCapture < bindingPublish && bindingPublish < materialJoin &&
            bindingNeed != std::string::npos && bindingBranch != std::string::npos &&
            bindingCapture < bindingNeed && bindingNeed < bindingBranch && bindingBranch < bindingPublish &&
            bindingResources != std::string::npos && bindingPublish < bindingResources && bindingResources < materialJoin &&
            samplesApply != std::string::npos && bindingJoin != std::string::npos && bindingApply != std::string::npos &&
            samplesApply < bindingJoin && bindingJoin < bindingApply &&
            capture.find("BuildRtCpuMaterialBindingPlan(owned->input,owned->output)",bindingPublish) != std::string::npos &&
            capture.find("samples.resize(job->frame.emissiveSampleOrdinals.size())") != std::string::npos &&
            capture.find("SmokeMaterialSampleFromEvaluation(surfaces[ordinal]->material,") != std::string::npos,
            "R4-031 production skips binding job on definition reuse and preserves cold preparation ordering");
        const auto begin = producer.find("void RtCpuProducerRewrite_OnRootViewBegin(");
        const auto end = producer.find("void RtCpuProducerRewrite_OnRootViewSealOrAbort(", begin);
        const std::string admission = begin != std::string::npos && end != std::string::npos
            ? producer.substr(begin, end - begin) : std::string();
        const std::string models = read(renderer / "tr_frontend_addmodels.cpp");
        Check(!admission.empty() && admission.find("PrepareResidentEntities(parms)") != std::string::npos &&
            producer.find("cached.refresh = !cached.initialized || ((inUpdateRegion || exact)") != std::string::npos &&
            producer.find("CaptureRetainedEntities(parms)") != std::string::npos &&
            producer.find("!vEntity->entityDef || !vEntity->pathTraceResident") != std::string::npos &&
            models.find("vEntity->pathTraceResident && shader->IsDrawn()") != std::string::npos &&
            models.find("vEntity->pathTraceResident && surfaceDirectlyVisible") == std::string::npos,
            "R4-015 portal region selects pose refresh while retained entities supply full membership");
        Check(producer.find("if (write.geometry) return WriteSurfaceReference(write);") != std::string::npos &&
            producer.find("slot.geometryOwners.clear(); // includes reclaimed queued inputs") != std::string::npos &&
            producer.find("PT CPU Resident Geometry Product Reuse") < producer.find("PT CPU Worker Skinned Prepare") &&
            producer.find("product.view.residentDynamic = m_residentDynamic") != std::string::npos &&
            producer.find("write.geometry = retained ? retained->write.geometry : g_service->RetainGeometry(write)") != std::string::npos,
            "R4-015 production immutable references and complete product reuse bypass geometry replay");
        const auto staticBegin = producer.find("bool CaptureResidentWorld(viewDef_t* parms)");
        const auto staticEnd = producer.find("void RtCpuProducerRewrite_OnRootViewBegin(", staticBegin);
        const std::string staticCapture = producer.substr(staticBegin, staticEnd - staticBegin);
        Check(staticCapture.find("capture.pathTraceResident = true;") <
                staticCapture.find("RtCpuProducerRewrite_CaptureAdmittedModel(&capture, cached.model)") &&
            producer.find("!vEntity->entityDef || !vEntity->pathTraceResident") != std::string::npos,
            "R4-017 direct static population satisfies capture admission while ordinary raster visits remain excluded");
        Check(producer.find("if (populate)\n            {\n                auto* tri = cached.model->Surface") != std::string::npos &&
            producer.find("product.view.residentStatic = m_residentStatic") != std::string::npos &&
            function.find("view->staticRevision != service->OverlayView()->staticRevision") != std::string::npos &&
            function.find("staticSurfaceMaterialSignatures[i] == m_rewriteStaticSurfaceMaterialSignatures[i]) continue") != std::string::npos &&
            function.find("if (rebuildStatic)\n            {\n                staticMaterialIds.resize") != std::string::npos &&
            function.find("CommitRayTracingSmokeSceneResources(resourceCommitDesc)") <
                function.find("m_rewriteStaticSurfaceMaterialSignatures = std::move(staticSurfaceMaterialSignatures)"),
            "R4-013 resident source authority and sparse static material patches advance only at commit");
    }
	Check(function.find("m_rewriteMaterialTableOwner == m_sceneInputs.materials.materialTableBuffer") != std::string::npos &&
		function.find("m_rewriteMaterialWorld == worldGen && m_rewriteMaterialMap == mapGen") != std::string::npos &&
		function.find("staticMaterialIndexes.data(), \"rewrite matidx\"") != std::string::npos &&
		function.find("packedTriMatIndex.push_back(inst.route.materialIndex)") != std::string::npos &&
		function.find("inst.route.materialId != last.materialId || inst.route.materialIndex != last.materialIndex") != std::string::npos &&
		function.find("materialLateChangeRejected") != std::string::npos &&
		function.find("BuildSmokeMaterialTable") == std::string::npos &&
		function.find("if (materialGrowth)") != std::string::npos &&
		function.find("AppendSmokeResidentMaterialRows(grownMaterialTable") != std::string::npos &&
		function.find("bindingBuildDesc.allowExistingTextureDescriptorTableWrites = false") != std::string::npos &&
		function.find("m_rewriteLastMaterialTable = std::move(authoredMaterialCandidate)") != std::string::npos,
		"R4-003 authored table authority and candidate growth are live without full table rebuilding");
    Check(function.find("RtCpuRewriteBuildRigidAttributePatch(view->rigidMeshes[inst.meshIndex]") <
            function.find("nvrhi::BufferHandle materialBuffers[4]") &&
        function.find("patch.vertices.data(), vertexBytes, size_t(patch.vertexOffset)") != std::string::npos &&
        function.find("m_rewritePackedRouteTriMatIndexBuffer, patch.materialIndexes.data()") != std::string::npos &&
        function.find("if (!skipPackedGeometryFill) m_rewriteLastPackedLayout.clear();") != std::string::npos &&
        function.find("CommitRayTracingSmokeSceneResources(resourceCommitDesc)") <
            function.find("m_rewriteLastPackedLayout.resize(join.joinedCount)"),
        "R4-018 exact attribute patches stage before GPU work and layout proof publishes only after scene commit");
    Check(function.find("BuildSmokeRewriteMaterialSamples(viewDef, *service, materialSamples, materialFrame, materialBindingsFrame,") != std::string::npos &&
        function.find("BuildSmokeDynamicMaterialEvalSampleForDrawSurf(ds, id, sample)") == std::string::npos &&
        function.find("SmokeRuntimeMaterialTableIdForDrawSurf(ds, baseId)") == std::string::npos &&
        function.find("SnapshotSmokeMaterialRecords(grownMaterialTable,{},viewDef)") != std::string::npos &&
        function.find("materialRecordWork->input.samples=std::move(materialFrame.recordSamples)") != std::string::npos &&
        function.find("frameMaterialStats.dynamicEvalMaterialSamples=std::move(materialSamples)") != std::string::npos &&
        function.find("BuildSmokeSurfaceTextureMatrices(ds") == std::string::npos &&
        function.find("materialFrame.Find(key.renderDefIndex, key.modelSurfaceIndex, baseId)") != std::string::npos &&
        function.find("geometryDependencyResult") != std::string::npos &&
        function.find("ProcessSmokeCrosshairZeroRoughnessToggle(viewDef)") != std::string::npos &&
        function.find("ProcessSmokeCrosshairFullMetalToggle(viewDef)") != std::string::npos,
        "R4-022 worker replaces evaluation, identity and matrices while coherent rejection and PT commands stay live");
    Check(function.find("service->SubmitRigidPreparation(viewRootFrame") < function.find("PT CPU Material Evaluate") &&
        function.find("service->FinishRigidPreparation(rigidResolveJob, viewRootFrame)") <
            function.find("currentBlasTokens.reserve(join.joinedCount)") &&
        function.find("service->FinishRigidPreparation(rigidResolveJob, viewRootFrame)") <
            function.find("service->SubmitMaterialRecords(viewRootFrame") &&
        function.find("const int32_t retained = rigidResolve->rows[ji].early;") != std::string::npos &&
        function.find("const auto& resolved = rigidResolve->rows[ji];") != std::string::npos &&
        function.find("const int retainIndex = rigidResolve->rows[ji].candidate;") != std::string::npos &&
        function.find("rec.sourceAssetId == inst.meshKey.sourceAssetId") == std::string::npos &&
        function.find("rec.sourceAssetId == mesh.meshKey.sourceAssetId") == std::string::npos &&
        function.find("rigidResolveOwnerSearchesSkipped") != std::string::npos,
        "R4-023 production dispatch precedes materials and prepared indices replace both owner searches");
    Check(function.find("RefreshSmokeMaterialTextureHandlesForActiveIds(activeMaterialIds, noIds, materialBindingsFrame.get())") != std::string::npos,
        "R4-024 registry refresh consumes the material worker binding frame");
    Check(function.find("materialRowSignatures = m_rewriteMaterialRowSignatures") != std::string::npos &&
        function.find("m_rewriteMaterialRowSignatures = std::move(materialRowSignatures)") >
            function.find("CommitRayTracingSmokeSceneResources(resourceCommitDesc)"),
        "R4-004 source signatures publish only with the accepted material scene");
	Check(function.find("m_rigidAttribution") == std::string::npos &&
		function.find("static RigidCommitAttribution") == std::string::npos &&
		function.find("idCVar rigid") == std::string::npos,
		"R3-008A attribution introduces no persistent member or CVar state");
}

void TestR3009NarrowRouteBufferResolverSourceContract()
{
	const auto readSource = [](const char* name)
	{
		const fs::path candidates[] = {
			fs::path(__FILE__).parent_path() / ".." / "renderer" / "NVRHI" / name,
			fs::path("E:/prog/rbdoom-3-bfg-rt-producer/neo/renderer/NVRHI") / name
		};
		for (const fs::path& candidate : candidates)
		{
			std::ifstream input(candidate);
			if (!input) continue;
			std::string text{ std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>() };
			if (!text.empty()) return text;
		}
		return std::string();
	};
	const auto compact = [](const std::string& text)
	{
		std::string result;
		for (char c : text)
			if (c != ' ' && c != '\t' && c != '\r' && c != '\n') result += c;
		return result;
	};
	const auto countToken = [](const std::string& text, const std::string& token)
	{
		std::size_t count = 0;
		for (std::size_t pos = text.find(token); pos != std::string::npos;
			pos = text.find(token, pos + token.size())) ++count;
		return count;
	};
	const auto functionBody = [](const std::string& text, const char* signature)
	{
		const std::size_t signaturePos = text.find(signature);
		const std::size_t begin = signaturePos == std::string::npos
			? std::string::npos : text.find('{', signaturePos);
		if (begin == std::string::npos) return std::string();
		int depth = 0;
		for (std::size_t pos = begin; pos < text.size(); ++pos)
		{
			if (text[pos] == '{') ++depth;
			if (text[pos] == '}' && --depth == 0) return text.substr(begin, pos + 1 - begin);
		}
		return std::string();
	};
	const std::string header = readSource("PathTraceSmokeResources.h");
	const std::string resources = readSource("PathTraceSmokeResources.cpp");
	const std::string scene = readSource("PathTraceSmokeSceneBuild.cpp");
	Check(!header.empty() && !resources.empty() && !scene.empty(),
		"R3-009 separately loads non-empty resource header, resource source, and scene source");
	if (header.empty() || resources.empty() || scene.empty()) return;
	const std::string broad = functionBody(resources, "RtSmokeSceneBufferCreateResult CreateSmokeSceneBuffers(");
	const std::string function = functionBody(scene, "bool PathTracePrimaryPass::TryBuildCpuProducerRewriteScene(");
	Check(!broad.empty() && !function.empty(), "R3-009 existing factory and production function anchors loaded");
	if (broad.empty() || function.empty()) return;
	const char* signature = "RtSmokeRigidRouteBufferResolveResult ResolveOrCreateSmokeRigidRouteBuffers(";
	const std::string helper = compact(functionBody(resources, signature));
	const std::string production = compact(function);
	Check(header.find(signature) != std::string::npos && !helper.empty() &&
		header.find("struct RtSmokeRigidRouteBufferResolveDesc") != std::string::npos &&
		header.find("struct RtSmokeRigidRouteBufferResolveResult") != std::string::npos,
		"R3-009 helper declaration and independently anchored definition exist");

	const char* fields[] = { "rigidRouteVertexBuffer", "rigidRouteIndexBuffer",
		"rigidRouteTriangleMaterialBuffer", "rigidRouteTriangleMaterialIndexBuffer", "rigidRouteInstanceBuffer" };
	const char* bytes[] = { "rigidRouteVertexBytes", "rigidRouteIndexBytes",
		"rigidRouteTriangleMaterialBytes", "rigidRouteTriangleMaterialIndexBytes", "rigidRouteInstanceBytes" };
	const char* names[] = { "PathTraceRigidRouteVertices", "PathTraceRigidRouteIndices",
		"PathTraceRigidRouteTriangleMaterials", "PathTraceRigidRouteTriangleMaterialIndexes", "PathTraceRigidRouteInstances" };
	const char* strides[] = { "PathTraceSmokeVertex", "uint32_t", "uint32_t", "uint32_t", "PathTraceRigidRouteInstance" };
	const char* members[] = { "m_rewritePackedRouteVertexBuffer", "m_rewritePackedRouteIndexBuffer",
		"m_rewritePackedRouteTriMatBuffer", "m_rewritePackedRouteTriMatIndexBuffer", "m_rewritePackedRouteInstanceBuffer" };
	const char* committed[] = { "m_rewriteLastCommittedPackedVertexBuffer", "m_rewriteLastCommittedPackedIndexBuffer",
		"m_rewriteLastCommittedPackedTriMatBuffer", "m_rewriteLastCommittedPackedTriMatIndexBuffer", "m_rewriteLastCommittedPackedInstanceBuffer" };
	bool descriptors = countToken(helper, "ReuseOrCreateSmokeGeometryBuffer(") == 5;
	bool physicalCount = countToken(helper, "result.rigidRoutePhysicalBufferCreateCount+=") == 5;
	bool threading = true;
	std::size_t lastCall = 0;
	for (std::size_t i = 0; i < 5; ++i)
	{
		const std::string field = fields[i];
		const std::string call = "result.buffers." + field + "=ReuseOrCreateSmokeGeometryBuffer(desc.device,desc.existingBuffers." +
			field + ",\"" + names[i] + "\",desc." + bytes[i] + ",sizeof(" + strides[i] + "),false,false,false);";
		const std::size_t pos = helper.find(call);
		descriptors = descriptors && pos != std::string::npos && (i == 0 || pos > lastCall) &&
			compact(broad).find(call.substr(0, call.size() - 2) + ",false,false,&result.physicalBufferCreateCount);") != std::string::npos && countToken(helper, std::string("\"") + names[i] + "\"") == 1;
		lastCall = pos;
		physicalCount = physicalCount && helper.find("result.rigidRoutePhysicalBufferCreateCount+=result.buffers." + field +
			"&&result.buffers." + field + "!=desc.existingBuffers." + field + ";") != std::string::npos;
		threading = threading && production.find(std::string(members[i]) + "=routeCreated.buffers." + field + ";") != std::string::npos &&
			production.find("routeCreated.buffers." + field + "!=" + committed[i]) != std::string::npos;
	}
	const char* forbidden[] = { "CreateSmokeSceneBuffers(", "createBuffer(", "staticVertex", "dynamicVertex", "previousStatic",
		"materialTable", "materialFeature", "emissive", "Emissive", "lightCandidate", "Doom", "Restir", "Unified", "skinned", "Skinned", "m_rewrite" };
	for (const char* token : forbidden) descriptors = descriptors && helper.find(token) == std::string::npos;
	Check(descriptors, "R3-009 helper isolates exactly five ordered descriptor-equivalent route resolves");
	const std::string input = "constRtSmokeRigidRouteBufferResolveDescrouteResolveDesc={routeDesc.device,{routeDesc.existingBuffers.rigidRouteVertexBuffer,"
		"routeDesc.existingBuffers.rigidRouteIndexBuffer,routeDesc.existingBuffers.rigidRouteTriangleMaterialBuffer,"
		"routeDesc.existingBuffers.rigidRouteTriangleMaterialIndexBuffer,routeDesc.existingBuffers.rigidRouteInstanceBuffer},"
		"routeDesc.rigidRouteVertexBytes,routeDesc.rigidRouteIndexBytes,routeDesc.rigidRouteTriangleMaterialBytes,"
		"routeDesc.rigidRouteTriangleMaterialIndexBytes,routeDesc.rigidRouteInstanceBytes};";
	Check(threading && production.find(input) != std::string::npos &&
		countToken(production, "ResolveOrCreateSmokeRigidRouteBuffers(routeResolveDesc)") == 1 &&
		production.find("CreateSmokeSceneBuffers(routeDesc)") == std::string::npos &&
		production.find("RtSmokeSceneBufferCreateDescrouteDesc={};") != std::string::npos &&
		production.find("if(!routeCreated.buffers.rigidRouteVertexBuffer||!routeCreated.buffers.rigidRouteInstanceBuffer)") != std::string::npos &&
		countToken(production, compact("\"rewrite route instances\"")) == 1 && countToken(production, "routeUploads[uploadCount++]") == 5,
		"R3-009 production substitutes narrow resolver and preserves handle, byte, guard and upload wiring");
	const std::string resolveEvent = compact("OPTICK_EVENT(\"PT CPU Commit Rigid Route Resolve\");");
	const std::size_t resolvePos = production.find(resolveEvent);
	const std::size_t uploadPos = production.rfind(compact("OPTICK_EVENT(\"PT CPU Commit Rigid Upload\");"), resolvePos);
	Check(physicalCount && production.find("rigidBufferCreateCount") == std::string::npos &&
		countToken(production, "OPTICK_TAG(\"rigidRoutePhysicalBufferCreateCount\",value.rigidRoutePhysicalBufferCreateCount)") == 1 &&
		countToken(production, "rigidAttribution.rigidRoutePhysicalBufferCreateCount+=routeCreated.rigidRoutePhysicalBufferCreateCount;") == 1 &&
		countToken(production, resolveEvent) == 1 && uploadPos != std::string::npos && uploadPos < resolvePos &&
		production.find("{" + resolveEvent + "routeCreated=ResolveOrCreateSmokeRigidRouteBuffers(routeResolveDesc);}") != std::string::npos,
		"R3-009 physical creation telemetry and single call-only nested event are exact");
}

void TestR3010SkinnedTransactionSourceContract()
{
    const auto read = [](const char* name) {
        std::ifstream input(fs::path(__FILE__).parent_path() / ".." / "renderer" / "NVRHI" / name);
        return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    };
    const std::string skin = read("PathTraceCpuProducerRewriteSkinned.cpp");
    const std::string scene = read("PathTraceSmokeSceneBuild.cpp");
    const std::string resources = read("PathTraceSmokeResources.cpp");
    Check(!skin.empty() && !scene.empty() && !resources.empty(), "R3-010 three production sources independently loaded");
    if (skin.empty() || scene.empty() || resources.empty()) return;
    const auto prepare = skin.find("OPTICK_EVENT(\"PT CPU Commit Skinned Prepare\")");
    const auto resolve = skin.find("OPTICK_EVENT(\"PT CPU Commit Skinned Resolve\")");
    const auto record = skin.find("OPTICK_EVENT(\"PT CPU Commit Skinned Record\")");
    const auto finalize = skin.find("void PathTracePrimaryPass::FinalizeRewriteSkinnedTransaction(");
    Check(prepare < resolve && resolve < record && record < finalize && finalize != std::string::npos,
        "R3-010 transaction stages anchored and ordered");
    if (!(prepare < resolve && resolve < record && record < finalize && finalize != std::string::npos)) return;
    const auto before = skin.substr(prepare, record - prepare);
    const auto recordEnd = skin.find("    catch (const std::bad_alloc&)", record);
    Check(recordEnd != std::string::npos && recordEnd < finalize &&
        skin.find("if (tx.recorded) throw;", recordEnd) < finalize,
        "R3-010 post-record API allocation errors cannot become a recoverable rejection");
    const auto commands = skin.substr(record, recordEnd - record);
    const auto worker = read("PathTraceCpuProducerRewrite.cpp");
    Check(skin.find("RtCpuRewriteValidateSkinnedPrepared(") < prepare &&
        skin.find("RtCpuRewriteBuildSkinnedRoutePackage(") == std::string::npos &&
        skin.find("layout.vertices.push_back") == std::string::npos &&
        skin.find("layout.blasIndexes.push_back") == std::string::npos &&
        worker.find("mesh.indexes[ii] >= mesh.vertexCount") != std::string::npos &&
        worker.find("RtCpuRewriteBuildSkinnedRoutePackage(inputs, 0, routes, true)") != std::string::npos &&
        before.find("RtCpuRewritePatchSkinnedRouteRecord(") != std::string::npos &&
        before.find("ValidateSmokeTriangleGeometryBufferRanges(") != std::string::npos &&
        before.find("PreflightRewriteGpuGeometryBatch(") != std::string::npos &&
        before.find("commandList->") == std::string::npos,
        "R4-005 authoritative worker route/index construction replaces main replay; compatibility, descriptors and retirement precede commands");
    Check(commands.find("createBuffer(") == std::string::npos && commands.find("createBindingSet(") == std::string::npos &&
        commands.find("CreateSmokeBlas(") == std::string::npos && commands.find("return reject(") == std::string::npos &&
        commands.find("PerformUpdate") != std::string::npos && commands.find("if (tx.replace)") != std::string::npos,
        "R3-010 recording has no object creation or data rejection and gates stable writes");
    const auto sceneStart = scene.find("bool PathTracePrimaryPass::TryBuildCpuProducerRewriteScene(");
    const auto sceneCommit = scene.find("CommitRayTracingSmokeSceneResources(resourceCommitDesc)", sceneStart);
    const auto sceneFinalize = scene.find("FinalizeRewriteSkinnedTransaction(skinnedTransaction)", sceneStart);
    const auto staticCreate = scene.find("RtSmokeBlasCreateResult blas = {};", sceneStart);
    const auto staticPublish = scene.find("m_rewriteStaticBlas = blas.accelStruct;", staticCreate);
    const auto staticBuffersPublish = scene.find("m_rewriteStaticDynamic = dyn;", staticCreate);
    const auto staticDescPublish = scene.find("m_rewriteStaticBlasDesc =", staticCreate);
    const auto staticRetire = scene.find("EnqueuePreflightedRewriteGpuGeometryBatch(staticRetirementBatch)", staticCreate);
    const auto staticPreflight = scene.find("mandatoryRetirement.insert(mandatoryRetirement.end(), staticRetirementBatch.begin(), staticRetirementBatch.end())", staticCreate);
    const auto accelerationSubmit = scene.find("SubmitSmokeAccelerationBuilds(submitDesc, timing)", staticCreate);
    Check(staticCreate != std::string::npos && sceneCommit != std::string::npos &&
        staticPublish != std::string::npos && staticPublish > sceneCommit &&
        staticBuffersPublish != std::string::npos && staticBuffersPublish > sceneCommit &&
        staticDescPublish != std::string::npos && staticDescPublish > sceneCommit &&
        staticRetire != std::string::npos && staticRetire > sceneCommit &&
        staticPreflight != std::string::npos && staticPreflight < accelerationSubmit &&
        scene.substr(sceneStart, staticCreate - sceneStart).find("RtSmokeDynamicGeometryBuffers existingDynamic = {};") != std::string::npos &&
        scene.substr(staticCreate, sceneCommit - staticCreate).find("combined.insert(combined.end(), staticRetirementBatch.begin(), staticRetirementBatch.end())") != std::string::npos,
        "R4-006 static candidate buffers cannot alias committed bytes; combined retirement preflights before submit and static publication/retirement follows scene commit");
    Check(sceneStart != std::string::npos && sceneCommit < sceneFinalize && sceneFinalize != std::string::npos &&
        skin.substr(0, finalize).find("m_rewritePreviousSkinnedJoints.swap") == std::string::npos &&
        skin.substr(finalize).find("m_rewritePreviousSkinnedJoints.swap") != std::string::npos &&
        scene.find("candidateSkin.output", sceneStart) < sceneCommit &&
        scene.find("combined = skinnedTransaction.retirement", sceneStart) < sceneCommit,
        "R3-010 scene consumes candidate handles and publishes skin/history only after combined preflight and scene commit");
    Check(resources.find("previousDesc->trackLiveness == bindingSetDesc.trackLiveness && *previousDesc == bindingSetDesc") != std::string::npos &&
        resources.find("desc.existingBindingSet->getLayout() == desc.bindingLayout") != std::string::npos &&
        scene.find("bindingBuildDesc.existingBindingReceipt = &tlasSlot->sceneBindingReceipt") != std::string::npos &&
        resources.find("desc.existingBindingReceipt->Compare(") != std::string::npos &&
        resources.find("result.bindingReceipt.Capture(desc.device, desc.bindingLayout, bindingSetDesc, result.bindingSet)") != std::string::npos &&
        scene.find("tlasSlot->sceneBindingReceipt = std::move(bindingBuildResult.bindingReceipt)", sceneCommit) < sceneFinalize,
        "R3-011 caller binding receipt is paired and published only after scene success; legacy descriptor contract preserved");
    Check(skin.find("meshBlasDesc.buildFlags = nvrhi::rt::AccelStructBuildFlags::AllowUpdate | nvrhi::rt::AccelStructBuildFlags::PreferFastBuild") != std::string::npos &&
        skin.find("kRewriteSkinnedBlasRefreshCadenceFrames = 60") != std::string::npos &&
        skin.find("m_rewriteNextSkinnedGeneration > UINT64_MAX - 2") != std::string::npos,
        "R3-010 update creation flags refresh cadence and generation exhaustion are explicit");
    const char* counters[] = {"physicalBufferCreateCount", "physicalBindingCreateCount", "physicalDescriptorTableCreateCount",
        "physicalBlasCreateCount", "physicalTlasCreateCount", "skinnedCommandsRecorded"};
    bool tags = true;
    for (const char* tag : counters) tags = tags && scene.find(std::string("OPTICK_TAG(\"") + tag + "\"") != std::string::npos;
    Check(tags, "R3-010 outer physical allocation and rejected-command counters are present");
    const std::string receipt = read("PathTraceBindingReuse.h");
    const std::string producer = read("PathTraceCpuProducerRewrite.cpp");
    const auto seal = producer.find("bool RtCpuProducerRewriteService::CopyOverlayFromInput(");
    const auto order = producer.find("OPTICK_EVENT(\"PT CPU Overlay Canonical Order\")", seal);
    Check(!receipt.empty() && receipt.find("struct RtPathTraceBindingReuseReceipt") != std::string::npos &&
        receipt.find("input.trackLiveness != candidate.trackLiveness") != std::string::npos &&
        receipt.find("input.bindings[i] != candidate.bindings[i]") != std::string::npos &&
        receipt.find("nvrhi::RefCountPtr<nvrhi::IResource>") != std::string::npos &&
        seal != std::string::npos && order < producer.find("overlay.view.rows = rows", seal),
        "R3-011 receipt owns exact caller handles and overlay canonicalizes before publication");
    Check(skin.find("tx.layoutComparison = RtCpuRewriteCompareSkinnedLayout(layout, retained.layout)") != std::string::npos &&
        scene.find("OPTICK_TAG(\"skinnedLayoutMismatchReason\"") != std::string::npos &&
        scene.find("OPTICK_TAG(\"sceneBindingMismatchSlot\"") != std::string::npos &&
        skin.find("m_rewriteTlasSlots[slot].sceneBindingReceipt = {}") != std::string::npos &&
        scene.find("m_rewriteTlasSlots[slot] = RtCpuRewriteIsolatedTlasSlot()") != std::string::npos,
        "R3-011 actual comparisons emit reasons and zero/drain clear receipt ownership");
}

int RunSelfTest()
{
	TestLifecycle();
	TestRigidMeshPersist();
	TestRigidRegistryInstance();
	TestEmptyPackRoundTrip();
	TestThreeFrameDirLoad();
	TestDoom32VsDoom33Ranking();
	TestSkipAllCleanTrap();
	TestPublishContract();
	TestRegistryContract();
	TestRegistryPublish();
	TestSmokeTlasCapacity();
	TestRigidCpuMeshSnapshot();
	TestRigidMeshCandidateMapGuard();
	TestRegistryBuiltBlasSubmit();
	TestA8S1CanonicalIdentityObservation();
	TestA8S1LifecycleTransitions();
	TestMaterialClassifyDeriveRingContract();
	TestCommittedDynamicGeometryContract();
	TestUniversePlanningSnapshotContract();
	TestProducerLaneContract();
	TestCaptureProductR3Bindings();
	TestR3005SkinnedBlasBufferDescriptorSourceContract();
	TestR3006SkinnedWorldSpaceAndProfileSourceContract();
	TestR3008ARigidBoundaryAttributionSourceContract();
	TestR3009NarrowRouteBufferResolverSourceContract();
	TestR3010SkinnedTransactionSourceContract();
	TestRegistryGap();
	TestCompareContract();
	TestRegistryCompare();
	TestCaseCWalkSet();
	TestCaseCStaticJoin();
	TestMergedCaseCWalkSet();
	TestHeadroom();
	TestRejectForbidden();
	TestSchemaMismatch();
	std::cout << (g_failures == 0 ? "PASS\n" : "FAIL\n");
	return g_failures == 0 ? 0 : 1;
}

int RunDir(const fs::path& root)
{
	const int fails = LoadAndPrintDir(root);
	g_failures += fails;
	std::cout << (g_failures == 0 ? "PASS\n" : "FAIL\n");
	return g_failures == 0 ? 0 : 1;
}

void PrintUsage()
{
	std::cout << "PathTraceCpuProducerHarness --self-test | --dir <path>\n";
	std::cout << "Human-cut packs live at "
		"E:/prog/rbdoom-3-BFG-prebuilt_cpu_dev/fixtures/ "
		"(not required for --self-test).\n";
	std::cout << "Ranking key: oracle_us and omit, never dirtyInst alone.\n";
}

} // namespace

int main(int argc, char** argv)
{
	if (argc == 2 && std::strcmp(argv[1], "--self-test") == 0)
	{
		return RunSelfTest();
	}
	if (argc == 3 && std::strcmp(argv[1], "--dir") == 0)
	{
		return RunDir(argv[2]);
	}
	PrintUsage();
	std::cout << "FAIL\n";
	return 1;
}
