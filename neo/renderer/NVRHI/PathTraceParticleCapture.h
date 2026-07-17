#pragma once

// Screen-space particle-card capture diagnostics.
//
// PC-T00 is deliberately read-only: it inventories live effect-card stages and
// their current BVH route without changing scene capture or rendering.

struct viewDef_t;

void AuditPathTraceParticleCompositeCandidates(const viewDef_t* viewDef);
