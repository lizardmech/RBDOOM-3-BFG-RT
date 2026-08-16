foreach(required
        UPT56_PRODUCTION_REFLECTION UPT56_PRODUCTION_DISASSEMBLY
        UPT56_CLASSIFY_REFLECTION UPT56_CLASSIFY_DISASSEMBLY
        UPT56_CONSUME_REFLECTION UPT56_CONSUME_DISASSEMBLY
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

if(NOT classify_reflection MATCHES "\"set\"[ \t]*:[ \t]*0,[ \t\r\n]*\"binding\"[ \t]*:[ \t]*33" OR
   NOT classify_reflection MATCHES "\"set\"[ \t]*:[ \t]*0,[ \t\r\n]*\"binding\"[ \t]*:[ \t]*34" OR
   NOT classify_reflection MATCHES "\"set\"[ \t]*:[ \t]*0,[ \t\r\n]*\"binding\"[ \t]*:[ \t]*35")
    message(FATAL_ERROR "UPT-56 classify reflection lacks queue bindings 33-35")
endif()
if(NOT consume_reflection MATCHES "\"set\"[ \t]*:[ \t]*0,[ \t\r\n]*\"binding\"[ \t]*:[ \t]*33" OR
   NOT consume_reflection MATCHES "\"set\"[ \t]*:[ \t]*0,[ \t\r\n]*\"binding\"[ \t]*:[ \t]*34")
    message(FATAL_ERROR "UPT-56 consume reflection lacks queue bindings 33-34")
endif()

file(WRITE "${UPT56_STAMP}"
    "UPT-56 classify ops=${classify_op_count} samples=${classify_sample_count}; production ops=${production_op_count} samples=${production_sample_count}; consume workgroup=64x1\n")
