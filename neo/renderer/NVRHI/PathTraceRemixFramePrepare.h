#pragma once

// CPU-only frame preparation scaffold for the RTX Remix-shaped ReSTIR rebuild.
//
// This mirrors the frame-ordering responsibility of Remix SceneManager without
// owning resources, RAB bindings, RTXDI dispatch, or reservoir reset policy.

#include <cstdint>

struct PathTraceRemixFramePrepareDesc
{
    uint64_t frameIndex = 0;
    uint32_t resetReasonFlags = 0;
};

struct PathTraceRemixFramePrepareObservationPackage
{
    uint64_t frameIndex = 0;
    uint32_t resetReasonFlags = 0;
};

class PathTraceRemixFramePrepare
{
public:
    void Clear();
    void BeginFrame(const PathTraceRemixFramePrepareDesc& desc);

    const PathTraceRemixFramePrepareObservationPackage& GetObservationPackage() const;

private:
    PathTraceRemixFramePrepareObservationPackage m_observationPackage;
};
