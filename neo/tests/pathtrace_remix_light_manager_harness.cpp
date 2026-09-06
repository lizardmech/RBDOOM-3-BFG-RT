#include "precompiled.h"
#include "PathTraceRemixLightManager.h"
#include "PathTraceDoomLights.h"
#include "PathTraceEmissiveCandidates.h"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

bool AssertFailed(const char*, int, const char*) { std::abort(); }
void* Mem_Alloc16(std::size_t size, memTag_t) {
    void* p=_aligned_malloc(size,16); if (!p && size) throw std::bad_alloc(); return p;
}
void Mem_Free16(void* p) { _aligned_free(p); }
void* Mem_ClearedAlloc(std::size_t size, memTag_t tag) {
    void* p=Mem_Alloc16(size,tag); if (p) std::memset(p,0,size); return p;
}

// Golden output was recorded before R4-033 production edits. Hash each named stats
// field rather than compiler padding; GPU record layouts have explicit padding.
struct Digest {
    uint64_t value = 1469598103934665603ull;
    template<class T> void Add(const T& v) {
        const auto* bytes = reinterpret_cast<const unsigned char*>(&v);
        for (size_t i=0;i<sizeof(T);++i) { value^=bytes[i]; value*=1099511628211ull; }
    }
    template<class T> void Vector(const std::vector<T>& v) {
        Add(uint64_t(v.size())); for (const auto& item:v) Add(item);
    }
};
static uint64_t OutputDigest(const PathTraceRemixLightManagerPrepareResult& r) {
    Digest d;
    d.Vector(r.currentLightPayloads); d.Vector(r.previousLightPayloads);
    d.Vector(r.currentToPreviousMap); d.Vector(r.previousToCurrentMap);
    for (const auto& range:r.lightRanges) d.Add(range);
    d.Add(r.stats.frameIndex);
    d.Add(r.stats.enabled);
    d.Add(r.stats.domain);
    d.Add(r.stats.strictRemixMapping);
    d.Add(r.stats.resetReasonFlags);
    d.Add(r.stats.currentLightCount);
    d.Add(r.stats.previousLightCount);
    d.Add(r.stats.currentToPreviousCount);
    d.Add(r.stats.previousToCurrentCount);
    d.Add(r.stats.currentMappedCount);
    d.Add(r.stats.currentInvalidCount);
    d.Add(r.stats.currentOnlyCount);
    d.Add(r.stats.previousMappedCount);
    d.Add(r.stats.previousInvalidCount);
    d.Add(r.stats.previousOnlyCount);
    d.Add(r.stats.invalidDuplicateIdentityCount);
    d.Add(r.stats.emissiveRangeOffset);
    d.Add(r.stats.emissiveRangeCount);
    d.Add(r.stats.doomAnalyticRangeOffset);
    d.Add(r.stats.doomAnalyticRangeCount);
    d.Add(r.stats.emissiveSampleCount);
    d.Add(r.stats.doomAnalyticSampleCount);
    d.Add(r.stats.totalSampleCount);
    d.Add(r.stats.nonEmptyRangeCount);
    d.Add(r.stats.payloadOnlyChange);
    d.Add(r.stats.mappedPayloadChangedCount);
    d.Add(r.stats.currentMappedByType);
    d.Add(r.stats.currentOnlyByType);
    d.Add(r.stats.previousMappedByType);
    d.Add(r.stats.previousOnlyByType);
    d.Add(r.stats.mappedPayloadChangedByType);
    d.Add(r.stats.duplicateIdentityByType);
    d.Add(r.stats.doomAnalyticCurrentSampleableCount);
    d.Add(r.stats.doomAnalyticStableCacheableCount);
    d.Add(r.stats.doomAnalyticUnstableDynamicCount);
    d.Add(r.stats.doomAnalyticRejectNoRemapCount);
    d.Add(r.stats.doomAnalyticRejectPayloadChangedCount);
    d.Add(r.stats.doomAnalyticRejectUnprovenContinuityCount);
    d.Add(r.stats.doomAnalyticRejectUnknownIdentityCount);
    d.Add(r.stats.doomAnalyticRejectDuplicateIdentityCount);
    d.Add(r.stats.doomAnalyticRejectPortalDisconnectedCount);
    d.Add(r.stats.doomAnalyticRejectOutOfSelectedAreaCount);
    d.Add(r.stats.structuralSignatureChanged);
    d.Add(r.stats.mappingSignatureChanged);
    d.Add(r.stats.payloadSignatureChanged);
    d.Add(r.stats.oldSmokeReservoirSignatureConsulted);
    d.Add(r.stats.resourceAllocationCount);
    d.Add(r.stats.shaderRouteCount);
    d.Add(r.stats.firstFailingContract);
    d.Add(r.stats.structuralSignature);
    d.Add(r.stats.mappingSignature);
    d.Add(r.stats.payloadSignature);
    d.Add(r.stats.firstPayloadChangedCurrent);
    d.Add(r.stats.firstPayloadChangedPrevious);
    d.Add(r.stats.firstCurrentOnly);
    d.Add(r.stats.firstPreviousOnly);
    d.Add(r.lastStructuralSignature); d.Add(r.lastMappingSignature); d.Add(r.lastPayloadSignature);
    d.Add(r.haveLastSignatures); d.Add(r.lightUniverseHistoryValid); d.Add(r.lastPrepareWasLightUniverse);
    return d.value;
}
// Reproduce the resident-scene collision using the production table builder.
// The independent lookup below follows the unchanged Slang hash/probe contract.
static unsigned CheckResidentEmissiveLookup() {
    struct IdentityRun { uint32_t instance, first, count; };
    static const IdentityRun runs[] = {
#include "fixtures/upt_emissive_lookup_mars_city1.inc"
    };
    std::vector<PathTraceUnifiedLightRecord> lights;
    for (const auto& run : runs) {
        for (uint32_t primitive = run.first; primitive < run.first + run.count; ++primitive) {
            PathTraceUnifiedLightRecord light = {};
            light.type = PATH_TRACE_UNIFIED_LIGHT_TYPE_EMISSIVE_TRIANGLE;
            light.sourceIndex = static_cast<uint32_t>(lights.size());
            light.instanceId = run.instance;
            light.primitiveIndex = primitive;
            lights.push_back(light);
        }
    }
    if (lights.size() != 4096) return 1;
    std::vector<PathTraceEmissiveDistributionEntry> distribution(lights.size());
    for (uint32_t i = 0; i < distribution.size(); ++i) {
        distribution[i].emissiveTriangleIndex = i;
        distribution[i].denseLightIndex = i;
        distribution[i].cumulativePdf = float(i + 1u) / float(lights.size());
    }
    const auto lookup = BuildPathTraceUnifiedEmissiveLookup(lights, distribution, 0, 4096);
    if (!lookup.exact) {
        std::cerr << "Resident emissive fixture cannot form an exact lookup within the shader probe limit\n";
        return 1;
    }
    if (lookup.entries.size() < 2 || (lookup.entries.size() & (lookup.entries.size() - 1)) != 0 ||
        lookup.entries.size() > lights.size() * 16u) return 1;
    const uint32_t mask = static_cast<uint32_t>(lookup.entries.size() - 1);
    uint32_t maxProbes = 0;
    for (uint32_t i = 0; i < lights.size(); ++i) {
        const auto& light = lights[i];
        uint32_t hash = light.instanceId ^ (light.primitiveIndex + 0x9e3779b9u +
            (light.instanceId << 6u) + (light.instanceId >> 2u));
        hash ^= hash >> 16u; hash *= 0x7feb352du;
        hash ^= hash >> 15u; hash *= 0x846ca68bu; hash ^= hash >> 16u;
        bool found = false;
        for (uint32_t probe = 0; probe < 16u; ++probe) {
            const auto& entry = lookup.entries[(hash + probe) & mask];
            if (!entry.occupied) break;
            if (entry.instanceId == light.instanceId && entry.primitiveIndex == light.primitiveIndex) {
                if (entry.denseLightIndex != i || entry.conditionalIdentityPdf != 1.0f / 4096.0f) return 1;
                maxProbes = std::max(maxProbes, probe + 1u);
                found = true;
                break;
            }
        }
        if (!found) return 1;
    }
    auto duplicate = lights;
    duplicate.back().instanceId = lights.front().instanceId;
    duplicate.back().primitiveIndex = lights.front().primitiveIndex;
    if (BuildPathTraceUnifiedEmissiveLookup(duplicate, distribution, 0, 4096).exact) return 1;
    auto badCdf = distribution;
    badCdf[2].cumulativePdf = 0.0f;
    if (BuildPathTraceUnifiedEmissiveLookup(lights, badCdf, 0, 4096).exact) return 1;
    std::cout << "4096 resident emissive identities resolved in " << lookup.entries.size()
        << " entries, max probes " << maxProbes << "; duplicate and malformed-CDF rejection preserved\n";
    return 0;
}
int main(int argc,char** argv) {
    const bool record=argc==2 && std::string(argv[1])=="--record";
    std::ifstream golden(std::filesystem::path(__FILE__).parent_path()/"pathtrace_remix_light_manager_golden.txt");
    if (!record && !golden) { std::cerr<<"Missing sealed golden fixture\n"; return 1; }
    PathTraceRemixLightManager manager;
    std::vector<PathTraceSmokeEmissiveTriangle> previousE;
    std::vector<PathTraceDoomAnalyticLightCandidate> previousA;
    std::vector<PathTraceDoomAnalyticLightCandidateIdentity> previousIds;
    unsigned failures=0, duplicateFrames=0, stableFrames=0;
    for (uint32_t f=0;f<48;++f) {
        if (f==10 || f==30) manager.Clear();
        PathTraceRemixFramePrepareObservationPackage frame { f+1, f==10 ? 1u : 0u };
        if (f==31) manager.PrepareDisabled(frame,2,true);
        const uint32_t domain=(f/4)%3;
        std::vector<PathTraceSmokeEmissiveTriangle> e(domain==0 || f%12==5 ? 0 : 64);
        std::vector<PathTraceDoomAnalyticLightCandidate> a(domain==1 || f%12==6 ? 0 : 12);
        if (f==47) { e.clear(); a.clear(); }
        std::vector<PathTraceDoomAnalyticLightCandidateIdentity> ids(a.size());
        std::vector<PathTraceEmissiveLightRemap> er(std::max(e.size(),previousE.size()));
        std::vector<PathTraceDoomAnalyticLightRemap> ar(std::max(a.size(),previousA.size()));
        for (uint32_t i=0;i<e.size();++i) {
            auto& v=e[i];
            v.centerAndArea[0]=float(i); v.centerAndArea[3]=2;
            v.normalAndLuminance[2]=1; v.estimatedRadianceAndLuminance[0]=3;
            v.estimatedRadianceAndLuminance[3]=1; v.sampleWeightAndPdf[0]=2;
            v.sampleWeightAndPdf[1]=.125f; v.materialIndex=10+i%4;
            v.instanceId=1+i/8; v.primitiveIndex=i; v.identityHashLo=100+i;
            if (f%4==2 && i%5==0) v.centerAndArea[1]=float(f);
        }
        if (e.size()>3 && f%8==3) { e[1]=e[0]; e[2]=e[0]; }
        if (!e.empty() && f%8==4) { e[0].identityHashLo=0; e[0].materialIndex=0; }
        if (!e.empty() && f%8==5) std::reverse(e.begin(),e.end());
        for (uint32_t i=0;i<a.size();++i) {
            auto& v=a[i]; v.originAndRadius[0]=float(i); v.originAndRadius[3]=2;
            v.doomRadiusAndArea[0]=8; v.colorAndIntensity[0]=1;
            v.colorAndIntensity[1]=.5f; v.renderLightIndex=200+i; v.entityNumber=300+i;
            ids[i].universeIndex=i; ids[i].remapIndex=i; ids[i].flags=7;
            if (f%4==2 && i%3==0) v.originAndRadius[1]=float(f);
            if (f%8==5 && i%2==0) v.colorAndIntensity[0]=v.colorAndIntensity[1]=0;
        }
        if (a.size()>3 && f%8==3) { a[1]=a[0]; a[2]=a[0]; }
        if (!a.empty() && f%8==4) { a[0].renderLightIndex=UINT32_MAX; ids[0].flags=0; }
        if (!a.empty() && f%8==7) { std::reverse(a.begin(),a.end()); std::reverse(ids.begin(),ids.end()); }
        for (uint32_t i=0;i<er.size();++i) {
            er[i].currentToPreviousIndex=i<previousE.size()?int32_t(i):-1;
            er[i].previousToCurrentIndex=i<e.size()?int32_t(i):-1; er[i].flags=RT_SMOKE_EMISSIVE_REMAP_VALID;
        }
        for (uint32_t i=0;i<ar.size();++i) {
            ar[i].currentToPreviousCandidateIndex=i<previousA.size()?int32_t(i):-1;
            ar[i].previousToCurrentCandidateIndex=i<a.size()?int32_t(i):-1; ar[i].flags=PATH_TRACE_DOOM_ANALYTIC_IDENTITY_REMAP_VALID;
        }
        PathTraceRemixLightManagerPrepareDesc desc;
        desc.framePackage=&frame; desc.currentEmissiveTriangles=&e; desc.previousEmissiveTriangles=&previousE;
        desc.emissiveRemap=&er; desc.currentAnalyticLights=&a; desc.previousAnalyticLights=&previousA;
        desc.currentAnalyticIdentities=&ids; desc.previousAnalyticIdentities=&previousIds; desc.analyticRemap=&ar;
        desc.emissiveSampleCount=8; desc.doomAnalyticSampleCount=16; desc.domain=domain;
        desc.strictRemixMapping=f%3!=0; desc.lightUniverseEnabled=f<16 || f>=32;
        auto result=manager.BuildPrepareResult(desc);
        duplicateFrames+=result.stats.invalidDuplicateIdentityCount!=0;
        stableFrames+=result.stats.doomAnalyticStableCacheableCount!=0;
        const auto actual=OutputDigest(result);
        if (record) std::cout<<std::hex<<actual<<"\n";
        else { uint64_t expected=0; if (!(golden>>std::hex>>expected) || expected!=actual) {
            std::cerr<<"Output differs from sealed baseline on frame "<<std::dec<<f<<"\n"; ++failures;
        } }
        manager.ApplyPrepareResult(std::move(result));
        previousE=std::move(e); previousA=std::move(a); previousIds=std::move(ids);
    }
    if (!record) {
        std::string excess;
        if (golden>>excess || !duplicateFrames || !stableFrames) ++failures;
        std::cout<<"48 complete-output comparisons; duplicate cases "<<duplicateFrames
            <<", stable analytic cases "<<stableFrames<<", failures "<<failures<<"\n";
    }
    if (!record) failures += CheckResidentEmissiveLookup();
    return failures?1:0;
}
