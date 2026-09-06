// Versioned CPU-producer fixture pack. Header-only so the standalone
// PathTraceCpuProducerHarness and the in-game packer write the same bytes.
// No engine types. No pointers in durable records.

#pragma once

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <type_traits>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace cpu_producer_pack
{

constexpr uint32_t kSchemaVersion = 3;
constexpr char kSchemaName[] = "cpu-producer-pack";
constexpr char kMagic[4] = { 'C', 'P', 'R', '1' };

const char* const kManifestName = "manifest.json";
const char* const kTexturesName = "textures.json";
const char* const kPackBins[] = {
	"meshes.bin",
	"materials.bin",
	"instances.bin",
	"lights.bin",
	"membership.bin",
	"frames.bin",
	"oracle.bin",
};

enum Dirty : uint32_t
{
	kDirtyNone = 0,
	kDirtyXform = 1u << 0,
	kDirtyJoints = 1u << 1,
	kDirtyMaterial = 1u << 2,
	kDirtyMesh = 1u << 3,
	kDirtyIntensity = 1u << 4,
	kDirtyShader = 1u << 5,
	kDirtyLight = 1u << 6,
};

struct MeshRecord
{
	uint64_t meshId;
	uint64_t contentChecksum;
	uint32_t modelEpoch;
	uint32_t deformationClass;
	uint32_t generation;
	uint32_t vertexCount;
	uint32_t indexCount;
	uint32_t _pad;
};

struct MaterialRecord
{
	uint64_t materialId;
	uint32_t generation;
	uint32_t overlayGeneration;
	uint32_t emissiveBase;
	uint32_t _pad;
};

struct InstanceRecord
{
	uint64_t instanceId;
	uint64_t meshId;
	uint32_t dirty;
	uint32_t generation;
	uint32_t live;
	uint32_t omittedSkin;
	uint32_t vertexCount;
	uint32_t _pad;
};

struct LightRecord
{
	uint64_t lightId;
	uint32_t dirty;
	uint32_t generation;
	uint32_t live;
	uint32_t _pad;
};

struct MembershipRecord
{
	int32_t areaNum;
	int32_t _pad;
};

struct FrameRecord
{
	uint32_t frameIndex;
	uint32_t dirtyInst;
	uint32_t dirtyXform;
	uint32_t dirtyJoints;
	uint32_t dirtyMaterial;
	uint32_t dirtyMesh;
	uint32_t dirtyLight;
	uint32_t omit;
	uint32_t admit;
};

struct OracleRecord
{
	uint32_t frameIndex;
	int32_t validationMs;
	int32_t dynamicPassClassifyMs;
	int32_t dynamicAppendMs;
	int32_t rtCpuSkinningAppendMs;
	int32_t skinnedCaptureAdmissionRoutes;
	int32_t skinnedCaptureOmittedSurfaces;
	int32_t staticCachedSurfaces;
	int32_t staticNewSurfaces;
	int32_t entityAdds;
	int32_t entityUpdates;
	int32_t entityUnchanged;
	int32_t entityFrees;
	int32_t lightAdds;
	int32_t lightUpdates;
	int32_t lightFrees;
	uint64_t rtCpuSkinningAppendUs;
	uint64_t buildSceneUs;
	uint64_t oracleUs;
	int32_t skinnedCaptureOmittedVerts;
	int32_t skinnedCaptureOmittedIndexes;
};

// Surface-stable id. No pointers. Distinct from entity PackDefId so
// omitted-skin surfaces stay first-class even when the entity hook is clean.
inline uint64_t PackSkinnedSurfaceId(
	uint64_t worldGeneration, uint32_t renderDefIndex, uint32_t surfaceIndex)
{
	return (worldGeneration << 32) ^
		(static_cast<uint64_t>(renderDefIndex) << 16) ^
		(static_cast<uint64_t>(surfaceIndex) + 1u);
}

struct Manifest
{
	uint32_t schemaVersion = 0;
	std::string schema;
	std::string saveName;
	std::string note;
	uint32_t frameCount = 0;
	int32_t width = 0;
	int32_t height = 0;
	int32_t gi = -1;
	int32_t membershipPresent = 0;
};

static_assert(std::is_trivially_copyable<MeshRecord>::value, "MeshRecord");
static_assert(std::is_trivially_copyable<MaterialRecord>::value, "MaterialRecord");
static_assert(std::is_trivially_copyable<InstanceRecord>::value, "InstanceRecord");
static_assert(std::is_trivially_copyable<LightRecord>::value, "LightRecord");
static_assert(std::is_trivially_copyable<MembershipRecord>::value, "MembershipRecord");
static_assert(std::is_trivially_copyable<FrameRecord>::value, "FrameRecord");
static_assert(std::is_trivially_copyable<OracleRecord>::value, "OracleRecord");

inline std::string JsonEscape(const std::string& s)
{
	std::string o;
	o.reserve(s.size());
	for (char c : s)
	{
		if (c == '"' || c == '\\')
		{
			o.push_back('\\');
		}
		o.push_back(c);
	}
	return o;
}

inline bool WriteText(const std::filesystem::path& path, const std::string& text)
{
	std::ofstream out(path, std::ios::binary | std::ios::trunc);
	if (!out)
	{
		return false;
	}
	out << text;
	return static_cast<bool>(out);
}

inline bool ReadText(const std::filesystem::path& path, std::string& text)
{
	std::ifstream in(path, std::ios::binary);
	if (!in)
	{
		return false;
	}
	std::ostringstream ss;
	ss << in.rdbuf();
	text = ss.str();
	return true;
}

template <typename T>
bool WriteBinTable(const std::filesystem::path& path, const std::vector<T>& rows)
{
	std::ofstream out(path, std::ios::binary | std::ios::trunc);
	if (!out)
	{
		return false;
	}
	const uint32_t version = kSchemaVersion;
	const uint32_t count = static_cast<uint32_t>(rows.size());
	const uint32_t stride = static_cast<uint32_t>(sizeof(T));
	out.write(kMagic, 4);
	out.write(reinterpret_cast<const char*>(&version), 4);
	out.write(reinterpret_cast<const char*>(&count), 4);
	out.write(reinterpret_cast<const char*>(&stride), 4);
	if (!rows.empty())
	{
		out.write(reinterpret_cast<const char*>(rows.data()),
			static_cast<std::streamsize>(rows.size() * sizeof(T)));
	}
	return static_cast<bool>(out);
}

template <typename T>
bool ReadBinTable(const std::filesystem::path& path, std::vector<T>& rows, std::string& err)
{
	std::ifstream in(path, std::ios::binary);
	if (!in)
	{
		err = "missing " + path.filename().string();
		return false;
	}
	char magic[4] = {};
	uint32_t version = 0;
	uint32_t count = 0;
	uint32_t stride = 0;
	in.read(magic, 4);
	in.read(reinterpret_cast<char*>(&version), 4);
	in.read(reinterpret_cast<char*>(&count), 4);
	in.read(reinterpret_cast<char*>(&stride), 4);
	if (!in || std::memcmp(magic, kMagic, 4) != 0)
	{
		err = "bad magic in " + path.filename().string();
		return false;
	}
	if (version != kSchemaVersion)
	{
		err = "schema version mismatch";
		return false;
	}
	if (stride != sizeof(T))
	{
		err = "stride mismatch in " + path.filename().string();
		return false;
	}
	rows.assign(count, T{});
	if (count > 0)
	{
		in.read(reinterpret_cast<char*>(rows.data()),
			static_cast<std::streamsize>(count * sizeof(T)));
		if (!in)
		{
			err = "truncated " + path.filename().string();
			return false;
		}
	}
	return true;
}

inline std::string ToLower(std::string s)
{
	for (char& c : s)
	{
		if (c >= 'A' && c <= 'Z')
		{
			c = static_cast<char>(c - 'A' + 'a');
		}
	}
	return s;
}

inline bool LooksForbiddenName(const std::string& name)
{
	const std::string lower = ToLower(name);
	if (lower.size() >= 4 && lower.compare(lower.size() - 4, 4, ".dmp") == 0)
	{
		return true;
	}
	return lower.find("minidump") != std::string::npos ||
		lower.find("viewdef") != std::string::npos ||
		lower.find("srftriangles") != std::string::npos;
}

inline bool TextLooksForbidden(const std::string& text)
{
	const std::string lower = ToLower(text);
	return lower.find("minidump") != std::string::npos ||
		lower.find("srftriangles_t") != std::string::npos ||
		lower.find("viewdef->") != std::string::npos ||
		lower.find("drawsurf_t*") != std::string::npos;
}

inline std::string WriteManifest(const Manifest& m)
{
	std::ostringstream ss;
	ss << "{\n"
	   << "  \"schema\": \"" << JsonEscape(m.schema.empty() ? kSchemaName : m.schema) << "\",\n"
	   << "  \"schemaVersion\": " << m.schemaVersion << ",\n"
	   << "  \"saveName\": \"" << JsonEscape(m.saveName) << "\",\n"
	   << "  \"note\": \"" << JsonEscape(m.note) << "\",\n"
	   << "  \"frameCount\": " << m.frameCount << ",\n"
	   << "  \"width\": " << m.width << ",\n"
	   << "  \"height\": " << m.height << ",\n"
	   << "  \"gi\": " << m.gi << ",\n"
	   << "  \"membershipPresent\": " << m.membershipPresent << "\n"
	   << "}\n";
	return ss.str();
}

inline bool GrabJsonString(const std::string& text, const char* key, std::string& out)
{
	const std::string pat = std::string("\"") + key + "\"";
	const auto pos = text.find(pat);
	if (pos == std::string::npos)
	{
		return false;
	}
	const auto colon = text.find(':', pos + pat.size());
	const auto q1 = text.find('"', colon);
	const auto q2 = text.find('"', q1 + 1);
	if (colon == std::string::npos || q1 == std::string::npos || q2 == std::string::npos)
	{
		return false;
	}
	out = text.substr(q1 + 1, q2 - q1 - 1);
	return true;
}

inline bool GrabJsonI32(const std::string& text, const char* key, int32_t& out)
{
	const std::string pat = std::string("\"") + key + "\"";
	const auto pos = text.find(pat);
	if (pos == std::string::npos)
	{
		return false;
	}
	const auto colon = text.find(':', pos + pat.size());
	if (colon == std::string::npos)
	{
		return false;
	}
	out = static_cast<int32_t>(std::strtol(text.c_str() + colon + 1, nullptr, 10));
	return true;
}

inline bool ParseManifest(const std::string& text, Manifest& m, std::string& err)
{
	if (TextLooksForbidden(text))
	{
		err = "manifest contains forbidden capture payload";
		return false;
	}
	int32_t schemaVersion = 0;
	int32_t frameCount = 0;
	if (!GrabJsonString(text, "schema", m.schema) ||
		!GrabJsonI32(text, "schemaVersion", schemaVersion) ||
		!GrabJsonString(text, "saveName", m.saveName) ||
		!GrabJsonI32(text, "frameCount", frameCount))
	{
		err = "manifest missing required fields";
		return false;
	}
	m.schemaVersion = static_cast<uint32_t>(schemaVersion);
	m.frameCount = static_cast<uint32_t>(frameCount);
	GrabJsonString(text, "note", m.note);
	GrabJsonI32(text, "width", m.width);
	GrabJsonI32(text, "height", m.height);
	GrabJsonI32(text, "gi", m.gi);
	GrabJsonI32(text, "membershipPresent", m.membershipPresent);
	if (m.schema != kSchemaName || m.schemaVersion != kSchemaVersion)
	{
		err = "schema version mismatch";
		return false;
	}
	return true;
}

inline std::string RejectPack(const std::filesystem::path& dir)
{
	if (!std::filesystem::exists(dir) || !std::filesystem::is_directory(dir))
	{
		return "pack dir missing";
	}
	for (const auto& entry : std::filesystem::directory_iterator(dir))
	{
		const std::string name = entry.path().filename().string();
		if (LooksForbiddenName(name))
		{
			return "rejected forbidden file: " + name;
		}
	}
	return {};
}

struct PackTables
{
	Manifest manifest;
	std::vector<MeshRecord> meshes;
	std::vector<MaterialRecord> materials;
	std::vector<InstanceRecord> instances;
	std::vector<LightRecord> lights;
	std::vector<MembershipRecord> membership;
	std::vector<FrameRecord> frames;
	std::vector<OracleRecord> oracles;
};

inline bool WritePack(const std::filesystem::path& dir, const PackTables& pack, std::string& err)
{
	std::error_code ec;
	std::filesystem::create_directories(dir, ec);
	if (ec)
	{
		err = "create_directories failed";
		return false;
	}
	Manifest manifest = pack.manifest;
	if (manifest.schema.empty())
	{
		manifest.schema = kSchemaName;
	}
	if (manifest.schemaVersion == 0)
	{
		manifest.schemaVersion = kSchemaVersion;
	}
	if (!WriteText(dir / kManifestName, WriteManifest(manifest)))
	{
		err = "write manifest failed";
		return false;
	}
	if (!WriteBinTable(dir / "meshes.bin", pack.meshes) ||
		!WriteBinTable(dir / "materials.bin", pack.materials) ||
		!WriteBinTable(dir / "instances.bin", pack.instances) ||
		!WriteBinTable(dir / "lights.bin", pack.lights) ||
		!WriteBinTable(dir / "membership.bin", pack.membership) ||
		!WriteBinTable(dir / "frames.bin", pack.frames) ||
		!WriteBinTable(dir / "oracle.bin", pack.oracles) ||
		!WriteText(dir / kTexturesName, "{\n  \"textures\": []\n}\n"))
	{
		err = "write pack bins failed";
		return false;
	}
	return true;
}

inline bool ReadPack(const std::filesystem::path& dir, PackTables& pack, std::string& err)
{
	err = RejectPack(dir);
	if (!err.empty())
	{
		return false;
	}
	std::string text;
	if (!ReadText(dir / kManifestName, text))
	{
		err = "missing manifest.json";
		return false;
	}
	if (!ParseManifest(text, pack.manifest, err))
	{
		return false;
	}
	for (const char* name : kPackBins)
	{
		if (!std::filesystem::exists(dir / name))
		{
			err = std::string("missing ") + name;
			return false;
		}
	}
	if (!std::filesystem::exists(dir / kTexturesName))
	{
		err = "missing textures.json";
		return false;
	}
	return ReadBinTable(dir / "meshes.bin", pack.meshes, err) &&
		ReadBinTable(dir / "materials.bin", pack.materials, err) &&
		ReadBinTable(dir / "instances.bin", pack.instances, err) &&
		ReadBinTable(dir / "lights.bin", pack.lights, err) &&
		ReadBinTable(dir / "membership.bin", pack.membership, err) &&
		ReadBinTable(dir / "frames.bin", pack.frames, err) &&
		ReadBinTable(dir / "oracle.bin", pack.oracles, err);
}

inline uint64_t PackDefId(uint64_t worldGeneration, int index)
{
	return (worldGeneration << 32) | static_cast<uint32_t>(index);
}

} // namespace cpu_producer_pack