// Language-neutral UPT random-slot ledger.  Define UPT_RANDOM_SLOT before
// including this file.  A slot is the pair (stream namespace, dimension), so
// dimensions may repeat only across different streams.

UPT_RANDOM_SLOT(InitialFamilySelection,       0x0100u, 0u)
UPT_RANDOM_SLOT(InitialReservoirSelection,    0x0101u, 0u)
UPT_RANDOM_SLOT(InitialLocalLightIdentity,    0x0102u, 0u)
UPT_RANDOM_SLOT(InitialLocalLightSurfaceU,    0x0102u, 1u)
UPT_RANDOM_SLOT(InitialLocalLightSurfaceV,    0x0102u, 2u)

UPT_RANDOM_SLOT(MaterialLobeSelection,        0x0200u, 0u)
UPT_RANDOM_SLOT(MaterialDirectionU,           0x0200u, 1u)
UPT_RANDOM_SLOT(MaterialDirectionV,           0x0200u, 2u)

UPT_RANDOM_SLOT(AnalyticLightSelection,       0x0300u, 0u)
UPT_RANDOM_SLOT(AnalyticLightSurfaceU,        0x0300u, 1u)
UPT_RANDOM_SLOT(AnalyticLightSurfaceV,        0x0300u, 2u)
UPT_RANDOM_SLOT(EmissiveLightSelection,       0x0301u, 0u)
UPT_RANDOM_SLOT(EmissiveSurfaceU,             0x0301u, 1u)
UPT_RANDOM_SLOT(EmissiveSurfaceV,             0x0301u, 2u)

UPT_RANDOM_SLOT(EnvironmentDirectionU,        0x0400u, 0u)
UPT_RANDOM_SLOT(EnvironmentDirectionV,        0x0400u, 1u)
UPT_RANDOM_SLOT(IndirectContinuationU,        0x0500u, 0u)
UPT_RANDOM_SLOT(IndirectContinuationV,        0x0500u, 1u)
UPT_RANDOM_SLOT(IndirectRouletteSurvival,     0x0501u, 0u)
UPT_RANDOM_SLOT(ReplayPathEvent,               0x0600u, 0u)

UPT_RANDOM_SLOT(TemporalTapSelection,         0x0700u, 0u)
UPT_RANDOM_SLOT(TemporalReservoirSelection,   0x0700u, 1u)
UPT_RANDOM_SLOT(SpatialNeighborCoordinateU,   0x0800u, 0u)
UPT_RANDOM_SLOT(SpatialNeighborCoordinateV,   0x0800u, 1u)
UPT_RANDOM_SLOT(SpatialReservoirSelection,    0x0800u, 2u)
