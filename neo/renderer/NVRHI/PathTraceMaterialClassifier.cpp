#include "precompiled.h"
#pragma hdrstop

#include "PathTraceCVars.h"
#include "PathTraceDoomMaterialClassifier.h"
#include "PathTraceMaterialClassifier.h"

#include <cstring>
#include <unordered_map>

namespace {

struct RtMaterialRecordKey
{
    uint32_t materialId = 0;
    uint64 materialNameHash = 0;

    bool operator==(const RtMaterialRecordKey& rhs) const
    {
        return materialId == rhs.materialId && materialNameHash == rhs.materialNameHash;
    }
};

struct RtMaterialRecordKeyHash
{
    size_t operator()(const RtMaterialRecordKey& key) const
    {
        uint64 hash = key.materialNameHash;
        hash ^= static_cast<uint64>(key.materialId) + 0x9e3779b97f4a7c15ull + (hash << 6) + (hash >> 2);
        return static_cast<size_t>(hash ^ (hash >> 32));
    }
};

struct RtMaterialClassTokenRule
{
    const char* token = nullptr;
    RtMaterialSurfaceClass surfaceClass = RtMaterialSurfaceClass::Unknown;
    bool matchFullText = false;
};

struct RtMaterialClassCandidate
{
    RtMaterialSurfaceClass surfaceClass = RtMaterialSurfaceClass::Unknown;
    RtMaterialSurfaceClassReason reason = RtMaterialSurfaceClassReason::Unknown;
    RtMaterialClassConfidence confidence = RtMaterialClassConfidence::FallbackNone;
    idStr evidence;
};

std::unordered_map<RtMaterialRecordKey, RtMaterialRecord, RtMaterialRecordKeyHash> g_materialRecords;
std::unordered_map<uint32_t, const RtMaterialRecord*> g_materialRecordsById;
RtMaterialClassifierStats g_materialClassifierStats;
bool g_dumpedDeclSurfaceDistribution = false;
int g_recordDebugLogs = 0;
uint32_t g_materialClassifierGeneration = 1u;

uint64 HashRtMaterialValue(uint64 hash, uint64 value)
{
    hash ^= value + 0x9e3779b97f4a7c15ull + (hash << 6) + (hash >> 2);
    return hash;
}

bool TryIndexPathTraceMaterialRecordById(
    std::unordered_map<uint32_t, const RtMaterialRecord*>& index,
    uint32_t materialId,
    const RtMaterialRecord* record,
    bool assertOnConflict)
{
    const std::pair<std::unordered_map<uint32_t, const RtMaterialRecord*>::iterator, bool>
        insertResult = index.emplace(materialId, record);
    if (!insertResult.second && insertResult.first->second != record)
    {
        if (assertOnConflict)
        {
            assert(false && "duplicate path-trace materialId maps to different classifier records");
        }
        insertResult.first->second = nullptr;
        return false;
    }
    return insertResult.first->second == record;
}

bool ValidatePathTraceMaterialRecordLookupIndex()
{
    if (g_materialRecordsById.size() != g_materialRecords.size())
    {
        return false;
    }
    for (const auto& entry : g_materialRecords)
    {
        const RtMaterialRecord& record = entry.second;
        const auto indexed = g_materialRecordsById.find(record.materialId);
        if (indexed == g_materialRecordsById.end() ||
            indexed->second != &record)
        {
            return false;
        }
    }
    return true;
}

uint64 HashRtMaterialFloat(uint64 hash, float value)
{
    uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return HashRtMaterialValue(hash, bits);
}

uint64 HashRtMaterialString(uint64 hash, const idStr& value)
{
    const char* cursor = value.c_str();
    while (*cursor)
    {
        hash = HashRtMaterialValue(hash, static_cast<uint8_t>(*cursor));
        ++cursor;
    }
    return HashRtMaterialValue(hash, 0xffu);
}

RtMaterialRecordKey BuildRtMaterialRecordKey(uint32_t materialId, const idStr& materialName)
{
    RtMaterialRecordKey key;
    key.materialId = materialId;
    key.materialNameHash = HashRtMaterialString(1469598103934665603ull, materialName);
    return key;
}

bool RtMaterialTextHasToken(const idStr& text, const char* token)
{
    return token && token[0] && text.Find(token, false) >= 0;
}

idStr RtMaterialLeafName(const idStr& text)
{
    const char* source = text.c_str();
    const char* leaf = source;
    for (const char* cursor = source; *cursor; ++cursor)
    {
        if (*cursor == '/' || *cursor == '\\')
        {
            leaf = cursor + 1;
        }
    }
    return idStr(leaf);
}

RtMaterialSurfaceClass RtSurfaceClassFromTokenRules(
    const idStr& text,
    const RtMaterialClassTokenRule* rules,
    int ruleCount,
    const char* sourceName,
    idStr& evidence)
{
    if (text.IsEmpty())
    {
        return RtMaterialSurfaceClass::Unknown;
    }

    const idStr leaf = RtMaterialLeafName(text);
    for (int ruleIndex = 0; ruleIndex < ruleCount; ++ruleIndex)
    {
        const RtMaterialClassTokenRule& rule = rules[ruleIndex];
        const idStr& matchText = rule.matchFullText ? text : leaf;
        if (RtMaterialTextHasToken(matchText, rule.token))
        {
            evidence = va("%s:%s", sourceName, rule.token);
            return rule.surfaceClass;
        }
    }
    return RtMaterialSurfaceClass::Unknown;
}

idStr RtMaterialFieldEvidence(const char* fieldName, const idStr& evidence)
{
    if (!fieldName || !fieldName[0] || evidence.IsEmpty())
    {
        return evidence;
    }

    const int fieldNameLength = static_cast<int>(std::strlen(fieldName));
    if (evidence.Icmpn(fieldName, fieldNameLength) == 0 && evidence[fieldNameLength] == ':')
    {
        return evidence;
    }

    const char* detail = evidence.c_str();
    static const char materialPrefix[] = "material:";
    const int materialPrefixLength = static_cast<int>(sizeof(materialPrefix) - 1);
    if (evidence.Icmpn(materialPrefix, materialPrefixLength) == 0)
    {
        detail += materialPrefixLength;
    }
    return idStr(va("%s:%s", fieldName, detail));
}

bool RtStageBlendUsesSourceAlpha(const shaderStage_t* stage)
{
    if (!stage)
    {
        return false;
    }
    const uint64 srcBlend = stage->drawStateBits & GLS_SRCBLEND_BITS;
    const uint64 dstBlend = stage->drawStateBits & GLS_DSTBLEND_BITS;
    return srcBlend == GLS_SRCBLEND_SRC_ALPHA ||
        srcBlend == GLS_SRCBLEND_ONE_MINUS_SRC_ALPHA ||
        dstBlend == GLS_DSTBLEND_SRC_ALPHA ||
        dstBlend == GLS_DSTBLEND_ONE_MINUS_SRC_ALPHA;
}

bool RtStageIsAdditiveBlend(const shaderStage_t* stage)
{
    if (!stage)
    {
        return false;
    }
    const uint64 srcBlend = stage->drawStateBits & GLS_SRCBLEND_BITS;
    const uint64 dstBlend = stage->drawStateBits & GLS_DSTBLEND_BITS;
    return (srcBlend == GLS_SRCBLEND_ONE || srcBlend == GLS_SRCBLEND_SRC_ALPHA) && dstBlend == GLS_DSTBLEND_ONE;
}

bool RtStageIsFilterBlend(const shaderStage_t* stage)
{
    if (!stage)
    {
        return false;
    }
    const uint64 srcBlend = stage->drawStateBits & GLS_SRCBLEND_BITS;
    const uint64 dstBlend = stage->drawStateBits & GLS_DSTBLEND_BITS;
    return (srcBlend == GLS_SRCBLEND_DST_COLOR && dstBlend == GLS_DSTBLEND_ZERO) ||
        (srcBlend == GLS_SRCBLEND_ZERO && dstBlend == GLS_DSTBLEND_SRC_COLOR) ||
        (srcBlend == GLS_SRCBLEND_ZERO && dstBlend == GLS_DSTBLEND_ONE_MINUS_SRC_COLOR) ||
        (srcBlend == GLS_SRCBLEND_ONE_MINUS_DST_COLOR && dstBlend == GLS_DSTBLEND_ONE);
}

bool RtStageConditionCanBeActive(const idMaterial* material, const shaderStage_t* stage);
bool RtMaterialRegisterDependsOnRuntime(const idMaterial* material, int registerIndex);

RtMaterialCompositingOp RtStageCompositingOperation(const shaderStage_t* stage)
{
    if (!stage)
    {
        return RtMaterialCompositingOp::Unknown;
    }
    if (stage->hasAlphaTest && !stage->ignoreAlphaTest)
    {
        return RtMaterialCompositingOp::AuthoredAlphaClip;
    }
    if (stage->lighting == SL_BUMP ||
        stage->lighting == SL_DIFFUSE ||
        stage->lighting == SL_SPECULAR ||
        stage->lighting == SL_COVERAGE)
    {
        // Interaction blend encodings select an input to Doom's lighting
        // packet; they are not framebuffer compositing equations.
        return RtMaterialCompositingOp::InteractionInput;
    }

    const uint64 srcBlend = stage->drawStateBits & GLS_SRCBLEND_BITS;
    const uint64 dstBlend = stage->drawStateBits & GLS_DSTBLEND_BITS;
    if (srcBlend != GLS_SRCBLEND_ZERO && dstBlend == GLS_DSTBLEND_ONE)
    {
        // Includes authored reflection overlays such as DST_COLOR,ONE and
        // DST_ALPHA,ONE. The named family records that the source contribution
        // is added; the raw factors above remain authoritative for evaluation.
        return RtMaterialCompositingOp::Additive;
    }
    if ((srcBlend == GLS_SRCBLEND_DST_COLOR && dstBlend == GLS_DSTBLEND_ZERO) ||
        (srcBlend == GLS_SRCBLEND_ZERO && dstBlend == GLS_DSTBLEND_SRC_COLOR))
    {
        return RtMaterialCompositingOp::MultiplyFilter;
    }
    if ((srcBlend == GLS_SRCBLEND_ZERO && dstBlend == GLS_DSTBLEND_ONE_MINUS_SRC_COLOR) ||
        (srcBlend == GLS_SRCBLEND_ONE_MINUS_DST_COLOR && dstBlend == GLS_DSTBLEND_ONE))
    {
        return RtMaterialCompositingOp::InvertedFilterBlackKey;
    }
    if (srcBlend == GLS_SRCBLEND_SRC_ALPHA && dstBlend == GLS_DSTBLEND_ONE_MINUS_SRC_ALPHA)
    {
        return RtMaterialCompositingOp::SourceAlphaOver;
    }
    if (srcBlend == GLS_SRCBLEND_ONE && dstBlend == GLS_DSTBLEND_ZERO)
    {
        return RtMaterialCompositingOp::OpaqueReplace;
    }
    return RtMaterialCompositingOp::Unknown;
}

std::vector<RtMaterialCompositingStageFact> CompileRtMaterialCompositingStages(const idMaterial* material)
{
    std::vector<RtMaterialCompositingStageFact> stages;
    if (!material)
    {
        return stages;
    }
    stages.reserve(material->GetNumStages());
    for (int stageIndex = 0; stageIndex < material->GetNumStages(); ++stageIndex)
    {
        const shaderStage_t* stage = material->GetStage(stageIndex);
        if (!stage)
        {
            continue;
        }
        RtMaterialCompositingStageFact fact;
        fact.stageIndex = stageIndex;
        fact.lighting = stage->lighting;
        fact.operation = RtStageCompositingOperation(stage);
        fact.srcBlendBits = stage->drawStateBits & GLS_SRCBLEND_BITS;
        fact.dstBlendBits = stage->drawStateBits & GLS_DSTBLEND_BITS;
        fact.hasAlphaTest = stage->hasAlphaTest;
        fact.ignoreAlphaTest = stage->ignoreAlphaTest;
        fact.alphaTestRegister = stage->alphaTestRegister;
        fact.conditionRegister = stage->conditionRegister;
        fact.conditionCanBeActive = RtStageConditionCanBeActive(material, stage);
        fact.conditionIsDynamic = RtMaterialRegisterDependsOnRuntime(material, stage->conditionRegister);
        for (int component = 0; component < 4; ++component)
        {
            fact.colorRegisters[component] = stage->color.registers[component];
        }
        fact.hasTextureMatrix = stage->texture.hasMatrix;
        for (int row = 0; row < 2; ++row)
        {
            for (int column = 0; column < 3; ++column)
            {
                fact.textureMatrixRegisters[row][column] = stage->texture.hasMatrix
                    ? stage->texture.matrix[row][column]
                    : -1;
            }
        }
        fact.texgen = stage->texture.texgen;
        fact.dynamicImage = stage->texture.dynamic;
        fact.dynamicFrameCount = stage->texture.dynamicFrameCount;
        fact.vertexColor = stage->vertexColor;
        if (stage->newStage)
        {
            fact.vertexProgram = stage->newStage->vertexProgram;
            fact.fragmentProgram = stage->newStage->fragmentProgram;
            fact.glslProgram = stage->newStage->glslProgram;
        }
        fact.imageName = stage->texture.image ? stage->texture.image->GetName() : "<none>";
        stages.push_back(fact);
    }
    return stages;
}

std::vector<RtMaterialInteractionPacket> CompileRtMaterialInteractionPackets(
    const std::vector<RtMaterialCompositingStageFact>& stages)
{
    std::vector<RtMaterialInteractionPacket> packets;
    for (const RtMaterialCompositingStageFact& stage : stages)
    {
        if (stage.lighting != SL_BUMP && stage.lighting != SL_DIFFUSE && stage.lighting != SL_SPECULAR)
        {
            continue;
        }

        const bool startsPacket =
            packets.empty() ||
            stage.lighting == SL_BUMP ||
            (stage.lighting == SL_DIFFUSE && packets.back().diffuseStageIndex >= 0) ||
            (stage.lighting == SL_SPECULAR && packets.back().specularStageIndex >= 0);
        if (startsPacket)
        {
            RtMaterialInteractionPacket packet;
            packet.firstStageIndex = stage.stageIndex;
            packet.lastStageIndex = stage.stageIndex;
            packets.push_back(packet);
        }

        RtMaterialInteractionPacket& packet = packets.back();
        packet.lastStageIndex = stage.stageIndex;
        if (stage.lighting == SL_BUMP)
        {
            packet.bumpStageIndex = stage.stageIndex;
        }
        else if (stage.lighting == SL_DIFFUSE)
        {
            packet.diffuseStageIndex = stage.stageIndex;
        }
        else
        {
            packet.specularStageIndex = stage.stageIndex;
        }
    }
    return packets;
}

bool RtStageConditionCanBeActive(const idMaterial* material, const shaderStage_t* stage)
{
    if (!material || !stage)
    {
        return false;
    }

    const float* constantRegisters = material->ConstantRegisters();
    const int registerCount = material->GetNumRegisters();
    if (constantRegisters && stage->conditionRegister >= 0 && stage->conditionRegister < registerCount)
    {
        return constantRegisters[stage->conditionRegister] != 0.0f;
    }
    return true;
}

bool RtMaterialRegisterDependsOnRuntime(const idMaterial* material, int registerIndex)
{
    if (registerIndex < 0)
    {
        return false;
    }
    if (registerIndex < EXP_REG_NUM_PREDEFINED)
    {
        return true;
    }
    return material && material->ConstantRegisters() == nullptr;
}

float RtStageConstantRegisterValue(const idMaterial* material, int registerIndex, float fallback)
{
    if (!material)
    {
        return fallback;
    }

    const float* constantRegisters = material->ConstantRegisters();
    const int registerCount = material->GetNumRegisters();
    if (constantRegisters && registerIndex >= 0 && registerIndex < registerCount)
    {
        return constantRegisters[registerIndex];
    }
    return fallback;
}

const char* RtStageLightingName(stageLighting_t lighting)
{
    switch (lighting)
    {
        case SL_AMBIENT:
            return "ambient";
        case SL_BUMP:
            return "bump";
        case SL_DIFFUSE:
            return "diffuse";
        case SL_SPECULAR:
            return "specular";
        case SL_COVERAGE:
            return "coverage";
        default:
            return "unknown";
    }
}

const char* RtTextureUsageName(textureUsage_t usage)
{
    switch (usage)
    {
        case TD_SPECULAR:
            return "TD_SPECULAR";
        case TD_DIFFUSE:
            return "TD_DIFFUSE";
        case TD_BUMP:
            return "TD_BUMP";
        case TD_COVERAGE:
            return "TD_COVERAGE";
        case TD_SPECULAR_PBR_RMAO:
            return "TD_SPECULAR_PBR_RMAO";
        case TD_SPECULAR_PBR_RMAOD:
            return "TD_SPECULAR_PBR_RMAOD";
        case TD_DEFAULT:
            return "TD_DEFAULT";
        default:
            return "TD_OTHER";
    }
}

const char* RtTextureColorFormatName(textureColor_t format)
{
    switch (format)
    {
        case CFM_DEFAULT:
            return "CFM_DEFAULT";
        case CFM_NORMAL_DXT5:
            return "CFM_NORMAL_DXT5";
        case CFM_YCOCG_DXT5:
            return "CFM_YCOCG_DXT5";
        case CFM_GREEN_ALPHA:
            return "CFM_GREEN_ALPHA";
        default:
            return "CFM_UNKNOWN";
    }
}

RtMaterialStageFacts AnalyzeRtMaterialStages(const idMaterial* material)
{
    RtMaterialStageFacts facts;
    if (!material)
    {
        return facts;
    }

    facts.stageCount = material->GetNumStages();
    for (int stageIndex = 0; stageIndex < material->GetNumStages(); ++stageIndex)
    {
        const shaderStage_t* stage = material->GetStage(stageIndex);
        if (!stage || !RtStageConditionCanBeActive(material, stage))
        {
            continue;
        }

        idImage* image = stage->texture.image;
        const textureUsage_t usage = image ? image->GetUsage() : TD_DEFAULT;
        const bool additiveBlend = RtStageIsAdditiveBlend(stage);
        const bool filterBlend = RtStageIsFilterBlend(stage);
        const bool alphaBlend = RtStageBlendUsesSourceAlpha(stage);
        const RtMaterialCompositingOp compositingOp = RtStageCompositingOperation(stage);
        const bool guiOrScreen =
            stage->texture.texgen == TG_SCREEN ||
            stage->texture.texgen == TG_SCREEN2 ||
            SmokeStageIsRenderMap(stage);
        const bool cubeMap =
            stage->texture.texgen == TG_SKYBOX_CUBE ||
            stage->texture.texgen == TG_WOBBLESKY_CUBE ||
            stage->texture.texgen == TG_REFLECT_CUBE ||
            stage->texture.texgen == TG_REFLECT_CUBE2;

        if (additiveBlend)
        {
            ++facts.additiveBlendStages;
        }
        if (filterBlend)
        {
            ++facts.filterBlendStages;
        }
        if (alphaBlend)
        {
            ++facts.alphaBlendStages;
        }
        switch (compositingOp)
        {
            case RtMaterialCompositingOp::OpaqueReplace:
                ++facts.opaqueReplaceStages;
                break;
            case RtMaterialCompositingOp::InvertedFilterBlackKey:
                ++facts.invertedFilterStages;
                break;
            case RtMaterialCompositingOp::AuthoredAlphaClip:
                ++facts.authoredAlphaClipStages;
                break;
            case RtMaterialCompositingOp::Unknown:
                ++facts.unknownCompositingStages;
                break;
            default:
                break;
        }
        if (guiOrScreen)
        {
            ++facts.guiOrScreenStages;
        }
        if (stage->texture.dynamic != DI_STATIC)
        {
            ++facts.dynamicImageStages;
        }
        if (stage->texture.cinematic != nullptr)
        {
            ++facts.cinematicStages;
        }
        if (cubeMap)
        {
            ++facts.cubeMapStages;
        }
        if (stage->texture.texgen == TG_REFLECT_CUBE2)
        {
            ++facts.reflectCube2Stages;
        }
        if (stage->newStage != nullptr)
        {
            ++facts.customProgramStages;
        }

        switch (stage->lighting)
        {
            case SL_BUMP:
                ++facts.bumpStages;
                if (facts.firstBumpStage < 0)
                {
                    facts.firstBumpStage = stageIndex;
                }
                break;
            case SL_DIFFUSE:
                ++facts.diffuseStages;
                if (facts.firstDiffuseStage < 0)
                {
                    facts.firstDiffuseStage = stageIndex;
                }
                break;
            case SL_SPECULAR:
                ++facts.specularStages;
                if (facts.firstSpecularStage < 0)
                {
                    facts.firstSpecularStage = stageIndex;
                }
                if (usage == TD_SPECULAR_PBR_RMAO || usage == TD_SPECULAR_PBR_RMAOD)
                {
                    ++facts.pbrRmaoStages;
                    if (facts.routeStage < 0)
                    {
                        facts.routeStage = stageIndex;
                    }
                }
                else if (image != nullptr)
                {
                    ++facts.legacySpecStages;
                    if (facts.routeStage < 0)
                    {
                        facts.routeStage = stageIndex;
                    }
                }
                break;
            case SL_COVERAGE:
                ++facts.coverageStages;
                break;
            case SL_AMBIENT:
                ++facts.ambientStages;
                break;
            default:
                break;
        }

        if (stage->lighting == SL_AMBIENT || additiveBlend || filterBlend || alphaBlend || guiOrScreen || cubeMap || stage->newStage != nullptr || stage->texture.cinematic != nullptr)
        {
            ++facts.effectStages;
        }
    }
    return facts;
}

RtMaterialDynamicFacts AnalyzeRtMaterialDynamicFacts(const idMaterial* material)
{
    RtMaterialDynamicFacts facts;
    if (!material)
    {
        return facts;
    }

    facts.materialUsesRuntimeRegisters = material->ConstantRegisters() == nullptr;
    facts.projectedDecal = material->GetSort() >= SS_DECAL && material->GetSort() < SS_FAR;

    for (int stageIndex = 0; stageIndex < material->GetNumStages(); ++stageIndex)
    {
        const shaderStage_t* stage = material->GetStage(stageIndex);
        if (!stage || !RtStageConditionCanBeActive(material, stage))
        {
            continue;
        }

        if (RtMaterialRegisterDependsOnRuntime(material, stage->conditionRegister))
        {
            ++facts.conditionRegisterStages;
        }

        bool colorDependsOnRuntime = false;
        for (int component = 0; component < 4; ++component)
        {
            colorDependsOnRuntime = colorDependsOnRuntime ||
                RtMaterialRegisterDependsOnRuntime(material, stage->color.registers[component]);
        }
        if (colorDependsOnRuntime)
        {
            ++facts.colorRegisterStages;
        }
        if (RtMaterialRegisterDependsOnRuntime(material, stage->color.registers[3]))
        {
            ++facts.alphaRegisterStages;
        }

        if (stage->hasAlphaTest &&
            RtMaterialRegisterDependsOnRuntime(material, stage->alphaTestRegister))
        {
            ++facts.alphaTestRegisterStages;
        }

        if (stage->texture.hasMatrix)
        {
            ++facts.textureMatrixStages;
            bool matrixDependsOnRuntime = false;
            for (int row = 0; row < 2; ++row)
            {
                for (int column = 0; column < 3; ++column)
                {
                    matrixDependsOnRuntime = matrixDependsOnRuntime ||
                        RtMaterialRegisterDependsOnRuntime(material, stage->texture.matrix[row][column]);
                }
            }
            if (matrixDependsOnRuntime)
            {
                ++facts.textureMatrixRegisterStages;

                idImage* image = stage->texture.image;
                const int imageWidth = image && image->GetUploadWidth() > 0 ? image->GetUploadWidth() : stage->texture.width;
                const int imageHeight = image && image->GetUploadHeight() > 0 ? image->GetUploadHeight() : stage->texture.height;
                const int shortDim = Min(imageWidth, imageHeight);
                const int longDim = Max(imageWidth, imageHeight);
                if (shortDim > 0 && longDim >= shortDim * 2)
                {
                    const int frames = Max(2, longDim / shortDim);
                    ++facts.flipbookAtlasStages;
                    facts.flipbookFrames = Max(facts.flipbookFrames, frames);
                    facts.flipbookAxis = imageWidth >= imageHeight ? 1 : 0;
                }
            }
        }

        if (stage->texture.dynamic != DI_STATIC || stage->texture.dynamicFrameCount > 0)
        {
            ++facts.dynamicImageStages;
        }
        if (stage->texture.cinematic != nullptr)
        {
            ++facts.cinematicStages;
        }
        if (stage->texture.texgen == TG_SCREEN ||
            stage->texture.texgen == TG_SCREEN2 ||
            SmokeStageIsRenderMap(stage))
        {
            ++facts.guiRenderTargetStages;
        }
        if (stage->newStage != nullptr)
        {
            ++facts.customProgramStages;
        }
    }

    return facts;
}

RtMaterialSurfaceClass RtSurfaceClassFromSurfaceType(surfTypes_t surfaceType)
{
    switch (surfaceType)
    {
        case SURFTYPE_METAL:
            return RtMaterialSurfaceClass::Metal;
        case SURFTYPE_STONE:
            return RtMaterialSurfaceClass::Stone;
        case SURFTYPE_FLESH:
            return RtMaterialSurfaceClass::Flesh;
        case SURFTYPE_WOOD:
            return RtMaterialSurfaceClass::Wood;
        case SURFTYPE_CARDBOARD:
            return RtMaterialSurfaceClass::Cardboard;
        case SURFTYPE_LIQUID:
            return RtMaterialSurfaceClass::Liquid;
        case SURFTYPE_GLASS:
            return RtMaterialSurfaceClass::Glass;
        case SURFTYPE_PLASTIC:
            return RtMaterialSurfaceClass::Plastic;
        case SURFTYPE_RICOCHET:
            return RtMaterialSurfaceClass::Ricochet;
        default:
            return RtMaterialSurfaceClass::Unknown;
    }
}

RtMaterialSurfaceClass RtSurfaceClassFromNameFallback(const idStr& materialName, idStr& evidence)
{
    static const RtMaterialClassTokenRule rules[] = {
        { "textures/decals/", RtMaterialSurfaceClass::Special, true },
        { "textures/decals2/", RtMaterialSurfaceClass::Special, true },
        { "textures/decalsd3xp/", RtMaterialSurfaceClass::Special, true },
        { "textures/skies/", RtMaterialSurfaceClass::Special, true },
        { "textures/sfx/", RtMaterialSurfaceClass::Special, true },
        { "textures/sfxd3xp/", RtMaterialSurfaceClass::Special, true },
        { "textures/particles/", RtMaterialSurfaceClass::Special, true },
        { "textures/particlesd3xp/", RtMaterialSurfaceClass::Special, true },
        { "textures/base_light/", RtMaterialSurfaceClass::Special, true },
        { "particles/", RtMaterialSurfaceClass::Special, true },
        { "lights/", RtMaterialSurfaceClass::Special, true },
        { "lights\\", RtMaterialSurfaceClass::Special, true },
        { "guiobjects", RtMaterialSurfaceClass::Special, true },
        { "guis/", RtMaterialSurfaceClass::Special, true },
        { "guis\\", RtMaterialSurfaceClass::Special, true },
        { "models/mapobjects/signs/", RtMaterialSurfaceClass::Special, true },
        { "models\\mapobjects\\signs\\", RtMaterialSurfaceClass::Special, true },
        { "symbol_letters", RtMaterialSurfaceClass::Special },
        { "spectrumdecal", RtMaterialSurfaceClass::Special },
        { "hologram", RtMaterialSurfaceClass::Special },
        { "emerlight", RtMaterialSurfaceClass::Special },
        { "textures/washroom/sign2", RtMaterialSurfaceClass::Special, true },
        { "textures/washroom/nosmoking", RtMaterialSurfaceClass::Special, true },
        { "decal", RtMaterialSurfaceClass::Special },
        { "splat", RtMaterialSurfaceClass::Special },
        { "stain", RtMaterialSurfaceClass::Special },
        { "bloodpool", RtMaterialSurfaceClass::Special },
        { "flare", RtMaterialSurfaceClass::Special },
        { "glow", RtMaterialSurfaceClass::Special },

        { "glass", RtMaterialSurfaceClass::Glass },
        { "window", RtMaterialSurfaceClass::Glass },
        { "visor", RtMaterialSurfaceClass::Glass },
        { "screen", RtMaterialSurfaceClass::Glass },
        { "textures/washroom/mirror", RtMaterialSurfaceClass::Glass, true },
        { "transparent", RtMaterialSurfaceClass::Glass },

        { "keycard", RtMaterialSurfaceClass::Plastic },
        { "keyboard", RtMaterialSurfaceClass::Plastic },
        { "computer", RtMaterialSurfaceClass::Plastic },
        { "monitor", RtMaterialSurfaceClass::Plastic },
        { "deskcomp", RtMaterialSurfaceClass::Plastic },
        { "laptop", RtMaterialSurfaceClass::Plastic },
        { "whiteboard", RtMaterialSurfaceClass::Plastic },
        { "phone", RtMaterialSurfaceClass::Plastic },
        { "/pda", RtMaterialSurfaceClass::Plastic, true },
        { "/cd", RtMaterialSurfaceClass::Plastic },
        { "medkit", RtMaterialSurfaceClass::Plastic },
        { "plastic", RtMaterialSurfaceClass::Plastic },
        { "poly", RtMaterialSurfaceClass::Plastic },
        { "hose", RtMaterialSurfaceClass::Plastic },
        { "foamcup", RtMaterialSurfaceClass::Plastic },
        { "cola", RtMaterialSurfaceClass::Plastic },
        { "duffle", RtMaterialSurfaceClass::Plastic },
        { "canvas", RtMaterialSurfaceClass::Plastic },
        { "cloth", RtMaterialSurfaceClass::Plastic },
        { "fabric", RtMaterialSurfaceClass::Plastic },
        { "leather", RtMaterialSurfaceClass::Plastic },

        { "cardboard", RtMaterialSurfaceClass::Cardboard },
        { "paper", RtMaterialSurfaceClass::Cardboard },
        { "magpage", RtMaterialSurfaceClass::Cardboard },
        { "textures/object/magpage", RtMaterialSurfaceClass::Cardboard, true },
        { "textures/object/magback", RtMaterialSurfaceClass::Cardboard, true },
        { "textures/object/mag1", RtMaterialSurfaceClass::Cardboard, true },
        { "textures/object/mag2", RtMaterialSurfaceClass::Cardboard, true },
        { "textures/object/mag3", RtMaterialSurfaceClass::Cardboard, true },
        { "textures/object/mag4", RtMaterialSurfaceClass::Cardboard, true },
        { "textures/object/mag5", RtMaterialSurfaceClass::Cardboard, true },
        { "textures/object/mag6", RtMaterialSurfaceClass::Cardboard, true },
        { "textures/object/post_it", RtMaterialSurfaceClass::Cardboard, true },
        { "noteboard", RtMaterialSurfaceClass::Cardboard },
        { "calendar", RtMaterialSurfaceClass::Cardboard },
        { "binder", RtMaterialSurfaceClass::Cardboard },

        { "flesh", RtMaterialSurfaceClass::Flesh },
        { "skin", RtMaterialSurfaceClass::Flesh },
        { "blood", RtMaterialSurfaceClass::Flesh },
        { "character", RtMaterialSurfaceClass::Flesh },
        { "/characters/", RtMaterialSurfaceClass::Flesh, true },
        { "\\characters\\", RtMaterialSurfaceClass::Flesh, true },

        { "wood", RtMaterialSurfaceClass::Wood },
        { "plank", RtMaterialSurfaceClass::Wood },

        { "liquid", RtMaterialSurfaceClass::Liquid },
        { "water", RtMaterialSurfaceClass::Liquid },
        { "slime", RtMaterialSurfaceClass::Liquid },
        { "fluid", RtMaterialSurfaceClass::Liquid },

        { "metal", RtMaterialSurfaceClass::Metal },
        { "textures/mcity/", RtMaterialSurfaceClass::Metal, true },
        { "textures/base_floor/", RtMaterialSurfaceClass::Metal, true },
        { "textures/base_wall/", RtMaterialSurfaceClass::Metal, true },
        { "textures/basewall/", RtMaterialSurfaceClass::Metal, true },
        { "textures/base_trim/", RtMaterialSurfaceClass::Metal, true },
        { "textures/basetrim/", RtMaterialSurfaceClass::Metal, true },
        { "textures/base_door/", RtMaterialSurfaceClass::Metal, true },
        { "textures/basedoor/", RtMaterialSurfaceClass::Metal, true },
        { "textures/alphalabs/", RtMaterialSurfaceClass::Metal, true },
        { "textures/enpro/", RtMaterialSurfaceClass::Metal, true },
        { "textures/outside/", RtMaterialSurfaceClass::Metal, true },
        { "textures/recycle_floor/", RtMaterialSurfaceClass::Metal, true },
        { "textures/recycle_wall/", RtMaterialSurfaceClass::Metal, true },
        { "models/mapobjects/enpro/", RtMaterialSurfaceClass::Metal, true },
        { "models/mapobjects/cpu/", RtMaterialSurfaceClass::Metal, true },
        { "models/mapobjects/doors/", RtMaterialSurfaceClass::Metal, true },
        { "models/mapobjects/kiosk/", RtMaterialSurfaceClass::Metal, true },
        { "models/mapobjects/skmachines/", RtMaterialSurfaceClass::Metal, true },
        { "models/mapobjects/swinglights/", RtMaterialSurfaceClass::Metal, true },
        { "steel", RtMaterialSurfaceClass::Metal },
        { "silver", RtMaterialSurfaceClass::Metal },
        { "gendesk", RtMaterialSurfaceClass::Metal },
        { "desk", RtMaterialSurfaceClass::Metal },
        { "table", RtMaterialSurfaceClass::Metal },
        { "counter", RtMaterialSurfaceClass::Metal },
        { "fridge", RtMaterialSurfaceClass::Metal },
        { "refrigerator", RtMaterialSurfaceClass::Metal },
        { "freezer", RtMaterialSurfaceClass::Metal },
        { "sink", RtMaterialSurfaceClass::Metal },
        { "fixture", RtMaterialSurfaceClass::Metal },
        { "lamp", RtMaterialSurfaceClass::Metal },
        { "textures/object/fan", RtMaterialSurfaceClass::Metal, true },
        { "textures/washroom/rfthinbeam", RtMaterialSurfaceClass::Metal, true },
        { "textures/washroom/rfcorner", RtMaterialSurfaceClass::Metal, true },
        { "gotcablecage", RtMaterialSurfaceClass::Metal },
        { "offcab", RtMaterialSurfaceClass::Metal },
        { "offdrawer", RtMaterialSurfaceClass::Metal },
        { "offlocker", RtMaterialSurfaceClass::Metal },
        { "sopbox", RtMaterialSurfaceClass::Metal },
        { "tbox", RtMaterialSurfaceClass::Metal },
        { "metalcrate", RtMaterialSurfaceClass::Metal },
        { "metal_crate", RtMaterialSurfaceClass::Metal },
        { "artifactcrate", RtMaterialSurfaceClass::Metal },
        { "artifact_crate", RtMaterialSurfaceClass::Metal },
        { "artifactcrates", RtMaterialSurfaceClass::Metal },
        { "artifact_crates", RtMaterialSurfaceClass::Metal },
        { "cube", RtMaterialSurfaceClass::Metal },
        { "diamondbox", RtMaterialSurfaceClass::Metal },
        { "pipe", RtMaterialSurfaceClass::Metal },
        { "grate", RtMaterialSurfaceClass::Metal },
        { "plate", RtMaterialSurfaceClass::Metal },
        { "panel", RtMaterialSurfaceClass::Metal },
        { "sflpanel", RtMaterialSurfaceClass::Metal },
        { "doortrim", RtMaterialSurfaceClass::Metal },
        { "doorframe", RtMaterialSurfaceClass::Metal },
        { "column", RtMaterialSurfaceClass::Metal },
        { "trim", RtMaterialSurfaceClass::Metal },
        { "vent", RtMaterialSurfaceClass::Metal },
        { "rack", RtMaterialSurfaceClass::Metal },
        { "cabinet", RtMaterialSurfaceClass::Metal },
        { "filecabinet", RtMaterialSurfaceClass::Metal },
        { "cart", RtMaterialSurfaceClass::Metal },
        { "heater", RtMaterialSurfaceClass::Metal },
        { "machine", RtMaterialSurfaceClass::Metal },
        { "modconsole", RtMaterialSurfaceClass::Metal },
        { "modsides", RtMaterialSurfaceClass::Metal },
        { "tecserver", RtMaterialSurfaceClass::Metal },
        { "wheels", RtMaterialSurfaceClass::Metal },
        { "chair", RtMaterialSurfaceClass::Metal },
        { "stool", RtMaterialSurfaceClass::Metal },
        { "seat", RtMaterialSurfaceClass::Metal },

        { "stone", RtMaterialSurfaceClass::Stone },
        { "textures/washroom/btile", RtMaterialSurfaceClass::Stone, true },
        { "textures/washroom/greenbtile", RtMaterialSurfaceClass::Stone, true },
        { "textures/washroom/rfceilingtile", RtMaterialSurfaceClass::Stone, true },
        { "rock", RtMaterialSurfaceClass::Stone },
        { "concrete", RtMaterialSurfaceClass::Stone },
        { "brick", RtMaterialSurfaceClass::Stone }
    };
    return RtSurfaceClassFromTokenRules(materialName, rules, sizeof(rules) / sizeof(rules[0]), "material", evidence);
}

RtMaterialSurfaceClass RtSurfaceClassFromImageNameFallback(const RtSmokeMaterialTextureInfo& info, idStr& evidence)
{
    RtMaterialSurfaceClass surfaceClass = RtSurfaceClassFromNameFallback(info.diffuseImageName, evidence);
    if (surfaceClass != RtMaterialSurfaceClass::Unknown)
    {
        evidence = RtMaterialFieldEvidence("diffuseImage", evidence);
        return surfaceClass;
    }
    surfaceClass = RtSurfaceClassFromNameFallback(info.normalImageName, evidence);
    if (surfaceClass != RtMaterialSurfaceClass::Unknown)
    {
        evidence = RtMaterialFieldEvidence("normalImage", evidence);
        return surfaceClass;
    }
    surfaceClass = RtSurfaceClassFromNameFallback(info.specularImageName, evidence);
    if (surfaceClass != RtMaterialSurfaceClass::Unknown)
    {
        evidence = RtMaterialFieldEvidence("specularImage", evidence);
        return surfaceClass;
    }
    surfaceClass = RtSurfaceClassFromNameFallback(info.emissiveImageName, evidence);
    if (surfaceClass != RtMaterialSurfaceClass::Unknown)
    {
        evidence = RtMaterialFieldEvidence("emissiveImage", evidence);
        return surfaceClass;
    }
    evidence = "";
    return RtMaterialSurfaceClass::Unknown;
}

bool RtMaterialTextHasNonMetalOverrideToken(const idStr& text)
{
    static const char* nonMetalTokens[] = {
        "textures/decals/",
        "textures/decals2/",
        "textures/decalsd3xp/",
        "textures/skies/",
        "textures/sfx/",
        "textures/sfxd3xp/",
        "textures/particles/",
        "textures/particlesd3xp/",
        "textures/base_light/",
        "particles/",
        "lights/",
        "lights\\",
        "guiobjects",
        "guis/",
        "guis\\",
        "glass",
        "window",
        "visor",
        "screen",
        "transparent"
    };

    for (int tokenIndex = 0; tokenIndex < static_cast<int>(sizeof(nonMetalTokens) / sizeof(nonMetalTokens[0])); ++tokenIndex)
    {
        if (RtMaterialTextHasToken(text, nonMetalTokens[tokenIndex]))
        {
            return true;
        }
    }

    return false;
}

bool RtIndustrialMetalFamilyFromNames(const RtSmokeMaterialTextureInfo& info, idStr& evidence)
{
    static const RtMaterialClassTokenRule rules[] = {
        { "textures/mcity/", RtMaterialSurfaceClass::Metal, true },
        { "textures/base_floor/", RtMaterialSurfaceClass::Metal, true },
        { "textures/base_wall/", RtMaterialSurfaceClass::Metal, true },
        { "textures/basewall/", RtMaterialSurfaceClass::Metal, true },
        { "textures/base_trim/", RtMaterialSurfaceClass::Metal, true },
        { "textures/basetrim/", RtMaterialSurfaceClass::Metal, true },
        { "textures/base_door/", RtMaterialSurfaceClass::Metal, true },
        { "textures/basedoor/", RtMaterialSurfaceClass::Metal, true },
        { "textures/alphalabs/", RtMaterialSurfaceClass::Metal, true },
        { "textures/enpro/", RtMaterialSurfaceClass::Metal, true },
        { "textures/outside/", RtMaterialSurfaceClass::Metal, true },
        { "textures/recycle_floor/", RtMaterialSurfaceClass::Metal, true },
        { "textures/recycle_wall/", RtMaterialSurfaceClass::Metal, true },
        { "models/mapobjects/enpro/", RtMaterialSurfaceClass::Metal, true },
        { "models/mapobjects/cpu/", RtMaterialSurfaceClass::Metal, true },
        { "models/mapobjects/doors/", RtMaterialSurfaceClass::Metal, true },
        { "models/mapobjects/kiosk/", RtMaterialSurfaceClass::Metal, true },
    };

    const idStr* fields[] = {
        &info.materialName,
        &info.diffuseImageName,
        &info.normalImageName,
        &info.specularImageName,
        &info.emissiveImageName
    };
    static const char* fieldNames[] = {
        "material",
        "diffuseImage",
        "normalImage",
        "specularImage",
        "emissiveImage"
    };

    for (int fieldIndex = 0; fieldIndex < 5; ++fieldIndex)
    {
        if (fields[fieldIndex]->IsEmpty() || RtMaterialTextHasNonMetalOverrideToken(*fields[fieldIndex]))
        {
            continue;
        }

        idStr familyEvidence;
        const RtMaterialSurfaceClass surfaceClass = RtSurfaceClassFromTokenRules(*fields[fieldIndex], rules, sizeof(rules) / sizeof(rules[0]), fieldNames[fieldIndex], familyEvidence);
        if (surfaceClass == RtMaterialSurfaceClass::Metal)
        {
            evidence = familyEvidence;
            return true;
        }
    }

    return false;
}


bool RtCharacterFleshFromNames(const RtSmokeMaterialTextureInfo& info, idStr& evidence)
{
    const idStr* fields[] = {
        &info.materialName,
        &info.diffuseImageName,
        &info.normalImageName,
        &info.specularImageName,
        &info.emissiveImageName
    };
    static const char* fieldNames[] = {
        "material",
        "diffuseImage",
        "normalImage",
        "specularImage",
        "emissiveImage"
    };
    static const char* characterTokens[] = {
        "/characters/",
        "\\characters\\",
        "models/characters/",
        "models\\characters\\"
    };

    for (int fieldIndex = 0; fieldIndex < 5; ++fieldIndex)
    {
        if (fields[fieldIndex]->IsEmpty())
        {
            continue;
        }
        for (int tokenIndex = 0; tokenIndex < static_cast<int>(sizeof(characterTokens) / sizeof(characterTokens[0])); ++tokenIndex)
        {
            if (RtMaterialTextHasToken(*fields[fieldIndex], characterTokens[tokenIndex]))
            {
                evidence = va("%s:%s", fieldNames[fieldIndex], characterTokens[tokenIndex]);
                return true;
            }
        }
    }

    return false;
}

RtMaterialSurfaceClass RtExplicitNonMetalClassFromNames(const RtSmokeMaterialTextureInfo& info, idStr& evidence)
{
    const idStr* fields[] = {
        &info.materialName,
        &info.diffuseImageName,
        &info.normalImageName,
        &info.specularImageName,
        &info.emissiveImageName
    };
    static const char* fieldNames[] = {
        "material",
        "diffuseImage",
        "normalImage",
        "specularImage",
        "emissiveImage"
    };

    for (int fieldIndex = 0; fieldIndex < 5; ++fieldIndex)
    {
        if (fields[fieldIndex]->IsEmpty())
        {
            continue;
        }

        idStr fieldEvidence;
        const RtMaterialSurfaceClass surfaceClass = RtSurfaceClassFromNameFallback(*fields[fieldIndex], fieldEvidence);
        if (surfaceClass != RtMaterialSurfaceClass::Unknown && surfaceClass != RtMaterialSurfaceClass::Metal)
        {
            evidence = RtMaterialFieldEvidence(fieldNames[fieldIndex], fieldEvidence);
            return surfaceClass;
        }
    }

    return RtMaterialSurfaceClass::Unknown;
}

bool RtFieldHasAnyToken(const idStr& text, const char* const* tokens, int tokenCount)
{
    if (text.IsEmpty())
    {
        return false;
    }

    for (int tokenIndex = 0; tokenIndex < tokenCount; ++tokenIndex)
    {
        if (RtMaterialTextHasToken(text, tokens[tokenIndex]))
        {
            return true;
        }
    }

    return false;
}

bool RtSpecialStageIntentFromNames(const RtSmokeMaterialTextureInfo& info, const RtMaterialStageFacts& stageFacts, idStr& evidence)
{
    const bool hasSpecialStageShape =
        info.emissive ||
        stageFacts.additiveBlendStages > 0 ||
        stageFacts.filterBlendStages > 0 ||
        stageFacts.alphaBlendStages > 0 ||
        stageFacts.coverageStages > 0;
    if (!hasSpecialStageShape)
    {
        return false;
    }

    static const char* specialFolderTokens[] = {
        "textures/decals/",
        "textures/decals2/",
        "textures/decalsd3xp/",
        "textures/sfx/",
        "textures/sfxd3xp/",
        "textures/particles/",
        "textures/particlesd3xp/",
        "textures/base_light/",
        "particles/",
        "lights/",
        "lights\\",
        "guis/",
        "guis\\"
    };
    static const char* effectLeafTokens[] = {
        "_add",
        "decal",
        "splat",
        "stain",
        "bloodpool",
        "light",
        "symbol",
        "letter",
        "screen",
        "monitor",
        "terminal",
        "console",
        "video",
        "cinematic",
        "cursor",
        "flare",
        "glow",
        "beam",
        "bolt",
        "spark",
        "smoke",
        "steam",
        "fog",
        "fire",
        "flame",
        "plasma",
        "mflash",
        "muzzle",
        "sprite",
        "hologram",
        "volum"
    };

    const idStr* fields[] = {
        &info.materialName,
        &info.diffuseImageName,
        &info.normalImageName,
        &info.specularImageName,
        &info.emissiveImageName
    };
    static const char* fieldNames[] = {
        "material",
        "diffuseImage",
        "normalImage",
        "specularImage",
        "emissiveImage"
    };

    for (int fieldIndex = 0; fieldIndex < 5; ++fieldIndex)
    {
        if (RtFieldHasAnyToken(*fields[fieldIndex], specialFolderTokens, static_cast<int>(sizeof(specialFolderTokens) / sizeof(specialFolderTokens[0]))))
        {
            evidence = va("stageSpecial:%s:folder", fieldNames[fieldIndex]);
            return true;
        }
    }

    const bool hasCompleteLitSurfaceStages =
        stageFacts.bumpStages > 0 &&
        stageFacts.diffuseStages > 0 &&
        stageFacts.specularStages > 0;

    if (info.emissive &&
        !hasCompleteLitSurfaceStages &&
        RtFieldHasAnyToken(info.emissiveImageName, effectLeafTokens, static_cast<int>(sizeof(effectLeafTokens) / sizeof(effectLeafTokens[0]))))
    {
        evidence = "stageSpecial:emissiveImage";
        return true;
    }
    if (info.emissive &&
        stageFacts.additiveBlendStages > 0 &&
        stageFacts.diffuseStages == 0 &&
        stageFacts.specularStages == 0)
    {
        evidence = "stageSpecial:emissiveAdditiveOnly";
        return true;
    }

    const bool hasTranslucentOrEffectBlend =
        stageFacts.additiveBlendStages > 0 ||
        stageFacts.filterBlendStages > 0 ||
        stageFacts.alphaBlendStages > 0 ||
        stageFacts.coverageStages > 0;
    if (!hasCompleteLitSurfaceStages && hasTranslucentOrEffectBlend)
    {
        for (int fieldIndex = 0; fieldIndex < 5; ++fieldIndex)
        {
            const idStr leaf = RtMaterialLeafName(*fields[fieldIndex]);
            if (RtFieldHasAnyToken(leaf, effectLeafTokens, static_cast<int>(sizeof(effectLeafTokens) / sizeof(effectLeafTokens[0]))))
            {
                evidence = va("stageSpecial:%s:effectToken", fieldNames[fieldIndex]);
                return true;
            }
        }
    }

    return false;
}

bool RtMaterialSortIsDecal(const idMaterial* material)
{
    if (!material)
    {
        return false;
    }

    const float sort = material->GetSort();
    return sort >= SS_DECAL && sort < SS_FAR;
}

bool RtMaterialSpecialFromStageFacts(const RtMaterialStageFacts& stageFacts, idStr& evidence)
{
    if (stageFacts.guiOrScreenStages > 0)
    {
        evidence = "stage:guiScreen";
        return true;
    }
    if (stageFacts.cinematicStages > 0)
    {
        evidence = "stage:cinematic";
        return true;
    }
    if (stageFacts.cubeMapStages > 0)
    {
        evidence = "stage:cubeMap";
        return true;
    }
    if (stageFacts.customProgramStages > 0)
    {
        evidence = "stage:program";
        return true;
    }
    return false;
}

RtMaterialClassCandidate ResolveSurfaceClassCandidate(const idMaterial* material, const RtSmokeMaterialTextureInfo& info, const RtMaterialStageFacts& stageFacts)
{
    RtMaterialClassCandidate candidate;
    const RtMaterialSurfaceClass surfaceTypeClass = RtSurfaceClassFromSurfaceType(material ? material->GetSurfaceType() : SURFTYPE_NONE);
    if (surfaceTypeClass != RtMaterialSurfaceClass::Unknown)
    {
        candidate.surfaceClass = surfaceTypeClass;
        candidate.reason = RtMaterialSurfaceClassReason::SurfaceType;
        candidate.confidence = RtMaterialClassConfidence::Authoritative;
        candidate.evidence = va("surfaceType:%d", material ? static_cast<int>(material->GetSurfaceType()) : static_cast<int>(SURFTYPE_NONE));
        return candidate;
    }

    if (RtMaterialSortIsDecal(material))
    {
        candidate.surfaceClass = RtMaterialSurfaceClass::Special;
        candidate.reason = RtMaterialSurfaceClassReason::MaterialSort;
        candidate.confidence = RtMaterialClassConfidence::Authoritative;
        candidate.evidence = va("sort:%.2f", material ? material->GetSort() : SS_BAD);
        return candidate;
    }

    if (RtMaterialSpecialFromStageFacts(stageFacts, candidate.evidence))
    {
        candidate.surfaceClass = RtMaterialSurfaceClass::Special;
        candidate.reason = RtMaterialSurfaceClassReason::StageKind;
        candidate.confidence = RtMaterialClassConfidence::Authoritative;
        return candidate;
    }

    if (RtSpecialStageIntentFromNames(info, stageFacts, candidate.evidence))
    {
        candidate.surfaceClass = RtMaterialSurfaceClass::Special;
        candidate.reason = RtMaterialSurfaceClassReason::StageKind;
        candidate.confidence = RtMaterialClassConfidence::Heuristic;
        return candidate;
    }

    if (RtCharacterFleshFromNames(info, candidate.evidence))
    {
        candidate.surfaceClass = RtMaterialSurfaceClass::Flesh;
        candidate.reason = RtMaterialSurfaceClassReason::NameToken;
        candidate.confidence = RtMaterialClassConfidence::Heuristic;
        return candidate;
    }

    candidate.surfaceClass = RtExplicitNonMetalClassFromNames(info, candidate.evidence);
    if (candidate.surfaceClass != RtMaterialSurfaceClass::Unknown)
    {
        candidate.reason = RtMaterialSurfaceClassReason::NameToken;
        candidate.confidence = RtMaterialClassConfidence::Heuristic;
        return candidate;
    }

    if (RtIndustrialMetalFamilyFromNames(info, candidate.evidence))
    {
        candidate.surfaceClass = RtMaterialSurfaceClass::Metal;
        candidate.reason = RtMaterialSurfaceClassReason::NameToken;
        candidate.confidence = RtMaterialClassConfidence::Heuristic;
        return candidate;
    }

    candidate.surfaceClass = RtSurfaceClassFromNameFallback(info.materialName, candidate.evidence);
    if (candidate.surfaceClass != RtMaterialSurfaceClass::Unknown)
    {
        candidate.reason = RtMaterialSurfaceClassReason::NameToken;
        candidate.confidence = RtMaterialClassConfidence::Heuristic;
        return candidate;
    }

    candidate.surfaceClass = RtSurfaceClassFromImageNameFallback(info, candidate.evidence);
    if (candidate.surfaceClass != RtMaterialSurfaceClass::Unknown)
    {
        candidate.reason = RtMaterialSurfaceClassReason::ImageNameToken;
        candidate.confidence = RtMaterialClassConfidence::Heuristic;
        return candidate;
    }

    candidate.surfaceClass = RtMaterialSurfaceClass::Metal;
    candidate.reason = RtMaterialSurfaceClassReason::FallbackMetal;
    candidate.confidence = RtMaterialClassConfidence::FallbackNone;
    candidate.evidence = "fallback:doomIndustrialDefault";
    return candidate;
}

RtMaterialBsdfParams RtDefaultBsdfForSurfaceClass(RtMaterialSurfaceClass surfaceClass)
{
    RtMaterialBsdfParams bsdf;
    switch (surfaceClass)
    {
        case RtMaterialSurfaceClass::Metal:
            bsdf.metallic = 1.0f;
            bsdf.roughness = 0.25f;
            break;
        case RtMaterialSurfaceClass::Stone:
            bsdf.roughness = 0.85f;
            break;
        case RtMaterialSurfaceClass::Wood:
            bsdf.roughness = 0.75f;
            break;
        case RtMaterialSurfaceClass::Cardboard:
            bsdf.roughness = 0.95f;
            break;
        case RtMaterialSurfaceClass::Liquid:
            bsdf.roughness = 0.02f;
            bsdf.ior = 1.33f;
            bsdf.transmission = 0.9f;
            bsdf.twoSidedBsdf = 1;
            break;
        case RtMaterialSurfaceClass::Glass:
            bsdf.roughness = 0.05f;
            bsdf.ior = 1.5f;
            bsdf.transmission = 0.9f;
            bsdf.twoSidedBsdf = 1;
            break;
        case RtMaterialSurfaceClass::Plastic:
            bsdf.roughness = 0.45f;
            break;
        case RtMaterialSurfaceClass::Flesh:
            bsdf.roughness = 0.55f;
            bsdf.ior = 1.4f;
            bsdf.subsurfaceHint = 1;
            break;
        case RtMaterialSurfaceClass::Ricochet:
            bsdf.roughness = 0.6f;
            break;
        case RtMaterialSurfaceClass::Special:
            bsdf.roughness = 0.7f;
            break;
        default:
            break;
    }
    return bsdf;
}

RtMaterialNormalDecodeMode ResolveNormalDecodeMode(const RtSmokeMaterialTextureInfo& info)
{
    if (!info.hasNormalImage)
    {
        return RtMaterialNormalDecodeMode::None;
    }
    if (r_pathTracingMatClassNormalDecodeMode.GetInteger() == 1)
    {
        return RtMaterialNormalDecodeMode::Rgb8Rg;
    }
    if (r_pathTracingMatClassNormalDecodeMode.GetInteger() == 2)
    {
        return RtMaterialNormalDecodeMode::CompressedWy;
    }
    return info.normalColorFormat == CFM_NORMAL_DXT5 ? RtMaterialNormalDecodeMode::CompressedWy : RtMaterialNormalDecodeMode::Rgb8Rg;
}

RtMaterialBsdfRoute ResolveBsdfRoute(const RtSmokeMaterialTextureInfo& info, const RtMaterialStageFacts& stageFacts, RtMaterialBsdfRouteReason& routeReason)
{
    (void)info;
    if (r_pathTracingMatClassUseRmao.GetInteger() != 0 && stageFacts.pbrRmaoStages > 0)
    {
        routeReason = RtMaterialBsdfRouteReason::PbrRmaoStage;
        return RtMaterialBsdfRoute::RealPbrRmao;
    }
    if (stageFacts.legacySpecStages > 0)
    {
        routeReason = RtMaterialBsdfRouteReason::LegacySpecularStage;
        return RtMaterialBsdfRoute::LegacySpecGloss;
    }
    if (info.hasSpecularImage &&
        info.specularUsage != TD_SPECULAR_PBR_RMAO &&
        info.specularUsage != TD_SPECULAR_PBR_RMAOD)
    {
        routeReason = RtMaterialBsdfRouteReason::DiscoveredLegacySpecularImage;
        return RtMaterialBsdfRoute::LegacySpecGloss;
    }
    if (stageFacts.pbrRmaoStages > 0)
    {
        routeReason = RtMaterialBsdfRouteReason::RmaoDisabled;
    }
    else
    {
        routeReason = RtMaterialBsdfRouteReason::NoCompatibleSpecularStage;
    }
    return RtMaterialBsdfRoute::SurfaceTypeFallback;
}

uint32_t QuantizeRtMaterialUnitFloat(float value)
{
    const float clamped = idMath::ClampFloat(0.0f, 1.0f, value);
    return static_cast<uint32_t>(clamped * 255.0f + 0.5f);
}

void ApplyRouteBBsdfPolicy(const idMaterial* material, RtMaterialRecord& record)
{
    if (record.route != RtMaterialBsdfRoute::LegacySpecGloss)
    {
        if (record.route == RtMaterialBsdfRoute::RealPbrRmao)
        {
            record.bsdfEvidence = "routeA:rmao";
        }
        else
        {
            record.bsdfEvidence = "routeC:classDefault";
        }
        return;
    }

    record.bsdfEvidence = "routeB:shaderPerPixelSpec";
}

uint64 ComputeRtMaterialRecordSignature(const idMaterial* material, const RtSmokeMaterialTextureInfo& info)
{
    uint64 hash = 1469598103934665603ull;
    hash = HashRtMaterialValue(hash, info.materialId);
    hash = HashRtMaterialString(hash, info.materialName);
    hash = HashRtMaterialString(hash, info.diffuseImageName);
    hash = HashRtMaterialString(hash, info.normalImageName);
    hash = HashRtMaterialString(hash, info.specularImageName);
    hash = HashRtMaterialString(hash, info.emissiveImageName);
    hash = HashRtMaterialString(hash, info.fallbackReason);
    hash = HashRtMaterialString(hash, info.normalReason);
    hash = HashRtMaterialString(hash, info.specularReason);
    hash = HashRtMaterialString(hash, info.emissiveReason);
    hash = HashRtMaterialString(hash, info.liquidFilmCoverageImageName);
    hash = HashRtMaterialString(hash, info.liquidFilmOverrideReason);
    hash = HashRtMaterialString(hash, info.liquidFilmReason);
    hash = HashRtMaterialValue(hash, material ? static_cast<uint64>(material->GetSurfaceType()) : 0u);
    hash = HashRtMaterialValue(hash, material ? static_cast<uint64>(material->GetSurfaceFlags()) : 0u);
    hash = HashRtMaterialFloat(hash, material ? material->GetSort() : 0.0f);
    hash = HashRtMaterialValue(hash, static_cast<uint64>(info.coverage));
    hash = HashRtMaterialValue(hash, static_cast<uint64>(info.diffuseUsage));
    hash = HashRtMaterialValue(hash, static_cast<uint64>(info.normalUsage));
    hash = HashRtMaterialValue(hash, static_cast<uint64>(info.specularUsage));
    hash = HashRtMaterialValue(hash, static_cast<uint64>(info.emissiveUsage));
    hash = HashRtMaterialValue(hash, static_cast<uint64>(info.diffuseColorFormat));
    hash = HashRtMaterialValue(hash, static_cast<uint64>(info.normalColorFormat));
    hash = HashRtMaterialValue(hash, static_cast<uint64>(info.specularColorFormat));
    hash = HashRtMaterialValue(hash, info.hasAlphaTest ? 1u : 0u);
    hash = HashRtMaterialFloat(hash, info.alphaCutoff);
    hash = HashRtMaterialValue(hash, info.alphaFromDiffuseLuma ? 1u : 0u);
    hash = HashRtMaterialValue(hash, info.alphaFromDiffuseDarkKey ? 1u : 0u);
    hash = HashRtMaterialValue(hash, info.alphaFromDiffuseMagentaKey ? 1u : 0u);
    hash = HashRtMaterialValue(hash, info.detailDecal ? 1u : 0u);
    hash = HashRtMaterialValue(hash, info.detailDecalLiquidPool ? 1u : 0u);
    hash = HashRtMaterialValue(hash, info.liquidFilmHasBloodSemantic ? 1u : 0u);
    hash = HashRtMaterialValue(hash, info.liquidFilmHasWetReflectStage ? 1u : 0u);
    hash = HashRtMaterialValue(hash, info.liquidFilmHasCoverageSource ? 1u : 0u);
    hash = HashRtMaterialValue(hash, info.liquidFilmHasWetNormalSource ? 1u : 0u);
    hash = HashRtMaterialValue(hash, info.liquidFilmExactOverride ? 1u : 0u);
    hash = HashRtMaterialValue(hash, info.liquidFilmCandidate ? 1u : 0u);
    hash = HashRtMaterialValue(hash, IsSmokeMaterialTextureVariant(info.materialId) ? 1u : 0u);
    hash = HashRtMaterialValue(hash, info.isDynamic ? 1u : 0u);
    hash = HashRtMaterialValue(hash, material ? static_cast<uint64>(material->GetNumStages()) : 0u);
    if (material)
    {
        const float* constantRegisters = material->ConstantRegisters();
        const int registerCount = material->GetNumRegisters();
        for (int stageIndex = 0; stageIndex < material->GetNumStages(); ++stageIndex)
        {
            const shaderStage_t* stage = material->GetStage(stageIndex);
            hash = HashRtMaterialValue(hash, stage ? static_cast<uint64>(stage->lighting) : 0u);
            hash = HashRtMaterialValue(hash, stage && stage->texture.image ? static_cast<uint64>(stage->texture.image->GetUsage()) : 0u);
            hash = HashRtMaterialValue(hash, stage && stage->texture.image ? static_cast<uint64>(stage->texture.image->GetOpts().colorFormat) : 0u);
            hash = HashRtMaterialValue(hash, stage ? stage->drawStateBits : 0u);
            hash = HashRtMaterialValue(hash, stage ? static_cast<uint64>(stage->texture.texgen) : 0u);
            hash = HashRtMaterialValue(hash, stage ? static_cast<uint64>(stage->texture.dynamic) : 0u);
            hash = HashRtMaterialValue(hash, stage ? static_cast<uint64>(stage->texture.dynamicFrameCount) : 0u);
            hash = HashRtMaterialValue(hash, stage && stage->texture.hasMatrix ? 1u : 0u);
            hash = HashRtMaterialValue(hash, stage && stage->newStage ? 1u : 0u);
            hash = HashRtMaterialValue(hash, stage && stage->texture.cinematic ? 1u : 0u);
            if (stage)
            {
                hash = HashRtMaterialValue(hash, static_cast<uint64>(stage->conditionRegister));
                hash = HashRtMaterialValue(hash, stage->hasAlphaTest ? 1u : 0u);
                hash = HashRtMaterialValue(hash, static_cast<uint64>(stage->alphaTestRegister));
                if (constantRegisters && stage->conditionRegister >= 0 && stage->conditionRegister < registerCount)
                {
                    hash = HashRtMaterialFloat(hash, constantRegisters[stage->conditionRegister]);
                }
                if (constantRegisters && stage->alphaTestRegister >= 0 && stage->alphaTestRegister < registerCount)
                {
                    hash = HashRtMaterialFloat(hash, constantRegisters[stage->alphaTestRegister]);
                }
                for (int component = 0; component < 4; ++component)
                {
                    const int registerIndex = stage->color.registers[component];
                    hash = HashRtMaterialValue(hash, static_cast<uint64>(registerIndex));
                    if (constantRegisters && registerIndex >= 0 && registerIndex < registerCount)
                    {
                        hash = HashRtMaterialFloat(hash, constantRegisters[registerIndex]);
                    }
                }
                if (stage->texture.hasMatrix)
                {
                    for (int row = 0; row < 2; ++row)
                    {
                        for (int column = 0; column < 3; ++column)
                        {
                            const int registerIndex = stage->texture.matrix[row][column];
                            hash = HashRtMaterialValue(hash, static_cast<uint64>(registerIndex));
                            if (constantRegisters && registerIndex >= 0 && registerIndex < registerCount)
                            {
                                hash = HashRtMaterialFloat(hash, constantRegisters[registerIndex]);
                            }
                        }
                    }
                }
            }
        }
    }
    hash = HashRtMaterialValue(hash, info.hasAlphaTest ? 1u : 0u);
    hash = HashRtMaterialValue(hash, info.emissive ? 1u : 0u);
    hash = HashRtMaterialValue(hash, static_cast<uint64>(r_pathTracingMatClassUseRmao.GetInteger() != 0 ? 1 : 0));
    hash = HashRtMaterialValue(hash, static_cast<uint64>(idMath::ClampInt(0, 2, r_pathTracingMatClassNormalDecodeMode.GetInteger())));
    return hash;
}

RtMaterialRecord BuildRtMaterialRecord(const idMaterial* material, const RtSmokeMaterialTextureInfo& info, uint64 signature)
{
    RtMaterialRecord record;
    record.valid = true;
    record.signature = signature;
    record.materialId = info.materialId;
    record.materialName = info.materialName;
    record.diffuseImageName = info.diffuseImageName;
    record.normalImageName = info.normalImageName;
    record.specularImageName = info.specularImageName;
    record.emissiveImageName = info.emissiveImageName;
    record.diffuseReason = info.fallbackReason;
    record.normalReason = info.normalReason;
    record.specularReason = info.specularReason;
    record.emissiveReason = info.emissiveReason;
    record.liquidFilmCoverageImageName = info.liquidFilmCoverageImageName;
    record.liquidFilmOverrideReason = info.liquidFilmOverrideReason;
    record.liquidFilmReason = info.liquidFilmReason;
    record.rawSurfaceType = material ? static_cast<int>(material->GetSurfaceType()) : static_cast<int>(SURFTYPE_NONE);
    record.surfaceFlags = material ? material->GetSurfaceFlags() : 0;
    record.sort = material ? material->GetSort() : 0.0f;
    record.coverage = info.coverage;
    record.hasDiffuseImage = info.hasDiffuseImage;
    record.hasNormalImage = info.hasNormalImage;
    record.hasSpecularImage = info.hasSpecularImage;
    record.hasEmissiveImage = info.hasEmissiveImage;
    record.diffuseUsage = info.diffuseUsage;
    record.normalUsage = info.normalUsage;
    record.specularUsage = info.specularUsage;
    record.emissiveUsage = info.emissiveUsage;
    record.diffuseColorFormat = info.diffuseColorFormat;
    record.normalColorFormat = info.normalColorFormat;
    record.specularColorFormat = info.specularColorFormat;
    record.emissiveColorFormat = info.emissiveColorFormat;
    record.alphaTested = info.hasAlphaTest;
    record.emissiveIntent = info.emissive;
    record.liquidFilmIsDetailDecal = info.detailDecal;
    record.liquidFilmHasBloodSemantic = info.liquidFilmHasBloodSemantic;
    record.liquidFilmHasWetReflectStage = info.liquidFilmHasWetReflectStage;
    record.liquidFilmHasCoverageSource = info.liquidFilmHasCoverageSource;
    record.liquidFilmHasWetNormalSource = info.liquidFilmHasWetNormalSource;
    record.liquidFilmExactOverride = info.liquidFilmExactOverride;
    record.liquidFilmLegacyPool = info.detailDecalLiquidPool;
    record.liquidFilmCandidate = info.liquidFilmCandidate;
    record.liquidFilmVariant = IsSmokeMaterialTextureVariant(info.materialId);
    record.liquidFilmDynamic = info.isDynamic;
    record.stageFacts = AnalyzeRtMaterialStages(material);
    record.compositingStages = CompileRtMaterialCompositingStages(material);
    record.interactionPackets = CompileRtMaterialInteractionPackets(record.compositingStages);
    record.dynamicFacts = AnalyzeRtMaterialDynamicFacts(material);

    const RtMaterialClassCandidate surfaceCandidate = ResolveSurfaceClassCandidate(material, info, record.stageFacts);
    record.surfaceClass = surfaceCandidate.surfaceClass;
    record.surfaceClassConfidence = surfaceCandidate.confidence;
    record.surfaceClassReason = surfaceCandidate.reason;
    record.surfaceClassEvidence = surfaceCandidate.evidence;

    record.route = ResolveBsdfRoute(info, record.stageFacts, record.routeReason);
    record.normalDecodeMode = ResolveNormalDecodeMode(info);
    record.bsdf = RtDefaultBsdfForSurfaceClass(record.surfaceClass);
    if (record.route == RtMaterialBsdfRoute::RealPbrRmao)
    {
        record.bsdf.roughness = 0.05f;
        record.bsdf.metallic = 0.0f;
        record.bsdf.ao = 1.0f;
    }
    else if (record.route == RtMaterialBsdfRoute::LegacySpecGloss)
    {
        record.bsdf.metallic =
            record.surfaceClass == RtMaterialSurfaceClass::Metal ||
            record.surfaceClass == RtMaterialSurfaceClass::Ricochet ? 1.0f : 0.0f;
    }
    ApplyRouteBBsdfPolicy(material, record);
    return record;
}

void AccumulateRecordStats(const RtMaterialRecord& record)
{
    switch (record.route)
    {
        case RtMaterialBsdfRoute::RealPbrRmao:
            ++g_materialClassifierStats.routeRealPbr;
            break;
        case RtMaterialBsdfRoute::LegacySpecGloss:
            ++g_materialClassifierStats.routeLegacySpec;
            break;
        case RtMaterialBsdfRoute::SurfaceTypeFallback:
            ++g_materialClassifierStats.routeFallback;
            break;
        default:
            break;
    }
    switch (record.surfaceClassConfidence)
    {
        case RtMaterialClassConfidence::Authoritative:
            ++g_materialClassifierStats.confidenceAuthoritative;
            break;
        case RtMaterialClassConfidence::Flag:
            ++g_materialClassifierStats.confidenceFlag;
            break;
        case RtMaterialClassConfidence::Heuristic:
            ++g_materialClassifierStats.confidenceHeuristic;
            break;
        case RtMaterialClassConfidence::FallbackNone:
            ++g_materialClassifierStats.confidenceFallbackNone;
            break;
    }
}

const char* RtMaterialStageRouteEvidence(const shaderStage_t* stage)
{
    if (!stage || stage->lighting != SL_SPECULAR || !stage->texture.image)
    {
        return "-";
    }

    const textureUsage_t usage = stage->texture.image->GetUsage();
    if (usage == TD_SPECULAR_PBR_RMAO || usage == TD_SPECULAR_PBR_RMAOD)
    {
        return "routeA_rmao";
    }
    return "routeB_specular";
}

void MaybeDumpRecordStages(const RtMaterialRecord& record)
{
    if (r_pathTracingMatClassDebugList.GetInteger() < 3)
    {
        return;
    }

    const idMaterial* material = declManager ? declManager->FindMaterial(record.materialName.c_str(), false) : nullptr;
    if (!material)
    {
        common->Printf("MatClass: stageDump id=%u material='%s' materialDeclMissing=1\n",
            record.materialId,
            record.materialName.c_str());
        return;
    }

    for (int stageIndex = 0; stageIndex < material->GetNumStages(); ++stageIndex)
    {
        const shaderStage_t* stage = material->GetStage(stageIndex);
        if (!stage)
        {
            continue;
        }

        idImage* image = stage->texture.image;
        const bool active = RtStageConditionCanBeActive(material, stage);
        const bool additive = RtStageIsAdditiveBlend(stage);
        const bool filter = RtStageIsFilterBlend(stage);
        const bool alphaBlend = RtStageBlendUsesSourceAlpha(stage);
        const bool guiOrScreen =
            stage->texture.texgen == TG_SCREEN ||
            stage->texture.texgen == TG_SCREEN2 ||
            SmokeStageIsRenderMap(stage);
        const bool cubeMap =
            stage->texture.texgen == TG_SKYBOX_CUBE ||
            stage->texture.texgen == TG_WOBBLESKY_CUBE ||
            stage->texture.texgen == TG_REFLECT_CUBE ||
            stage->texture.texgen == TG_REFLECT_CUBE2;
        const float conditionValue = RtStageConstantRegisterValue(material, stage->conditionRegister, 1.0f);
        const float alphaTestValue = RtStageConstantRegisterValue(material, stage->alphaTestRegister, -1.0f);
        const textureUsage_t usage = image ? image->GetUsage() : TD_DEFAULT;
        const textureColor_t colorFormat = image ? image->GetOpts().colorFormat : CFM_DEFAULT;
        const RtMaterialCompositingOp compositingOp = RtStageCompositingOperation(stage);

        common->Printf("MatClass: stage id=%u index=%d active=%d conditionReg=%d condition=%.3f lighting=%s routeEvidence=%s image='%s' usage=%s color=%s compositing=%s drawState=0x%llx srcBlendBits=0x%llx dstBlendBits=0x%llx additive=%d filter=%d alphaBlend=%d alphaTest=%d ignoreAlphaTest=%d alphaReg=%d alphaValue=%.3f guiScreen=%d dynamic=%d cinematic=%d cube=%d customProgram=%d texgen=%d\n",
            record.materialId,
            stageIndex,
            active ? 1 : 0,
            stage->conditionRegister,
            conditionValue,
            RtStageLightingName(stage->lighting),
            RtMaterialStageRouteEvidence(stage),
            image ? image->GetName() : "<none>",
            RtTextureUsageName(usage),
            RtTextureColorFormatName(colorFormat),
            RtMaterialCompositingOpName(compositingOp),
            static_cast<unsigned long long>(stage->drawStateBits),
            static_cast<unsigned long long>(stage->drawStateBits & GLS_SRCBLEND_BITS),
            static_cast<unsigned long long>(stage->drawStateBits & GLS_DSTBLEND_BITS),
            additive ? 1 : 0,
            filter ? 1 : 0,
            alphaBlend ? 1 : 0,
            stage->hasAlphaTest ? 1 : 0,
            stage->ignoreAlphaTest ? 1 : 0,
            stage->alphaTestRegister,
            alphaTestValue,
            guiOrScreen ? 1 : 0,
            static_cast<int>(stage->texture.dynamic),
            stage->texture.cinematic ? 1 : 0,
            cubeMap ? 1 : 0,
            stage->newStage ? 1 : 0,
            static_cast<int>(stage->texture.texgen));
    }
}

void MaybeDumpRecord(const RtMaterialRecord& record)
{
    const int debugList = r_pathTracingMatClassDebugList.GetInteger();
    if (debugList <= 0)
    {
        return;
    }
    const int maxLogs = Max(0, r_pathTracingMatClassDebugMax.GetInteger());
    if (maxLogs > 0 && g_recordDebugLogs >= maxLogs)
    {
        return;
    }
    common->Printf("MatClass: record id=%u material='%s'\n",
        record.materialId,
        record.materialName.c_str());
    common->Printf("MatClass: route id=%u route=%s routeReason=%s routeStage=%d surfaceType=%d class=%s classReason=%s classEvidence='%s' confidence=%s normalDecode=%s\n",
        record.materialId,
        RtMaterialBsdfRouteName(record.route),
        RtMaterialBsdfRouteReasonName(record.routeReason),
        record.stageFacts.routeStage,
        record.rawSurfaceType,
        RtMaterialSurfaceClassName(record.surfaceClass),
        RtMaterialSurfaceClassReasonName(record.surfaceClassReason),
        record.surfaceClassEvidence.c_str(),
        RtMaterialClassConfidenceName(record.surfaceClassConfidence),
        RtMaterialNormalDecodeModeName(record.normalDecodeMode));
    common->Printf("MatClass: stages id=%u total=%d bump=%d diffuse=%d specular=%d rmao=%d legacySpec=%d ambient=%d effect=%d additive=%d filter=%d alphaBlend=%d coverage=%d guiScreen=%d dynamic=%d cinematic=%d cube=%d reflect2=%d program=%d\n",
        record.materialId,
        record.stageFacts.stageCount,
        record.stageFacts.bumpStages,
        record.stageFacts.diffuseStages,
        record.stageFacts.specularStages,
        record.stageFacts.pbrRmaoStages,
        record.stageFacts.legacySpecStages,
        record.stageFacts.ambientStages,
        record.stageFacts.effectStages,
        record.stageFacts.additiveBlendStages,
        record.stageFacts.filterBlendStages,
        record.stageFacts.alphaBlendStages,
        record.stageFacts.coverageStages,
        record.stageFacts.guiOrScreenStages,
        record.stageFacts.dynamicImageStages,
        record.stageFacts.cinematicStages,
        record.stageFacts.cubeMapStages,
        record.stageFacts.reflectCube2Stages,
        record.stageFacts.customProgramStages);
    common->Printf("MatClass: liquidFilm id=%u candidate=%d detail=%d blood=%d reflect2=%d coverage=%d coverageImage='%s' wetNormal=%d normalImage='%s' exactOverride=%d overrideReason='%s' legacyPool=%d variant=%d dynamic=%d reason='%s' mode=%d debug=%d page=%d\n",
        record.materialId,
        record.liquidFilmCandidate ? 1 : 0,
        record.liquidFilmIsDetailDecal ? 1 : 0,
        record.liquidFilmHasBloodSemantic ? 1 : 0,
        record.liquidFilmHasWetReflectStage ? 1 : 0,
        record.liquidFilmHasCoverageSource ? 1 : 0,
        record.liquidFilmCoverageImageName.c_str(),
        record.liquidFilmHasWetNormalSource ? 1 : 0,
        record.normalImageName.c_str(),
        record.liquidFilmExactOverride ? 1 : 0,
        record.liquidFilmOverrideReason.c_str(),
        record.liquidFilmLegacyPool ? 1 : 0,
        record.liquidFilmVariant ? 1 : 0,
        record.liquidFilmDynamic ? 1 : 0,
        record.liquidFilmReason.c_str(),
        r_pathTracingLiquidPoolMode.GetInteger(),
        r_pathTracingLiquidPoolDebug.GetInteger(),
        r_pathTracingLiquidPoolDebugPage.GetInteger());
    common->Printf("MatClass: ordered id=%u declaredStages=%d interactionPackets=%d\n",
        record.materialId,
        static_cast<int>(record.compositingStages.size()),
        static_cast<int>(record.interactionPackets.size()));
    const bool needsPerInstanceDynamic =
        record.dynamicFacts.materialUsesRuntimeRegisters ||
        record.dynamicFacts.conditionRegisterStages > 0 ||
        record.dynamicFacts.colorRegisterStages > 0 ||
        record.dynamicFacts.alphaRegisterStages > 0 ||
        record.dynamicFacts.alphaTestRegisterStages > 0 ||
        record.dynamicFacts.textureMatrixRegisterStages > 0 ||
        record.dynamicFacts.dynamicImageStages > 0 ||
        record.dynamicFacts.cinematicStages > 0 ||
        record.dynamicFacts.guiRenderTargetStages > 0 ||
        record.dynamicFacts.customProgramStages > 0;
    common->Printf("MatClass: dynamic id=%u materialRegs=%d condition=%d color=%d alpha=%d alphaTest=%d texMatrix=%d texMatrixRegs=%d dynamicImage=%d cinematic=%d guiRender=%d program=%d decal=%d flipbook=%d frames=%d axis=%d needsInstance=%d\n",
        record.materialId,
        record.dynamicFacts.materialUsesRuntimeRegisters ? 1 : 0,
        record.dynamicFacts.conditionRegisterStages,
        record.dynamicFacts.colorRegisterStages,
        record.dynamicFacts.alphaRegisterStages,
        record.dynamicFacts.alphaTestRegisterStages,
        record.dynamicFacts.textureMatrixStages,
        record.dynamicFacts.textureMatrixRegisterStages,
        record.dynamicFacts.dynamicImageStages,
        record.dynamicFacts.cinematicStages,
        record.dynamicFacts.guiRenderTargetStages,
        record.dynamicFacts.customProgramStages,
        record.dynamicFacts.projectedDecal ? 1 : 0,
        record.dynamicFacts.flipbookAtlasStages,
        record.dynamicFacts.flipbookFrames,
        record.dynamicFacts.flipbookAxis,
        needsPerInstanceDynamic ? 1 : 0);
    common->Printf("MatClass: images id=%u diffuse=%d/%s/%s normal=%d/%s/%s specular=%d/%s/%s emissive=%d/%s/%s alphaTest=%d emissiveIntent=%d\n",
        record.materialId,
        record.hasDiffuseImage ? 1 : 0,
        RtTextureUsageName(record.diffuseUsage),
        RtTextureColorFormatName(record.diffuseColorFormat),
        record.hasNormalImage ? 1 : 0,
        RtTextureUsageName(record.normalUsage),
        RtTextureColorFormatName(record.normalColorFormat),
        record.hasSpecularImage ? 1 : 0,
        RtTextureUsageName(record.specularUsage),
        RtTextureColorFormatName(record.specularColorFormat),
        record.hasEmissiveImage ? 1 : 0,
        RtTextureUsageName(record.emissiveUsage),
        RtTextureColorFormatName(record.emissiveColorFormat),
        record.alphaTested ? 1 : 0,
        record.emissiveIntent ? 1 : 0);
    common->Printf("MatClass: imageNames id=%u diffuse='%s' normal='%s' specular='%s' emissive='%s'\n",
        record.materialId,
        record.diffuseImageName.c_str(),
        record.normalImageName.c_str(),
        record.specularImageName.c_str(),
        record.emissiveImageName.c_str());
    common->Printf("MatClass: imageReasons id=%u diffuse='%s' normal='%s' specular='%s' emissive='%s'\n",
        record.materialId,
        record.diffuseReason.c_str(),
        record.normalReason.c_str(),
        record.specularReason.c_str(),
        record.emissiveReason.c_str());
    common->Printf("MatClass: bsdf id=%u roughness=%.3f metallic=%.3f ior=%.3f transmission=%.3f f0=%.3f ao=%.3f subsurface=%d twoSided=%d bsdfEvidence='%s'\n",
        record.materialId,
        record.bsdf.roughness,
        record.bsdf.metallic,
        record.bsdf.ior,
        record.bsdf.transmission,
        record.bsdf.specularF0,
        record.bsdf.ao,
        static_cast<int>(record.bsdf.subsurfaceHint),
        static_cast<int>(record.bsdf.twoSidedBsdf),
        record.bsdfEvidence.c_str());
    common->Printf("MatClass: packed id=%u flags=0x%08x params=0x%08x dynamic=0x%08x\n",
        record.materialId,
        PackPathTraceMaterialClassifierFlags(record),
        PackPathTraceMaterialClassifierParams(record),
        PackPathTraceMaterialClassifierDynamicFlags(record));
    MaybeDumpRecordStages(record);
    ++g_recordDebugLogs;
}

const char* SurfaceTypeNameForDump(surfTypes_t surfaceType)
{
    switch (surfaceType)
    {
        case SURFTYPE_METAL:
            return "metal";
        case SURFTYPE_STONE:
            return "stone";
        case SURFTYPE_FLESH:
            return "flesh";
        case SURFTYPE_WOOD:
            return "wood";
        case SURFTYPE_CARDBOARD:
            return "cardboard";
        case SURFTYPE_LIQUID:
            return "liquid";
        case SURFTYPE_GLASS:
            return "glass";
        case SURFTYPE_PLASTIC:
            return "plastic";
        case SURFTYPE_RICOCHET:
            return "ricochet";
        case SURFTYPE_NONE:
            return "none";
        default:
            return "other";
    }
}

}

const char* RtMaterialSurfaceClassName(RtMaterialSurfaceClass surfaceClass)
{
    switch (surfaceClass)
    {
        case RtMaterialSurfaceClass::Metal:
            return "Metal";
        case RtMaterialSurfaceClass::Stone:
            return "Stone";
        case RtMaterialSurfaceClass::Flesh:
            return "Flesh";
        case RtMaterialSurfaceClass::Wood:
            return "Wood";
        case RtMaterialSurfaceClass::Cardboard:
            return "Cardboard";
        case RtMaterialSurfaceClass::Liquid:
            return "Liquid";
        case RtMaterialSurfaceClass::Glass:
            return "Glass";
        case RtMaterialSurfaceClass::Plastic:
            return "Plastic";
        case RtMaterialSurfaceClass::Ricochet:
            return "Ricochet";
        case RtMaterialSurfaceClass::Special:
            return "Special";
        default:
            return "Unknown";
    }
}

const char* RtMaterialClassConfidenceName(RtMaterialClassConfidence confidence)
{
    switch (confidence)
    {
        case RtMaterialClassConfidence::Authoritative:
            return "Authoritative";
        case RtMaterialClassConfidence::Flag:
            return "Flag";
        case RtMaterialClassConfidence::Heuristic:
            return "Heuristic";
        default:
            return "FallbackNone";
    }
}

const char* RtMaterialBsdfRouteName(RtMaterialBsdfRoute route)
{
    switch (route)
    {
        case RtMaterialBsdfRoute::RealPbrRmao:
            return "RouteA_RMAO";
        case RtMaterialBsdfRoute::LegacySpecGloss:
            return "RouteB_LegacySpec";
        case RtMaterialBsdfRoute::SurfaceTypeFallback:
            return "RouteC_SurfaceFallback";
        default:
            return "Unknown";
    }
}

const char* RtMaterialSurfaceClassReasonName(RtMaterialSurfaceClassReason reason)
{
    switch (reason)
    {
        case RtMaterialSurfaceClassReason::MaterialSort:
            return "MaterialSort";
        case RtMaterialSurfaceClassReason::SurfaceType:
            return "SurfaceType";
        case RtMaterialSurfaceClassReason::StageKind:
            return "StageKind";
        case RtMaterialSurfaceClassReason::NameToken:
            return "NameToken";
        case RtMaterialSurfaceClassReason::ImageNameToken:
            return "ImageNameToken";
        case RtMaterialSurfaceClassReason::FallbackMetal:
            return "FallbackMetal";
        case RtMaterialSurfaceClassReason::FallbackUnknown:
            return "FallbackUnknown";
        default:
            return "Unknown";
    }
}

const char* RtMaterialBsdfRouteReasonName(RtMaterialBsdfRouteReason reason)
{
    switch (reason)
    {
        case RtMaterialBsdfRouteReason::PbrRmaoStage:
            return "PbrRmaoStage";
        case RtMaterialBsdfRouteReason::LegacySpecularStage:
            return "LegacySpecularStage";
        case RtMaterialBsdfRouteReason::DiscoveredLegacySpecularImage:
            return "DiscoveredLegacySpecularImage";
        case RtMaterialBsdfRouteReason::RmaoDisabled:
            return "RmaoDisabled";
        case RtMaterialBsdfRouteReason::NoCompatibleSpecularStage:
            return "NoCompatibleSpecularStage";
        default:
            return "Unknown";
    }
}

const char* RtMaterialNormalDecodeModeName(RtMaterialNormalDecodeMode mode)
{
    switch (mode)
    {
        case RtMaterialNormalDecodeMode::Rgb8Rg:
            return "RGB8_RG";
        case RtMaterialNormalDecodeMode::CompressedWy:
            return "Compressed_WY";
        default:
            return "None";
    }
}

const char* RtMaterialCompositingOpName(RtMaterialCompositingOp operation)
{
    switch (operation)
    {
        case RtMaterialCompositingOp::OpaqueReplace:
            return "opaque-replace";
        case RtMaterialCompositingOp::Additive:
            return "additive";
        case RtMaterialCompositingOp::MultiplyFilter:
            return "multiply-filter";
        case RtMaterialCompositingOp::InvertedFilterBlackKey:
            return "inverted-filter-black-key";
        case RtMaterialCompositingOp::SourceAlphaOver:
            return "source-alpha-over";
        case RtMaterialCompositingOp::AuthoredAlphaClip:
            return "authored-alpha-clip";
        case RtMaterialCompositingOp::InteractionInput:
            return "interaction-input";
        default:
            return "unknown";
    }
}

void BeginPathTraceMaterialClassifierFrame()
{
    g_materialClassifierStats.frameHits = 0;
    g_materialClassifierStats.frameMisses = 0;
    g_materialClassifierStats.frameRebuilds = 0;
    g_materialClassifierStats.routeRealPbr = 0;
    g_materialClassifierStats.routeLegacySpec = 0;
    g_materialClassifierStats.routeFallback = 0;
    g_materialClassifierStats.confidenceAuthoritative = 0;
    g_materialClassifierStats.confidenceFlag = 0;
    g_materialClassifierStats.confidenceHeuristic = 0;
    g_materialClassifierStats.confidenceFallbackNone = 0;
    g_materialClassifierStats.compositingStages = 0;
    g_materialClassifierStats.maxCompositingStages = 0;
    g_materialClassifierStats.materialsOverFourStages = 0;
    g_materialClassifierStats.materialsOverEightStages = 0;
    g_materialClassifierStats.compositingOpaque = 0;
    g_materialClassifierStats.compositingAdditive = 0;
    g_materialClassifierStats.compositingMultiply = 0;
    g_materialClassifierStats.compositingInverted = 0;
    g_materialClassifierStats.compositingAlphaOver = 0;
    g_materialClassifierStats.compositingAlphaClip = 0;
    g_materialClassifierStats.compositingInteraction = 0;
    g_materialClassifierStats.compositingUnknown = 0;
    g_recordDebugLogs = 0;
}

const RtMaterialRecord& RegisterPathTraceMaterialRecord(const idMaterial* material, const RtSmokeMaterialTextureInfo& info)
{
    const uint64 signature = ComputeRtMaterialRecordSignature(material, info);
    const RtMaterialRecordKey key = BuildRtMaterialRecordKey(info.materialId, info.materialName);
    std::pair<std::unordered_map<RtMaterialRecordKey, RtMaterialRecord, RtMaterialRecordKeyHash>::iterator, bool> insertResult =
        g_materialRecords.emplace(key, RtMaterialRecord());
    RtMaterialRecord& record = insertResult.first->second;
    if (!record.valid)
    {
        ++g_materialClassifierStats.misses;
        ++g_materialClassifierStats.frameMisses;
        record = BuildRtMaterialRecord(material, info, signature);
        ++g_materialClassifierGeneration;
        MaybeDumpRecord(record);
    }
    else if (record.signature != signature)
    {
        ++g_materialClassifierStats.rebuilds;
        ++g_materialClassifierStats.frameRebuilds;
        record = BuildRtMaterialRecord(material, info, signature);
        ++g_materialClassifierGeneration;
        MaybeDumpRecord(record);
    }
    else
    {
        ++g_materialClassifierStats.hits;
        ++g_materialClassifierStats.frameHits;
    }
    TryIndexPathTraceMaterialRecordById(
        g_materialRecordsById,
        info.materialId,
        &record,
        true);
#ifndef NDEBUG
    assert(ValidatePathTraceMaterialRecordLookupIndex());
#endif
    AccumulateRecordStats(record);
    return record;
}

const RtMaterialRecord* FindPathTraceMaterialRecord(uint32_t materialId)
{
    const auto record = g_materialRecordsById.find(materialId);
    return record != g_materialRecordsById.end() ? record->second : nullptr;
}

int GetPathTraceMaterialRecordCount()
{
    return static_cast<int>(g_materialRecords.size());
}

bool PathTraceMaterialRecordLookupIndexSelfTest()
{
    std::unordered_map<RtMaterialRecordKey, RtMaterialRecord, RtMaterialRecordKeyHash>
        records;
    std::unordered_map<uint32_t, const RtMaterialRecord*> index;

    RtMaterialRecordKey firstKey;
    firstKey.materialId = 11u;
    firstKey.materialNameHash = 101u;
    RtMaterialRecord& first = records.emplace(
        firstKey,
        RtMaterialRecord()).first->second;
    first.materialId = firstKey.materialId;
    first.signature = 1u;
    if (!TryIndexPathTraceMaterialRecordById(
            index, first.materialId, &first, false) ||
        index.find(first.materialId) == index.end() ||
        index.find(first.materialId)->second != &first)
    {
        return false;
    }

    first.signature = 2u;
    if (!TryIndexPathTraceMaterialRecordById(
            index, first.materialId, &first, false) ||
        index.find(first.materialId)->second != &first ||
        index.find(first.materialId)->second->signature != 2u)
    {
        return false;
    }

    RtMaterialRecordKey secondKey;
    secondKey.materialId = 22u;
    secondKey.materialNameHash = 202u;
    RtMaterialRecord& second = records.emplace(
        secondKey,
        RtMaterialRecord()).first->second;
    second.materialId = secondKey.materialId;
    if (!TryIndexPathTraceMaterialRecordById(
            index, second.materialId, &second, false) ||
        index.find(99u) != index.end())
    {
        return false;
    }

    RtMaterialRecordKey duplicateKey;
    duplicateKey.materialId = first.materialId;
    duplicateKey.materialNameHash = 303u;
    RtMaterialRecord& duplicate = records.emplace(
        duplicateKey,
        RtMaterialRecord()).first->second;
    duplicate.materialId = first.materialId;
    return !TryIndexPathTraceMaterialRecordById(
            index, duplicate.materialId, &duplicate, false) &&
        index.find(first.materialId) != index.end() &&
        index.find(first.materialId)->second == nullptr;
}

RtMaterialClassifierStats GetPathTraceMaterialClassifierStats()
{
    RtMaterialClassifierStats stats = g_materialClassifierStats;
    stats.records = static_cast<int>(g_materialRecords.size());
    stats.compositingStages = 0;
    stats.maxCompositingStages = 0;
    stats.materialsOverFourStages = 0;
    stats.materialsOverEightStages = 0;
    stats.compositingOpaque = 0;
    stats.compositingAdditive = 0;
    stats.compositingMultiply = 0;
    stats.compositingInverted = 0;
    stats.compositingAlphaOver = 0;
    stats.compositingAlphaClip = 0;
    stats.compositingInteraction = 0;
    stats.compositingUnknown = 0;
    for (const auto& entry : g_materialRecords)
    {
        const RtMaterialRecord& record = entry.second;
        if (!record.valid)
        {
            continue;
        }
        const int compositingStageCount = static_cast<int>(record.compositingStages.size());
        stats.compositingStages += compositingStageCount;
        stats.maxCompositingStages = Max(stats.maxCompositingStages, compositingStageCount);
        stats.materialsOverFourStages += compositingStageCount > 4 ? 1 : 0;
        stats.materialsOverEightStages += compositingStageCount > 8 ? 1 : 0;
        for (const RtMaterialCompositingStageFact& stage : record.compositingStages)
        {
            switch (stage.operation)
            {
                case RtMaterialCompositingOp::OpaqueReplace:
                    ++stats.compositingOpaque;
                    break;
                case RtMaterialCompositingOp::Additive:
                    ++stats.compositingAdditive;
                    break;
                case RtMaterialCompositingOp::MultiplyFilter:
                    ++stats.compositingMultiply;
                    break;
                case RtMaterialCompositingOp::InvertedFilterBlackKey:
                    ++stats.compositingInverted;
                    break;
                case RtMaterialCompositingOp::SourceAlphaOver:
                    ++stats.compositingAlphaOver;
                    break;
                case RtMaterialCompositingOp::AuthoredAlphaClip:
                    ++stats.compositingAlphaClip;
                    break;
                case RtMaterialCompositingOp::InteractionInput:
                    ++stats.compositingInteraction;
                    break;
                default:
                    ++stats.compositingUnknown;
                    break;
            }
        }
    }
    return stats;
}

uint32_t GetPathTraceMaterialClassifierGeneration()
{
    return g_materialClassifierGeneration;
}

uint32_t PackPathTraceMaterialClassifierFlags(const RtMaterialRecord& record)
{
    uint32_t flags = 0;
    flags |= static_cast<uint32_t>(record.surfaceClass) & 0x0fu;
    flags |= (static_cast<uint32_t>(record.surfaceClassConfidence) & 0x03u) << 4;
    flags |= (static_cast<uint32_t>(record.surfaceClassReason) & 0x0fu) << 6;
    flags |= (static_cast<uint32_t>(record.route) & 0x0fu) << 10;
    flags |= (static_cast<uint32_t>(record.routeReason) & 0x0fu) << 14;
    flags |= (static_cast<uint32_t>(record.normalDecodeMode) & 0x03u) << 18;
    flags |= record.hasDiffuseImage ? (1u << 20) : 0u;
    flags |= record.hasNormalImage ? (1u << 21) : 0u;
    flags |= record.hasSpecularImage ? (1u << 22) : 0u;
    flags |= record.hasEmissiveImage ? (1u << 23) : 0u;
    flags |= record.alphaTested ? (1u << 24) : 0u;
    flags |= record.emissiveIntent ? (1u << 25) : 0u;
    flags |= record.bsdf.twoSidedBsdf != 0 ? (1u << 26) : 0u;
    flags |= record.bsdf.subsurfaceHint != 0 ? (1u << 27) : 0u;
    flags |= record.stageFacts.additiveBlendStages > 0 ? (1u << 28) : 0u;
    flags |= record.stageFacts.filterBlendStages > 0 ? (1u << 29) : 0u;
    flags |= record.stageFacts.guiOrScreenStages > 0 ? (1u << 30) : 0u;
    flags |= record.stageFacts.effectStages > 0 ? (1u << 31) : 0u;
    return flags;
}

uint32_t PackPathTraceMaterialClassifierParams(const RtMaterialRecord& record)
{
    return QuantizeRtMaterialUnitFloat(record.bsdf.roughness) |
        (QuantizeRtMaterialUnitFloat(record.bsdf.metallic) << 8) |
        (QuantizeRtMaterialUnitFloat(record.bsdf.transmission) << 16) |
        (QuantizeRtMaterialUnitFloat(record.bsdf.specularF0) << 24);
}

uint32_t PackPathTraceMaterialClassifierDynamicFlags(const RtMaterialRecord& record)
{
    const RtMaterialDynamicFacts& facts = record.dynamicFacts;
    uint32_t flags = 0;
    flags |= facts.materialUsesRuntimeRegisters ? (1u << 2) : 0u;
    flags |= facts.colorRegisterStages > 0 ? (1u << 3) : 0u;
    flags |= (facts.alphaRegisterStages > 0 || facts.alphaTestRegisterStages > 0) ? (1u << 4) : 0u;
    flags |= facts.conditionRegisterStages > 0 ? (1u << 5) : 0u;
    flags |= facts.textureMatrixRegisterStages > 0 ? (1u << 6) : 0u;
    flags |= (facts.dynamicImageStages > 0 || facts.cinematicStages > 0) ? (1u << 7) : 0u;
    flags |= facts.projectedDecal ? (1u << 8) : 0u;
    flags |= facts.guiRenderTargetStages > 0 ? (1u << 9) : 0u;
    flags |= facts.customProgramStages > 0 ? (1u << 10) : 0u;
    flags |= facts.flipbookAtlasStages > 0 ? (1u << 11) : 0u;
    return flags;
}

void MaybeDumpPathTraceMaterialDeclSurfaceTypeDistribution()
{
    if (r_pathTracingMatClassDebugList.GetInteger() < 2 || g_dumpedDeclSurfaceDistribution)
    {
        return;
    }
    g_dumpedDeclSurfaceDistribution = true;

    int counts[SURFTYPE_15 + 2] = {};
    const int materialCount = declManager ? declManager->GetNumDecls(DECL_MATERIAL) : 0;
    for (int materialIndex = 0; materialIndex < materialCount; ++materialIndex)
    {
        const idMaterial* material = static_cast<const idMaterial*>(declManager->DeclByIndex(DECL_MATERIAL, materialIndex, false));
        const int surfaceType = material ? static_cast<int>(material->GetSurfaceType()) : static_cast<int>(SURFTYPE_NONE);
        if (surfaceType >= 0 && surfaceType <= SURFTYPE_15)
        {
            ++counts[surfaceType];
        }
        else
        {
            ++counts[SURFTYPE_15 + 1];
        }
    }

    common->Printf("MatClass: decl surfaceType distribution total=%d\n", materialCount);
    for (int surfaceType = SURFTYPE_NONE; surfaceType <= SURFTYPE_15; ++surfaceType)
    {
        common->Printf("MatClass: decl surfaceType %d (%s) count=%d\n",
            surfaceType,
            SurfaceTypeNameForDump(static_cast<surfTypes_t>(surfaceType)),
            counts[surfaceType]);
    }
    if (counts[SURFTYPE_15 + 1] > 0)
    {
        common->Printf("MatClass: decl surfaceType other count=%d\n", counts[SURFTYPE_15 + 1]);
    }
}
