#include "precompiled.h"
#pragma hdrstop

#include "PathTraceRemixFramePrepare.h"

void PathTraceRemixFramePrepare::Clear()
{
    m_observationPackage = PathTraceRemixFramePrepareObservationPackage();
}

void PathTraceRemixFramePrepare::BeginFrame(const PathTraceRemixFramePrepareDesc& desc)
{
    m_observationPackage = PathTraceRemixFramePrepareObservationPackage();
    m_observationPackage.frameIndex = desc.frameIndex;
    m_observationPackage.resetReasonFlags = desc.resetReasonFlags;
}

const PathTraceRemixFramePrepareObservationPackage& PathTraceRemixFramePrepare::GetObservationPackage() const
{
    return m_observationPackage;
}
