# Clean RTXDI DI material-feature RT shader manifest.
#
# Entries are "source-stem|build label". The root shader CMake file owns the
# generic Vulkan compile helper; feature shader additions should live here with
# their .rt.hlsl/.hlsli pair in this directory.

set(PATH_TRACING_CLEAN_RTXDI_DI_MATERIAL_FEATURE_RT_SHADERS
    "pathtrace_clean_rtxdi_di_transmission_producer|clean-room RTXDI DI transmission producer"
    "pathtrace_clean_rtxdi_di_glass|clean-room RTXDI DI glass material-feature")

