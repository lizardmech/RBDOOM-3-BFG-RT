foreach(required UPT05_SOURCE_DIR UPT05_STAMP)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "UPT-05 material-boundary verification missing ${required}")
    endif()
endforeach()

set(core_modules
    upt04_initial_direct.slang
    upt04_initial_indirect.slang
    upt04_initial_shared.slang
    upt04_initial_direct_only_shared.slang
    upt04_local_light_evaluation.slang)
foreach(module IN LISTS core_modules)
    file(READ "${UPT05_SOURCE_DIR}/${module}" source)
    if(source MATCHES "import[ \t]+(upt04_material_primary_live|upt04_material_set_live_probe|upt05_material_openpbr|upt05_material_set_openpbr)")
        message(FATAL_ERROR
            "UPT-05 core module ${module} imports a concrete material implementation")
    endif()
    if(NOT source MATCHES "IUpt04MaterialSet")
        message(FATAL_ERROR
            "UPT-05 core module ${module} is not generic over IUpt04MaterialSet")
    endif()
endforeach()

set(entry_modules
    upt04_initial_rayquery.slang
    upt04_initial_raygen.slang
    upt04_initial_diagnostics.slang
    upt04_initial_direct_only_rayquery.slang
    upt04_initial_direct_only_raygen.slang
    upt04_initial_indirect_only_rayquery.slang
    upt04_initial_indirect_only_raygen.slang)
foreach(module IN LISTS entry_modules)
    file(READ "${UPT05_SOURCE_DIR}/${module}" source)
    if(NOT source MATCHES "import[ \t]+upt05_material_set_openpbr")
        message(FATAL_ERROR
            "UPT-05 entry module ${module} does not select the OpenPBR composition")
    endif()
    if(NOT source MATCHES "Upt05OpenPbrMaterialSet")
        message(FATAL_ERROR
            "UPT-05 entry module ${module} lacks static material specialization")
    endif()
endforeach()

file(READ "${UPT05_SOURCE_DIR}/upt04_material_provider_contract.slang" contract)
if(NOT contract MATCHES "interface[ \t]+IUpt04MaterialSet")
    message(FATAL_ERROR "UPT-05 material provider interface is missing")
endif()

file(READ "${UPT05_SOURCE_DIR}/upt05_material_set_openpbr.slang" composition)
if(NOT composition MATCHES
        "struct[ \t]+Upt05OpenPbrMaterialSet[ \t]*:[ \t]*IUpt04MaterialSet")
    message(FATAL_ERROR "UPT-05 OpenPBR composition does not conform to the material interface")
endif()

file(READ "${UPT05_SOURCE_DIR}/upt05_material_openpbr.slang" material_math)
foreach(required_symbol
        Upt05OpenPbrEvaluateEonDiffuse
        Upt05OpenPbrEvaluateGgx
        Upt05OpenPbrGgxReflectionPdf
        Upt05OpenPbrSampleGgxVndf
        Upt05SampleOpenPbr)
    if(NOT material_math MATCHES "${required_symbol}")
        message(FATAL_ERROR "UPT-05 OpenPBR module lacks ${required_symbol}")
    endif()
endforeach()
if(material_math MATCHES
        "StructuredBuffer|RWStructuredBuffer|Texture[123]D|RWTexture|SamplerState|RayQuery|TraceRay")
    message(FATAL_ERROR
        "UPT-05 OpenPBR math module owns a GPU resource or ray operation")
endif()

file(WRITE "${UPT05_STAMP}"
    "UPT-05 material boundary verified: generic ReSTIR core, static OpenPBR composition, EON+GGX VNDF eval/sample/pdf, material math resources=0 rays=0\n")
