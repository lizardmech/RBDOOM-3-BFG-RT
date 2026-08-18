foreach(required
        UPT56_PRODUCTION_REFLECTION UPT56_PRODUCTION_DISASSEMBLY
        UPT56_CLASSIFY_REFLECTION UPT56_CLASSIFY_DISASSEMBLY
        UPT56_CONSUME_REFLECTION UPT56_CONSUME_DISASSEMBLY
        UPT56_W_CLASSIFY_REFLECTION UPT56_W_CLASSIFY_DISASSEMBLY
        UPT56_STAMP)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "UPT-56 emissive-closure verification missing ${required}")
    endif()
endforeach()

file(READ "${UPT56_PRODUCTION_REFLECTION}" production_reflection)
file(READ "${UPT56_PRODUCTION_DISASSEMBLY}" production_disassembly)
file(READ "${UPT56_CLASSIFY_REFLECTION}" classify_reflection)
file(READ "${UPT56_CLASSIFY_DISASSEMBLY}" classify_disassembly)
file(READ "${UPT56_CONSUME_REFLECTION}" consume_reflection)
file(READ "${UPT56_CONSUME_DISASSEMBLY}" consume_disassembly)

if(NOT classify_reflection MATCHES "\"workgroup_size\"[ \t\r\n]*:[ \t\r\n]*\\[[ \t\r\n]*8[ \t]*,[ \t\r\n]*8")
    message(FATAL_ERROR "UPT-56 classify workgroup is not 8x8")
endif()
if(NOT consume_reflection MATCHES "\"workgroup_size\"[ \t\r\n]*:[ \t\r\n]*\\[[ \t\r\n]*64[ \t]*,[ \t\r\n]*1")
    message(FATAL_ERROR "UPT-56 consume workgroup is not 64x1")
endif()

string(REGEX MATCHALL "Op[A-Za-z0-9]+" production_ops "${production_disassembly}")
string(REGEX MATCHALL "Op[A-Za-z0-9]+" classify_ops "${classify_disassembly}")
list(LENGTH production_ops production_op_count)
list(LENGTH classify_ops classify_op_count)
if(NOT classify_op_count LESS production_op_count)
    message(FATAL_ERROR
        "UPT-56 classify SPIR-V op count ${classify_op_count} is not smaller than production ${production_op_count}")
endif()

string(REGEX MATCHALL "OpImageSample" production_samples "${production_disassembly}")
string(REGEX MATCHALL "OpImageSample" classify_samples "${classify_disassembly}")
list(LENGTH production_samples production_sample_count)
list(LENGTH classify_samples classify_sample_count)
if(NOT classify_sample_count LESS production_sample_count)
    message(FATAL_ERROR
        "UPT-56 classify image-sample count ${classify_sample_count} is not smaller than production ${production_sample_count}")
endif()

file(READ "${UPT56_W_CLASSIFY_REFLECTION}" w_classify_reflection)
file(READ "${UPT56_W_CLASSIFY_DISASSEMBLY}" w_classify_disassembly)

if(NOT w_classify_reflection MATCHES "\"workgroup_size\"[ \t\r\n]*:[ \t\r\n]*\\[[ \t\r\n]*8[ \t]*,[ \t\r\n]*8")
    message(FATAL_ERROR "UPT-56 W classify workgroup is not 8x8")
endif()

string(REGEX MATCHALL "Op[A-Za-z0-9]+" w_classify_ops "${w_classify_disassembly}")
list(LENGTH w_classify_ops w_classify_op_count)
if(w_classify_op_count GREATER 18700)
    message(FATAL_ERROR
        "UPT-56 W classify SPIR-V op count ${w_classify_op_count} exceeds 18700")
endif()

string(REGEX MATCHALL "OpImageSample" w_classify_samples "${w_classify_disassembly}")
list(LENGTH w_classify_samples w_classify_sample_count)
if(w_classify_sample_count GREATER 7)
    message(FATAL_ERROR
        "UPT-56 W classify image-sample count ${w_classify_sample_count} exceeds 7")
endif()

string(REGEX MATCHALL "OpRayQueryInitializeKHR" w_classify_rq_init
    "${w_classify_disassembly}")
list(LENGTH w_classify_rq_init w_classify_rq_init_count)
string(REGEX MATCHALL "OpRayQueryProceedKHR" w_classify_rq_proceed
    "${w_classify_disassembly}")
list(LENGTH w_classify_rq_proceed w_classify_rq_proceed_count)
if(NOT w_classify_rq_init_count EQUAL 2 OR NOT w_classify_rq_proceed_count EQUAL 2)
    message(FATAL_ERROR
        "UPT-56 W classify ray-query sites rqInit=${w_classify_rq_init_count} rqProceed=${w_classify_rq_proceed_count}, expected 2/2")
endif()

if(w_classify_disassembly MATCHES "%gUpt04EmissiveReplay")
    message(FATAL_ERROR "UPT-56 W classify still references %gUpt04EmissiveReplay")
endif()
if(w_classify_disassembly MATCHES "%gUpt04EmissiveGeometry")
    message(FATAL_ERROR "UPT-56 W classify still references %gUpt04EmissiveGeometry")
endif()
if(NOT w_classify_disassembly MATCHES "%gUpt56Emissive")
    message(FATAL_ERROR "UPT-56 W classify is missing %gUpt56Emissive* buffer references")
endif()


if(DEFINED UPT56_WG_CLASSIFY_DISASSEMBLY AND EXISTS "${UPT56_WG_CLASSIFY_DISASSEMBLY}")
    file(READ "${UPT56_WG_CLASSIFY_DISASSEMBLY}" wg_classify_disassembly)
    if(wg_classify_disassembly MATCHES "%gUpt04Skinned")
        message(FATAL_ERROR
            "UPT-56 W+G classify still references %gUpt04Skinned*")
    endif()
    string(REGEX MATCHALL "Op[A-Za-z0-9]+" wg_ops "${wg_classify_disassembly}")
    list(LENGTH wg_ops wg_op_count)
    string(REGEX MATCHALL "OpImageSample" wg_samples "${wg_classify_disassembly}")
    list(LENGTH wg_samples wg_sample_count)
    string(REGEX MATCHALL "OpRayQueryInitializeKHR" wg_rq_init "${wg_classify_disassembly}")
    list(LENGTH wg_rq_init wg_rq_init_count)
    string(REGEX MATCHALL "OpRayQueryProceedKHR" wg_rq_proceed "${wg_classify_disassembly}")
    list(LENGTH wg_rq_proceed wg_rq_proceed_count)
else()
    set(wg_op_count 0)
    set(wg_sample_count 0)
    set(wg_rq_init_count 0)
    set(wg_rq_proceed_count 0)
endif()
if(DEFINED UPT56_XG_PRODUCE_DISASSEMBLY AND EXISTS "${UPT56_XG_PRODUCE_DISASSEMBLY}")
    file(READ "${UPT56_XG_PRODUCE_DISASSEMBLY}" xg_produce_disassembly)
    if(xg_produce_disassembly MATCHES "%gUpt04Skinned")
        message(FATAL_ERROR
            "UPT-56 X+G produce still references %gUpt04Skinned*")
    endif()
    string(REGEX MATCHALL "Op[A-Za-z0-9]+" xg_ops "${xg_produce_disassembly}")
    list(LENGTH xg_ops xg_op_count)
    string(REGEX MATCHALL "OpImageSample" xg_samples "${xg_produce_disassembly}")
    list(LENGTH xg_samples xg_sample_count)
    string(REGEX MATCHALL "OpRayQueryInitializeKHR" xg_rq_init "${xg_produce_disassembly}")
    list(LENGTH xg_rq_init xg_rq_init_count)
    string(REGEX MATCHALL "OpRayQueryProceedKHR" xg_rq_proceed "${xg_produce_disassembly}")
    list(LENGTH xg_rq_proceed xg_rq_proceed_count)
else()
    set(xg_op_count 0)
    set(xg_sample_count 0)
    set(xg_rq_init_count 0)
    set(xg_rq_proceed_count 0)
endif()

file(WRITE "${UPT56_STAMP}"
    "UPT-56 classify ops=${classify_op_count} samples=${classify_sample_count}; production ops=${production_op_count} samples=${production_sample_count}; consume workgroup=64x1 queue=disabled; W classify ops=${w_classify_op_count} samples=${w_classify_sample_count} rqInit=${w_classify_rq_init_count} rqProceed=${w_classify_rq_proceed_count}; W+G classify ops=${wg_op_count} samples=${wg_sample_count} rqInit=${wg_rq_init_count} rqProceed=${wg_rq_proceed_count}; X+G produce ops=${xg_op_count} samples=${xg_sample_count} rqInit=${xg_rq_init_count} rqProceed=${xg_rq_proceed_count}\n")