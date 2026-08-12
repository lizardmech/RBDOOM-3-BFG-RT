foreach(required UPT10_REFLECTION UPT10_DISASSEMBLY UPT10_STAMP
        UPT31_REFLECTION UPT31_DISASSEMBLY
        UPT32_REFLECTION UPT32_DISASSEMBLY
        UPT33_REFLECTION UPT33_DISASSEMBLY
        UPT34_REFLECTION UPT34_DISASSEMBLY
        UPT10_HOST_SOURCE UPT10_SHADER_SOURCE)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "UPT-30 shared spatial verification missing ${required}")
    endif()
endforeach()

file(READ "${UPT10_REFLECTION}" reflection)
file(READ "${UPT10_DISASSEMBLY}" disassembly)
file(READ "${UPT31_REFLECTION}" stored_reflection)
file(READ "${UPT31_DISASSEMBLY}" stored_disassembly)
file(READ "${UPT32_REFLECTION}" workgroup_reflection)
file(READ "${UPT32_DISASSEMBLY}" workgroup_disassembly)
file(READ "${UPT33_REFLECTION}" rescue_reflection)
file(READ "${UPT33_DISASSEMBLY}" rescue_disassembly)
file(READ "${UPT34_REFLECTION}" multi_reflection)
file(READ "${UPT34_DISASSEMBLY}" multi_disassembly)
file(READ "${UPT10_HOST_SOURCE}" host_source)
file(READ "${UPT10_SHADER_SOURCE}" shader_source)

string(REGEX MATCHALL "\"binding\"[ \t]*:" bindings "${reflection}")
list(LENGTH bindings binding_count)
if(NOT binding_count EQUAL 27)
    message(FATAL_ERROR
        "UPT-30 selected-pair spatial must expose 27 bindings including the endpoint sidecar")
endif()
string(REGEX MATCHALL "\"binding\"[ \t]*:" stored_bindings
    "${stored_reflection}")
list(LENGTH stored_bindings stored_binding_count)
if(NOT stored_binding_count EQUAL 27)
    message(FATAL_ERROR
        "UPT-31 stored-target spatial must preserve all 27 shared bindings")
endif()
string(REGEX MATCHALL "\"binding\"[ \t]*:" workgroup_bindings
    "${workgroup_reflection}")
list(LENGTH workgroup_bindings workgroup_binding_count)
if(NOT workgroup_binding_count EQUAL 27)
    message(FATAL_ERROR
        "UPT-32 workgroup-pair spatial must preserve all 27 shared bindings")
endif()
string(REGEX MATCHALL "\"binding\"[ \t]*:" rescue_bindings
    "${rescue_reflection}")
list(LENGTH rescue_bindings rescue_binding_count)
if(NOT rescue_binding_count EQUAL 27)
    message(FATAL_ERROR
        "UPT-33 rescue spatial must preserve all 27 shared bindings")
endif()
string(REGEX MATCHALL "\"binding\"[ \t]*:" multi_bindings
    "${multi_reflection}")
list(LENGTH multi_bindings multi_binding_count)
if(NOT multi_binding_count EQUAL 27)
    message(FATAL_ERROR
        "UPT-34 multi-neighbor spatial must preserve all 27 shared bindings")
endif()
if(NOT reflection MATCHES
        "\"name\"[ \t]*:[ \t]*\"gUpt04StaticTriangleClasses\"[^}]*\"set\"[ \t]*:[ \t]*0[^}]*\"binding\"[ \t]*:[ \t]*9" OR
   NOT reflection MATCHES
        "\"name\"[ \t]*:[ \t]*\"gUpt04DynamicTriangleClasses\"[^}]*\"set\"[ \t]*:[ \t]*0[^}]*\"binding\"[ \t]*:[ \t]*13")
    message(FATAL_ERROR
        "UPT-30 selected-pair spatial lost exact triangle-class shader bindings 9/13")
endif()
if(NOT host_source MATCHES
        "StructuredBuffer_SRV.9, geometry.staticTriangleClassBuffer." OR
   NOT host_source MATCHES
        "StructuredBuffer_SRV.13, geometry.dynamicTriangleClassBuffer." OR
   NOT host_source MATCHES "Upt04AddReplayGeometryLayoutItems.layoutDesc.")
    message(FATAL_ERROR
        "UPT-30 selected-pair spatial host replay descriptor contract lacks triangle-class parity")
endif()
if(NOT reflection MATCHES
        "\"set\"[ \t]*:[ \t]*0,[ \t\r\n]*\"binding\"[ \t]*:[ \t]*22")
    message(FATAL_ERROR
        "UPT-30 selected-pair spatial lacks the exact emissive lookup binding")
endif()
if(NOT reflection MATCHES
        "\"name\"[ \t]*:[ \t]*\"emissiveDistributionCountAndValid\",[ \t\r\n]*\"type\"[ \t]*:[ \t]*\"uint\",[ \t\r\n]*\"offset\"[ \t]*:[ \t]*88" OR
   NOT reflection MATCHES
        "\"name\"[ \t]*:[ \t]*\"emissiveLookupCapacityAndValid\",[ \t\r\n]*\"type\"[ \t]*:[ \t]*\"uint\",[ \t\r\n]*\"offset\"[ \t]*:[ \t]*92")
    message(FATAL_ERROR
        "UPT-30 selected-pair spatial lost its 96-byte replay control ABI")
endif()

string(REGEX MATCHALL "OpRayQueryInitializeKHR" ray_queries "${disassembly}")
list(LENGTH ray_queries ray_query_count)
if(NOT ray_query_count EQUAL 6 OR disassembly MATCHES "OpTraceRay")
    message(FATAL_ERROR
        "UPT-30 paired spatial must preserve six static RayQuery sites and no TraceRay site")
endif()
string(REGEX MATCHALL "OpRayQueryInitializeKHR" stored_ray_queries
    "${stored_disassembly}")
list(LENGTH stored_ray_queries stored_ray_query_count)
if(NOT stored_ray_query_count EQUAL 6 OR stored_disassembly MATCHES "OpTraceRay")
    message(FATAL_ERROR
        "UPT-31 stored-target spatial must preserve six static RayQuery sites and no TraceRay site")
endif()
string(REGEX MATCHALL "OpRayQueryInitializeKHR" workgroup_ray_queries
    "${workgroup_disassembly}")
list(LENGTH workgroup_ray_queries workgroup_ray_query_count)
if(workgroup_ray_query_count LESS 3 OR workgroup_ray_query_count GREATER 6
        OR workgroup_disassembly MATCHES "OpTraceRay")
    message(FATAL_ERROR
        "UPT-32 workgroup-pair spatial must retain 3..6 static RayQuery sites and no TraceRay site")
endif()
if(NOT workgroup_disassembly MATCHES "OpControlBarrier" OR
   NOT workgroup_disassembly MATCHES "Workgroup")
    message(FATAL_ERROR
        "UPT-32 workgroup-pair spatial lost its group-shared prepass/resample barrier")
endif()
string(REGEX MATCHALL "OpRayQueryInitializeKHR" rescue_ray_queries
    "${rescue_disassembly}")
list(LENGTH rescue_ray_queries rescue_ray_query_count)
if(rescue_ray_query_count LESS 3 OR rescue_ray_query_count GREATER 6
        OR rescue_disassembly MATCHES "OpTraceRay"
        OR NOT rescue_disassembly MATCHES "OpControlBarrier")
    message(FATAL_ERROR
        "UPT-33 rescue spatial must retain 3..6 static RayQuery sites, one barrier, and no TraceRay site")
endif()
string(REGEX MATCHALL "OpRayQueryInitializeKHR" multi_ray_queries
    "${multi_disassembly}")
list(LENGTH multi_ray_queries multi_ray_query_count)
string(REGEX MATCHALL "OpControlBarrier" multi_barriers
    "${multi_disassembly}")
list(LENGTH multi_barriers multi_barrier_count)
if(multi_ray_query_count LESS 3 OR multi_ray_query_count GREATER 6
        OR multi_disassembly MATCHES "OpTraceRay"
        OR NOT multi_barrier_count EQUAL 1)
    message(FATAL_ERROR
        "UPT-34 multi-neighbor spatial must retain 3..6 static RayQuery sites, exactly one barrier, and no TraceRay site")
endif()

# The bounded variant must actually remove source-side direct emitter work,
# rather than merely select a differently named but equivalent artifact.
string(REGEX MATCHALL "OpImageSampleExplicitLod" baseline_texture_samples
    "${disassembly}")
string(REGEX MATCHALL "OpImageSampleExplicitLod" stored_texture_samples
    "${stored_disassembly}")
list(LENGTH baseline_texture_samples baseline_texture_sample_count)
list(LENGTH stored_texture_samples stored_texture_sample_count)
if(NOT stored_texture_sample_count LESS baseline_texture_sample_count)
    message(FATAL_ERROR
        "UPT-31 stored-target artifact did not reduce static explicit texture-sample sites (${baseline_texture_sample_count} -> ${stored_texture_sample_count})")
endif()

# Tie the exhaustive 1-D involution proof below to the four-phase shader
# schedule. Horizontal and vertical phases use the same coordinate mapping.
foreach(required_source_pattern
        "frameSampleIndex & 3u"
        "const bool vertical = phase >= 2u"
        "const bool shiftedOrigin = (phase & 1u) != 0u"
        "const uint leaderParity = shiftedOrigin ? 1u : 0u"
        "if ((coordinate & 1u) != leaderParity)"
        "if (coordinate + 1u >= extent)"
        "const uint2 partnerPixel = pixel")
    string(FIND "${shader_source}" "${required_source_pattern}" source_offset)
    if(source_offset EQUAL -1)
        message(FATAL_ERROR
            "UPT-30 paired spatial schedule drifted from the involution oracle: ${required_source_pattern}")
    endif()
endforeach()

foreach(required_upt34_source_pattern
        "public static const uint kUpt34NeighborCount = 3u"
        "return 1u + ((base + ordinal * 20u) % 63u)"
        "float(kUpt34NeighborCount) * neighborTerm"
        "float(kUpt34NeighborCount) * neighborCrossTerm"
        "(canonicalMisSum + float(missingPairCount))"
        "Upt10MapProposal(canonicalProposal)"
        "ordinal == 0u ? 1u : 0u"
        "acceptedSampleIds[acceptedOrdinal] == neighborSampleId"
        "Upt34ExecuteWorkgroupMultiNeighbor")
    string(FIND "${shader_source}" "${required_upt34_source_pattern}"
        upt34_source_offset)
    if(upt34_source_offset EQUAL -1)
        message(FATAL_ERROR
            "UPT-34 simultaneous multi-neighbor contract missing source pattern: ${required_upt34_source_pattern}")
    endif()
endforeach()
if(NOT host_source MATCHES
        "upt09_spatial_shared_workgroup_multi3_rescue_stored_target_light64.bin" OR
   NOT host_source MATCHES
        "multiNeighbor=.u neighbors=.u")
    message(FATAL_ERROR
        "UPT-34 host specialization or runtime diagnostic is missing")
endif()

foreach(required_upt33_source_pattern
        "public bool Upt09SharedEmptyCenterRescueEligible"
        "targetZero.status = kUpt10ShiftValidZero"
        "output.selected.replayKey |= kUpt03ReplayKeyRescueBit"
        "output.selected.flags &= ~(kUpt04CandidateFlagReplayable"
        "Upt09PublishSharedEmptyCenterRescue"
        "& kUpt03ReplayKeyRescueBit) != 0u")
    string(FIND "${shader_source}" "${required_upt33_source_pattern}"
        upt33_source_offset)
    if(upt33_source_offset EQUAL -1)
        message(FATAL_ERROR
            "UPT-33 rescue/non-donation contract missing source pattern: ${required_upt33_source_pattern}")
    endif()
endforeach()

foreach(required_upt32_source_pattern
        "groupshared Upt03PackedReservoir gUpt32ShiftedCandidates[64u]"
        "return 1u + (seed % 63u)"
        "return (dispatchPixel + offset) % dimensions"
        "const uint partnerLaneIndex = laneIndex ^ pairMask"
        "const uint pairScreenLocal = pairScreenDelta.x <= 7u"
        "GroupMemoryBarrierWithGroupSync()"
        "gUpt32ShiftValid[partnerLaneIndex]"
        "Upt09PublishSharedPairDirection")
    string(FIND "${shader_source}" "${required_upt32_source_pattern}"
        upt32_source_offset)
    if(upt32_source_offset EQUAL -1)
        message(FATAL_ERROR
            "UPT-32 workgroup-pair contract missing source pattern: ${required_upt32_source_pattern}")
    endif()
endforeach()
foreach(extent RANGE 1 17)
    foreach(phase RANGE 0 3)
        math(EXPR shifted "${phase} & 1")
        set(leader_parity ${shifted})
        math(EXPR last "${extent} - 1")
        foreach(coordinate RANGE 0 ${last})
            if(shifted EQUAL 1 AND coordinate EQUAL 0)
                continue()
            endif()
            math(EXPR parity "${coordinate} & 1")
            if(parity EQUAL leader_parity)
                math(EXPR partner "${coordinate} + 1")
                if(partner GREATER_EQUAL extent)
                    continue()
                endif()
            else()
                math(EXPR partner "${coordinate} - 1")
            endif()
            math(EXPR partner_parity "${partner} & 1")
            if(partner_parity EQUAL leader_parity)
                math(EXPR reverse "${partner} + 1")
            else()
                math(EXPR reverse "${partner} - 1")
            endif()
            if(NOT reverse EQUAL coordinate)
                message(FATAL_ERROR
                    "UPT-30 pairing is not an involution: extent=${extent} phase=${phase} coordinate=${coordinate} partner=${partner} reverse=${reverse}")
            endif()
        endforeach()
    endforeach()
endforeach()

# Prove all three masks are nonzero, distinct and self-inverting for every
# possible hashed base. This is the exact arithmetic used by Upt34PairMask.
foreach(base RANGE 0 62)
    set(previous_masks "")
    foreach(ordinal RANGE 0 2)
        math(EXPR mask "1 + ((${base} + ${ordinal} * 20) % 63)")
        if(mask LESS 1 OR mask GREATER 63)
            message(FATAL_ERROR
                "UPT-34 produced invalid mask: base=${base} ordinal=${ordinal} mask=${mask}")
        endif()
        list(FIND previous_masks "${mask}" duplicate_mask_index)
        if(NOT duplicate_mask_index EQUAL -1)
            message(FATAL_ERROR
                "UPT-34 masks are not distinct: base=${base} ordinal=${ordinal} mask=${mask}")
        endif()
        list(APPEND previous_masks "${mask}")
        foreach(lane RANGE 0 63)
            math(EXPR partner "${lane} ^ ${mask}")
            math(EXPR reverse "${partner} ^ ${mask}")
            if(partner EQUAL lane OR NOT reverse EQUAL lane)
                message(FATAL_ERROR
                    "UPT-34 pairing is not fixed-point-free/involutive: base=${base} ordinal=${ordinal} lane=${lane} partner=${partner} reverse=${reverse}")
            endif()
        endforeach()
    endforeach()
endforeach()

file(WRITE "${UPT10_STAMP}"
    "UPT-30/31/32/33/34 paired spatial verified: bindings=27, triangleClasses=9/13 host+shader, leaderPairing=involution(extents1..17,phases0..3), workgroupPairing=three-distinct-xor-involutions(64 lanes), pushConstants=96, static RayQuery sites=6/workgroup${workgroup_ray_query_count}/rescue${rescue_ray_query_count}/multi${multi_ray_query_count}, multiBarriers=${multi_barrier_count}, TraceRay=0, explicitTextureSampleSites=${baseline_texture_sample_count}->${stored_texture_sample_count}\n")
