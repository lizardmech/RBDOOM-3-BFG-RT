#include "precompiled.h"
#pragma hdrstop

#include "PathTraceSkinning.h"
#include "../RenderCommon.h"
#include "../Model_local.h"

namespace {

bool SmokeRtCpuPointerLooksPlausible(const void* pointer)
{
    const uintptr_t address = reinterpret_cast<uintptr_t>(pointer);
    if (address < 0x10000u || (address & (alignof(void*) - 1u)) != 0u)
    {
        return false;
    }
    // Captured draw surfaces can outlive frontend-owned dynamic-model memory.
    // Reject poison/sentinel values before following staticModelWithJoints.
    // Current supported 64-bit targets use the lower canonical user range.
    if (sizeof(uintptr_t) == 8 && address > static_cast<uintptr_t>(0x00007FFFFFFFFFFFull))
    {
        return false;
    }
    return true;
}

#if defined(_MSC_VER) && defined(_WIN32)
__declspec(noinline)
#endif
bool SmokeRtCpuTryReadModelSkinning(
    const idRenderModelStatic* model,
    int& jointCount,
    const idJointMat*& joints)
{
#if defined(_MSC_VER) && defined(_WIN32)
    __try
    {
        jointCount = model->numInvertedJoints;
        joints = model->jointsInverted;
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        jointCount = 0;
        joints = nullptr;
        return false;
    }
#else
    jointCount = model->numInvertedJoints;
    joints = model->jointsInverted;
    return true;
#endif
}

#if defined(_MSC_VER) && defined(_WIN32)
__declspec(noinline)
#endif
bool SmokeRtCpuTryCopyJoint(const idJointMat* source, idJointMat& destination)
{
#if defined(_MSC_VER) && defined(_WIN32)
    __try
    {
        memcpy(&destination, source, sizeof(destination));
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
#else
    memcpy(&destination, source, sizeof(destination));
    return true;
#endif
}

bool SmokeRtCpuTryLoadVertexJoints(const idDrawVert& base, const idJointMat* joints, idJointMat loaded[4])
{
    return joints &&
        SmokeRtCpuTryCopyJoint(joints + base.color[0], loaded[0]) &&
        SmokeRtCpuTryCopyJoint(joints + base.color[1], loaded[1]) &&
        SmokeRtCpuTryCopyJoint(joints + base.color[2], loaded[2]) &&
        SmokeRtCpuTryCopyJoint(joints + base.color[3], loaded[3]);
}

}

bool GetSmokeRtCpuSkinningJointSnapshot(const srfTriangles_t* tri, SmokeRtSkinningJointSnapshot& out)
{
    out = SmokeRtSkinningJointSnapshot();
    if (!r_useGPUSkinning.GetBool() || !tri)
    {
        return false;
    }

    const idRenderModelStatic* model = tri->staticModelWithJoints;
    if (!SmokeRtCpuPointerLooksPlausible(model))
    {
        return false;
    }

    int jointCount = 0;
    const idJointMat* joints = nullptr;
    if (!SmokeRtCpuTryReadModelSkinning(model, jointCount, joints) ||
        jointCount <= 0 ||
        jointCount > 4096 ||
        !SmokeRtCpuPointerLooksPlausible(joints))
    {
        return false;
    }
    out.jointsInverted = joints;
    out.numInvertedJoints = jointCount;
    return true;
}

const idJointMat* GetSmokeRtCpuSkinningJoints(const srfTriangles_t* tri)
{
    SmokeRtSkinningJointSnapshot snapshot;
    return GetSmokeRtCpuSkinningJointSnapshot(tri, snapshot) ? snapshot.jointsInverted : nullptr;
}

idVec3 TransformSmokeSkinnedVertexPosition(const idDrawVert& base, const idJointMat* joints)
{
    idJointMat loaded[4];
    if (!SmokeRtCpuTryLoadVertexJoints(base, joints, loaded))
    {
        return base.xyz;
    }

    const float w0 = base.color2[0] * (1.0f / 255.0f);
    const float w1 = base.color2[1] * (1.0f / 255.0f);
    const float w2 = base.color2[2] * (1.0f / 255.0f);
    const float w3 = base.color2[3] * (1.0f / 255.0f);

    idJointMat accum;
    idJointMat::Mul(accum, loaded[0], w0);
    idJointMat::Mad(accum, loaded[1], w1);
    idJointMat::Mad(accum, loaded[2], w2);
    idJointMat::Mad(accum, loaded[3], w3);

    return accum * idVec4(base.xyz.x, base.xyz.y, base.xyz.z, 1.0f);
}

idVec3 TransformSmokeSkinnedVertexNormal(const idDrawVert& base, const idJointMat* joints)
{
    idJointMat loaded[4];
    if (!SmokeRtCpuTryLoadVertexJoints(base, joints, loaded))
    {
        return base.GetNormal();
    }

    const float w0 = base.color2[0] * (1.0f / 255.0f);
    const float w1 = base.color2[1] * (1.0f / 255.0f);
    const float w2 = base.color2[2] * (1.0f / 255.0f);
    const float w3 = base.color2[3] * (1.0f / 255.0f);

    idJointMat accum;
    idJointMat::Mul(accum, loaded[0], w0);
    idJointMat::Mad(accum, loaded[1], w1);
    idJointMat::Mad(accum, loaded[2], w2);
    idJointMat::Mad(accum, loaded[3], w3);

    idVec3 normal = accum * base.GetNormal();
    normal.Normalize();
    return normal;
}

idVec3 TransformSmokeSkinnedVertexTangent(const idDrawVert& base, const idJointMat* joints)
{
    idJointMat loaded[4];
    if (!SmokeRtCpuTryLoadVertexJoints(base, joints, loaded))
    {
        return base.GetTangent();
    }

    const float w0 = base.color2[0] * (1.0f / 255.0f);
    const float w1 = base.color2[1] * (1.0f / 255.0f);
    const float w2 = base.color2[2] * (1.0f / 255.0f);
    const float w3 = base.color2[3] * (1.0f / 255.0f);

    idJointMat accum;
    idJointMat::Mul(accum, loaded[0], w0);
    idJointMat::Mad(accum, loaded[1], w1);
    idJointMat::Mad(accum, loaded[2], w2);
    idJointMat::Mad(accum, loaded[3], w3);

    idVec3 tangent = accum * base.GetTangent();
    tangent.Normalize();
    return tangent;
}

idVec3 TransformSmokeSkinnedVertexBitangent(const idDrawVert& base, const idJointMat* joints)
{
    idJointMat loaded[4];
    if (!SmokeRtCpuTryLoadVertexJoints(base, joints, loaded))
    {
        return base.GetBiTangent();
    }

    const float w0 = base.color2[0] * (1.0f / 255.0f);
    const float w1 = base.color2[1] * (1.0f / 255.0f);
    const float w2 = base.color2[2] * (1.0f / 255.0f);
    const float w3 = base.color2[3] * (1.0f / 255.0f);

    idJointMat accum;
    idJointMat::Mul(accum, loaded[0], w0);
    idJointMat::Mad(accum, loaded[1], w1);
    idJointMat::Mad(accum, loaded[2], w2);
    idJointMat::Mad(accum, loaded[3], w3);

    idVec3 bitangent = accum * base.GetBiTangent();
    bitangent.Normalize();
    return bitangent;
}

bool SmokeSkinnedSurfaceLikelyBasePose(const drawSurf_t* drawSurf, const srfTriangles_t* tri)
{
    const viewEntity_t* space = drawSurf ? drawSurf->space : nullptr;
    const idRenderEntityLocal* entityDef = space ? space->entityDef : nullptr;
    const renderEntity_t* renderEntity = entityDef ? &entityDef->parms : nullptr;
    return (drawSurf && drawSurf->jointCache != 0) ||
        (tri && tri->staticModelWithJoints != nullptr) ||
        (renderEntity && renderEntity->joints != nullptr && renderEntity->numJoints > 0);
}
