#include "../idlib/precompiled.h"
#include "../renderer/NVRHI/PathTraceTextureRegistry.h"
#include "../renderer/NVRHI/PathTraceMaterialBindingKernel.h"

#include <cstdio>

idCommon* idLib::common = nullptr;
bool AssertFailed(const char*, int, const char*) { return false; }

void* Mem_Alloc16(std::size_t size, memTag_t)
{
    return _aligned_malloc(size, 16);
}

void Mem_Free16(void* pointer)
{
    _aligned_free(pointer);
}

void* Mem_ClearedAlloc(std::size_t size, memTag_t tag)
{
    void* pointer = Mem_Alloc16(size, tag);
    if (pointer != nullptr)
    {
        std::memset(pointer, 0, size);
    }
    return pointer;
}

bool TestMaterialBindingFrame()
{
    const char* names[]={"","textures/stone","_white","GUIS/monitor","Gui\\panel","video/a","videos/a",
        "cinematics/a","generated/a","abcCINEMATICxyz","abcSCRATCHxyz","abcRENDERxyz","movie.swf",
        "textures/a\\b","guis","gui","video","cinematics","generated","textures/long_plain_name"};
    for (bool allow:{false,true}) for (const char* name:names) {
        const bool expected=IsSmokeImageNameSafeForRayTracing(name) || (allow && IsSmokeImageNameGuiLike(name));
        if (RtCpuMaterialBindingNameSafe(name,allow)!=expected) return false;
    }
    ClearSmokeMaterialTextureRegistry();
    RtSmokeMaterialTextureInfo base;base.materialId=240;base.diffuseImageName="textures/stone";
    base.hasTextureHandle=true;base.hasSafeTexture=true;
    PublishCompleteSmokeMaterialTextureInfo(std::move(base));
    RtCpuMaterialBindingPlanInput input;
    if (SnapshotSmokeMaterialBindingFrame(input,0)) return false;
    const std::vector<uint32_t> unrelated {999};
    if (!SnapshotSmokeMaterialBindingFrame(input,32ull*1024*1024,&unrelated) || !input.rows.empty()) return false;
    const std::vector<uint32_t> active {240};
    auto frame=SnapshotSmokeMaterialBindingFrame(input,32ull*1024*1024,&active);
    RtCpuMaterialBindingPlan plan;
    if (!frame || !BuildRtCpuMaterialBindingPlan(input,plan)) return false;
    auto premature=plan;
    if (CompleteSmokeMaterialBindingFrame(*frame,std::move(premature))) return false;
    if (!ResolveSmokeMaterialBindingResources(*frame)) return false;
    auto malformed=plan;malformed.safety.push_back(0);
    if (CompleteSmokeMaterialBindingFrame(*frame,std::move(malformed))) return false;
    malformed=plan;malformed.rules[0].resource=UINT32_MAX;
    if (CompleteSmokeMaterialBindingFrame(*frame,std::move(malformed))) return false;
    malformed=plan;malformed.rows.begin()->second[0]=UINT32_MAX;
    if (CompleteSmokeMaterialBindingFrame(*frame,std::move(malformed))) return false;
    if (!CompleteSmokeMaterialBindingFrame(*frame,std::move(plan))) return false;
    const auto generation=SmokeMaterialTextureRegistryGeneration();
    auto refreshed=RefreshSmokeMaterialTextureHandlesForActiveIds({240},{240},frame.get());
    if (refreshed.visited!=1 || refreshed.changedMaterialIds!=std::vector<uint32_t>({240}) ||
        FindSmokeMaterialTextureInfo(240)->hasTextureHandle || FindSmokeMaterialTextureInfo(240)->hasSafeTexture ||
        SmokeMaterialTextureRegistryGeneration()!=generation+1) return false;
    if (!RegisterSmokeMaterialTextureVariant(241,240,frame.get()) ||
        !RegisterSmokeMaterialTextureVariant(241,240,frame.get())) return false;
    refreshed=RefreshSmokeMaterialTextureHandlesForActiveIds({241},{},frame.get());
    if (refreshed.visited!=1 || !refreshed.changedMaterialIds.empty()) return false;
    // Mutated provenance must not consume the old prepared row. In this harness
    // the legacy resource-refresh oracle is a no-op, making a stale apply visible.
    auto* changed=FindSmokeMaterialTextureInfo(241);changed->diffuseImageName="other/name";changed->hasSafeTexture=true;
    refreshed=RefreshSmokeMaterialTextureHandlesForActiveIds({241},{},frame.get());
    if (!changed->hasSafeTexture || !refreshed.changedMaterialIds.empty()) return false;
    ClearSmokeMaterialTextureRegistry();
    return true;
}

int main()
{
    const bool passed = SmokeMaterialTextureRegistryAtomicPublicationSelfTest() && TestMaterialBindingFrame() &&
        SmokeMaterialBindingDefinitionSelfTest();
    std::printf("PathTrace texture registry atomic publication: %s\n",
        passed ? "PASS" : "FAIL");
    return passed ? 0 : 1;
}
