#pragma once

// CPU skinning bridge for captured Doom surfaces.
//
// This is the temporary CPU path for skinned/deformed geometry capture. The
// functions are narrow so a future GPU skinning implementation can replace the
// data source without disturbing scene capture classification rules.

class idDrawVert;
class idJointMat;
class idVec3;
struct drawSurf_t;
struct srfTriangles_t;

struct SmokeRtSkinningJointSnapshot
{
    const idJointMat* jointsInverted = nullptr;
    int numInvertedJoints = 0;
};

bool GetSmokeRtCpuSkinningJointSnapshot(const srfTriangles_t* tri, SmokeRtSkinningJointSnapshot& out);
const idJointMat* GetSmokeRtCpuSkinningJoints(const srfTriangles_t* tri);
idVec3 TransformSmokeSkinnedVertexPosition(const idDrawVert& base, const idJointMat* joints);
idVec3 TransformSmokeSkinnedVertexNormal(const idDrawVert& base, const idJointMat* joints);
idVec3 TransformSmokeSkinnedVertexTangent(const idDrawVert& base, const idJointMat* joints);
idVec3 TransformSmokeSkinnedVertexBitangent(const idDrawVert& base, const idJointMat* joints);
bool SmokeSkinnedSurfaceLikelyBasePose(const drawSurf_t* drawSurf, const srfTriangles_t* tri);
