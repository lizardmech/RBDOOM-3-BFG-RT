#include "precompiled.h"
#pragma hdrstop

#include "PathTraceCpuProducerPublish.h"
#include "PathTraceCVars.h"

int DecodeCpuProducerPublishMode()
{
	const int raw = r_pathTracingCpuProducerPublish.GetInteger();
	bool rejected = false;
	std::string log;
	const cpu_producer_publish::PublishMode mode =
		cpu_producer_publish::DecodePublishModeRaw(raw, &rejected, &log);
	if (rejected)
	{
		common->Warning("%s", log.c_str());
	}
	return static_cast<int>(mode);
}

int DecodeCpuProducerRegistryMode()
{
	const int raw = r_pathTracingCpuProducerRegistry.GetInteger();
	bool rejected = false;
	std::string log;
	const cpu_producer_publish::RegistryMode mode =
		cpu_producer_publish::DecodeRegistryModeRaw(raw, &rejected, &log);
	if (rejected)
	{
		common->Warning("%s", log.c_str());
	}
	return static_cast<int>(mode);
}

namespace cpu_producer_publish
{

namespace
{
constexpr int kCompareDumpFrameCount = 3;

struct CompareDumpAccumulator
{
	idStr dest;
	std::string text;
	int frames = 0;
};

CompareDumpAccumulator g_compareDump;
}

void MaybeDumpCpuProducerCompare(
	const CompareFrameInput& in,
	const CompareFrameResult& result,
	const CompareDumpConfig& cfg)
{
	const char* requested = r_pathTracingCpuProducerCompareDump.GetString();
	if (requested == nullptr || requested[0] == '\0')
	{
		if (g_compareDump.frames > 0)
		{
			g_compareDump = CompareDumpAccumulator();
		}
		return;
	}

	if (g_compareDump.dest.IsEmpty() || g_compareDump.dest.Icmp(requested) != 0)
	{
		g_compareDump = CompareDumpAccumulator();
		g_compareDump.dest = requested;
		common->Printf("PathTraceCpuProducerCompareDump: capturing %d frames to %s\n",
			kCompareDumpFrameCount, requested);
	}

	g_compareDump.text += FormatCompareDumpText(in, result, cfg);
	++g_compareDump.frames;
	if (g_compareDump.frames < kCompareDumpFrameCount)
	{
		return;
	}

	idStr dest = g_compareDump.dest;
	const bool relativeName = dest.Find('/') < 0 && dest.Find('\\') < 0;
	bool wroteOk = false;
	if (relativeName)
	{
		idFile* file = fileSystem->OpenFileWrite(dest.c_str());
		if (file != nullptr)
		{
			const int wrote = file->Write(
				g_compareDump.text.data(),
				static_cast<int>(g_compareDump.text.size()));
			fileSystem->CloseFile(file);
			wroteOk = wrote == static_cast<int>(g_compareDump.text.size());
		}
	}
	else
	{
		FILE* file = nullptr;
#if defined(_MSC_VER)
		fopen_s(&file, dest.c_str(), "wb");
#else
		file = fopen(dest.c_str(), "wb");
#endif
		if (file != nullptr)
		{
			const size_t wrote = fwrite(
				g_compareDump.text.data(), 1, g_compareDump.text.size(), file);
			fclose(file);
			wroteOk = wrote == g_compareDump.text.size();
		}
	}

	if (!wroteOk)
	{
		common->Warning(
			"PathTraceCpuProducerCompareDump: write failed path=%s (quote hyphenated paths; prefer a simple filename)",
			dest.c_str());
		g_compareDump = CompareDumpAccumulator();
		r_pathTracingCpuProducerCompareDump.SetString("");
		return;
	}
	common->Printf(
		"PathTraceCpuProducerCompareDump: wrote %s frames=%d bytes=%u\n",
		dest.c_str(),
		g_compareDump.frames,
		static_cast<unsigned>(g_compareDump.text.size()));
	g_compareDump = CompareDumpAccumulator();
	r_pathTracingCpuProducerCompareDump.SetString("");
}

void MaybeDumpCaseCWalk(const CaseCWalkSet& walk)
{
	const char* requested = r_pathTracingCpuProducerCaseCDump.GetString();
	if (requested == nullptr || requested[0] == '\0')
	{
		return;
	}

	idStr dest = requested;
	// Bare filename only (0a26dbf79): strip any tokenized directory prefix.
	if (dest.Find('/') >= 0 || dest.Find('\\') >= 0)
	{
		idStr fileName;
		dest.ExtractFileName(fileName);
		dest = fileName;
	}
	if (dest.IsEmpty())
	{
		common->Warning("PathTraceCpuProducerCaseCDump: empty filename, cleared");
		r_pathTracingCpuProducerCaseCDump.SetString("");
		return;
	}

	idStr text;
	text.Format(
		"PathTraceCpuProducerCaseCDump static=%u merged=%u tri_s=%u tri_m=%u\n",
		walk.staticBakeOnly(),
		walk.mergedDynamicOnly(),
		walk.staticBakeTriangles,
		walk.mergedDynamicTriangles);
	for (uint64_t id : walk.staticBakeOnlyIds)
	{
		text += va("static %llu\n", static_cast<unsigned long long>(id));
	}
	for (uint64_t id : walk.mergedDynamicOnlyIds)
	{
		text += va("merged %llu\n", static_cast<unsigned long long>(id));
	}

	idFile* file = fileSystem->OpenFileWrite(dest.c_str());
	if (file == nullptr)
	{
		common->Warning(
			"PathTraceCpuProducerCaseCDump: write failed name=%s (use a bare filename; quote hyphenated paths)",
			dest.c_str());
		r_pathTracingCpuProducerCaseCDump.SetString("");
		return;
	}
	const int wrote = file->Write(text.c_str(), text.Length());
	fileSystem->CloseFile(file);
	if (wrote != text.Length())
	{
		common->Warning(
			"PathTraceCpuProducerCaseCDump: short write name=%s",
			dest.c_str());
		r_pathTracingCpuProducerCaseCDump.SetString("");
		return;
	}
	common->Printf(
		"PathTraceCpuProducerCaseCDump: wrote %s static=%u merged=%u\n",
		dest.c_str(),
		walk.staticBakeOnly(),
		walk.mergedDynamicOnly());
	r_pathTracingCpuProducerCaseCDump.SetString("");
}


void MaybeDumpMergedWalk(const MergedCaseCWalkSet& walk)
{
	const char* requested = r_pathTracingCpuProducerMergedWalkDump.GetString();
	if (requested == nullptr || requested[0] == '\0')
	{
		return;
	}

	idStr dest = requested;
	if (dest.Find('/') >= 0 || dest.Find('\\') >= 0)
	{
		idStr fileName;
		dest.ExtractFileName(fileName);
		dest = fileName;
	}
	if (dest.IsEmpty())
	{
		common->Warning("PathTraceCpuProducerMergedWalkDump: empty filename, cleared");
		r_pathTracingCpuProducerMergedWalkDump.SetString("");
		return;
	}

	idStr text;
	text.Format(
		"PathTraceCpuProducerMergedWalkDump covered=%u stable=%u tri_c=%u tri_s=%u excl_p=%u excl_d=%u\n",
		walk.covered(),
		walk.stable(),
		walk.coveredTriangles,
		walk.stableTriangles,
		walk.excludedParticle,
		walk.excludedDeform);
	for (uint64_t id : walk.coveredIds)
	{
		text += va("covered %llu\n", static_cast<unsigned long long>(id));
	}

	idFile* file = fileSystem->OpenFileWrite(dest.c_str());
	if (file == nullptr)
	{
		common->Warning(
			"PathTraceCpuProducerMergedWalkDump: write failed name=%s (use a bare filename; quote hyphenated paths)",
			dest.c_str());
		r_pathTracingCpuProducerMergedWalkDump.SetString("");
		return;
	}
	const int wrote = file->Write(text.c_str(), text.Length());
	fileSystem->CloseFile(file);
	if (wrote != text.Length())
	{
		common->Warning(
			"PathTraceCpuProducerMergedWalkDump: short write name=%s",
			dest.c_str());
		r_pathTracingCpuProducerMergedWalkDump.SetString("");
		return;
	}
	common->Printf(
		"PathTraceCpuProducerMergedWalkDump: wrote %s covered=%u stable=%u\n",
		dest.c_str(),
		walk.covered(),
		walk.stable());
	r_pathTracingCpuProducerMergedWalkDump.SetString("");
}

void MaybeDumpHeadroom(const HeadroomResult& headroom)
{
	const char* requested = r_pathTracingCpuProducerHeadroomDump.GetString();
	if (requested == nullptr || requested[0] == '\0')
	{
		return;
	}

	idStr dest = requested;
	if (dest.Find('/') >= 0 || dest.Find('\\') >= 0)
	{
		idStr fileName;
		dest.ExtractFileName(fileName);
		dest = fileName;
	}
	if (dest.IsEmpty())
	{
		common->Warning("PathTraceCpuProducerHeadroomDump: empty filename, cleared");
		r_pathTracingCpuProducerHeadroomDump.SetString("");
		return;
	}

	idStr text;
	text.Format(
		"PathTraceCpuProducerHeadroomDump cand=%u rigid=%u skinned=%u\n",
		headroom.total(),
		headroom.rigid(),
		headroom.skinned());
	for (uint64_t id : headroom.rigidIds)
	{
		text += va("rigid %llu\n", static_cast<unsigned long long>(id));
	}
	for (uint64_t id : headroom.skinnedIds)
	{
		text += va("skinned %llu\n", static_cast<unsigned long long>(id));
	}

	idFile* file = fileSystem->OpenFileWrite(dest.c_str());
	if (file == nullptr)
	{
		common->Warning(
			"PathTraceCpuProducerHeadroomDump: write failed name=%s (use a bare filename; quote hyphenated paths)",
			dest.c_str());
		r_pathTracingCpuProducerHeadroomDump.SetString("");
		return;
	}
	const int wrote = file->Write(text.c_str(), text.Length());
	fileSystem->CloseFile(file);
	if (wrote != text.Length())
	{
		common->Warning(
			"PathTraceCpuProducerHeadroomDump: short write name=%s",
			dest.c_str());
		r_pathTracingCpuProducerHeadroomDump.SetString("");
		return;
	}
	common->Printf(
		"PathTraceCpuProducerHeadroomDump: wrote %s cand=%u rigid=%u skinned=%u\n",
		dest.c_str(),
		headroom.total(),
		headroom.rigid(),
		headroom.skinned());
	r_pathTracingCpuProducerHeadroomDump.SetString("");
}

void MaybeDumpRegistryGap(const RegistryGapResult& gap)
{
	const char* requested = r_pathTracingCpuProducerRegistryGapDump.GetString();
	if (requested == nullptr || requested[0] == '\0')
	{
		return;
	}

	idStr dest = requested;
	if (dest.Find('/') >= 0 || dest.Find('\\') >= 0)
	{
		idStr fileName;
		dest.ExtractFileName(fileName);
		dest = fileName;
	}
	if (dest.IsEmpty())
	{
		common->Warning("PathTraceCpuProducerRegistryGapDump: empty filename, cleared");
		r_pathTracingCpuProducerRegistryGapDump.SetString("");
		return;
	}

	idStr text;
	text.Format(
		"PathTraceCpuProducerRegistryGapDump live=%u product=%u gap=%u\n",
		gap.live(),
		gap.product(),
		gap.gap());
	for (uint64_t id : gap.gapIds)
	{
		text += va("gap %llu\n", static_cast<unsigned long long>(id));
	}

	idFile* file = fileSystem->OpenFileWrite(dest.c_str());
	if (file == nullptr)
	{
		common->Warning(
			"PathTraceCpuProducerRegistryGapDump: write failed name=%s (use a bare filename; quote hyphenated paths)",
			dest.c_str());
		r_pathTracingCpuProducerRegistryGapDump.SetString("");
		return;
	}
	const int wrote = file->Write(text.c_str(), text.Length());
	fileSystem->CloseFile(file);
	if (wrote != text.Length())
	{
		common->Warning(
			"PathTraceCpuProducerRegistryGapDump: short write name=%s",
			dest.c_str());
		r_pathTracingCpuProducerRegistryGapDump.SetString("");
		return;
	}
	common->Printf(
		"PathTraceCpuProducerRegistryGapDump: wrote %s live=%u product=%u gap=%u\n",
		dest.c_str(),
		gap.live(),
		gap.product(),
		gap.gap());
	r_pathTracingCpuProducerRegistryGapDump.SetString("");
}

void MaybeDumpCpuProducerRegistry(
	const CompareFrameInput& in,
	const CompareFrameResult& result)
{
	const char* requested = r_pathTracingCpuProducerRegistryDump.GetString();
	if (requested == nullptr || requested[0] == '\0')
	{
		return;
	}

	idStr dest = requested;
	if (dest.Find('/') >= 0 || dest.Find('\\') >= 0)
	{
		idStr fileName;
		dest.ExtractFileName(fileName);
		dest = fileName;
	}
	if (dest.IsEmpty())
	{
		common->Warning("PathTraceCpuProducerRegistryDump: empty filename, cleared");
		r_pathTracingCpuProducerRegistryDump.SetString("");
		return;
	}

	const std::string body = FormatRegistryDumpText(in, result);
	idFile* file = fileSystem->OpenFileWrite(dest.c_str());
	if (file == nullptr)
	{
		common->Warning(
			"PathTraceCpuProducerRegistryDump: write failed name=%s (use a bare filename; quote hyphenated paths)",
			dest.c_str());
		r_pathTracingCpuProducerRegistryDump.SetString("");
		return;
	}
	const int wrote = file->Write(body.c_str(), static_cast<int>(body.size()));
	fileSystem->CloseFile(file);
	if (wrote != static_cast<int>(body.size()))
	{
		common->Warning(
			"PathTraceCpuProducerRegistryDump: short write name=%s",
			dest.c_str());
		r_pathTracingCpuProducerRegistryDump.SetString("");
		return;
	}
	common->Printf(
		"PathTraceCpuProducerRegistryDump: wrote %s live=%u gpuResident=%u extras=%u\n",
		dest.c_str(),
		static_cast<unsigned>(in.registryLive.size()),
		static_cast<unsigned>(result.registryGpuResidentIds.size()),
		static_cast<unsigned>(in.registrySubmitted.size()));
	r_pathTracingCpuProducerRegistryDump.SetString("");
}

} // namespace cpu_producer_publish