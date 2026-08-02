#pragma once

#include <cstdint>

// Hot P0 -> D0 contract owned by the unified PT lane.  History, motion,
// material identity, and future reconnection data belong in optional
// sidecars; they must not inflate the initial-shading load.
static constexpr std::uint32_t PATH_TRACE_UNIFIED_PT_PRIMARY_RECEIVER_STRIDE = 48u;
