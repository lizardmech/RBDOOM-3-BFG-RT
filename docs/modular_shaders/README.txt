Modular Shader Architecture Planning Lane
=========================================

Purpose
-------

This folder defines the implementation lane for making path-tracing material
and shader behavior modular enough that glass, liquid-pool decals, and later
special shaders do not spread ad hoc branches through low-level DI/GI code.

The current renderer already has useful pieces:

    primary-surface records with material fields
    material classifier facts
    detail-decal composite staging
    ReSTIR pass-plan scaffolding
    standalone primary-surface and reflection producer routes

The missing boundary is a stable material-feature contract between:

    material classification / primary-surface production
    material evaluation / path-event sampling
    DI/GI/ReSTIR consumer adapters
    standalone feature passes

Workers must not solve this by putting glass or liquid-specific branches
directly into every DI/GI helper. The goal is to make unsupported material
features fail closed in DI/GI, while path-tracer-only or standalone-pass
features can be added deliberately.


Primary Rule
------------

Separate material features from render passes.

Material features answer:

    what kind of surface or modifier is this?
    which lobes or effects does it expose?
    which downstream consumers are allowed to use it?
    which consumers must treat it as unsupported?

Render passes answer:

    which shader runs?
    which resources are bound?
    which material capabilities are consumed?
    which output resource is written?
    which validation view proves the result?

Do not add a new shader by editing unrelated DI/GI math. Add a material
capability, expose it through the primary-surface/material contract, then add
only the pass or adapter that owns the feature.


Reference Roots
---------------

RTX Remix architecture reference:

    E:/prog/references/dxvk-remix-git

NVIDIA nvpro shader math reference:

    E:/prog/references/nvpro_core2-main/nvshaders

These references are not automatic source drops. Check file-level license
headers before reusing any source text. Prefer using them for contracts,
resource flow, pass separation, and BSDF interface shape.


Required Reading
----------------

Read these before starting any task:

    docs/modular_shaders/README.txt
    docs/modular_shaders/worker_protocol.txt
    docs/modular_shaders/material_contract.txt

Glass / reflection quality (view-16 clean DI):

    docs/modular_shaders/basic_glass_psr_reflection_steps.txt
    docs/modular_shaders/dedicated_reflection_secondary_steps.txt
    docs/modular_shaders/stable_reflection_hit_lighting_steps.txt
        **Single production system: deterministic hybrid clear glass. Existing
        clean DI shades the transmitted/primary surface; one mirror hit per
        glass pixel supplies exact emissive, bounded analytic RIS lighting,
        dense RR material guides, and Fresnel sidecar radiance. Duplicate
        sparse, linked-DI/GI, NEE-cache, and legacy fallback systems are removed.**
    docs/modular_shaders/io_whitelist.txt
    docs/modular_shaders/worker_tasks.txt
    docs/modular_shaders/validation_matrix.txt
    docs/modular_shaders/manager_notes.txt
    docs/modular_shaders/glass_shading_notes.txt
    docs/modular_shaders/psr_transmission_refactor_guide.txt

Also read the relevant existing path-tracing architecture docs:

    docs/pathtrace_core_refactor_plan.txt
    docs/pathtrace_core_refactor_tasks/task_02_primary_surface_contract.txt
    docs/pathtrace_core_refactor_tasks/task_05_restir_pass_split.txt
    docs/pathtrace_modules.txt


Standing Restrictions
---------------------

Do not:

    copy NVIDIA source text into rbdoom without file-level license review
    route glass through the opaque diffuse BRDF gate
    make DI/GI understand glass internals unless a task explicitly opens that
        consumer contract
    treat liquid-pool decals as glass/refraction
    reuse alpha-over for liquid pools if overlap should be idempotent
    hide unsupported material kinds by falling back to albedo or old lighting
    add broad shader branches to clean DI/GI sentinel paths without checking
        shader size and SPIR-V risk
    add standalone passes without explicit host dispatch/binding ownership
    change CMake shader layout unless the task explicitly needs a new shader
        entry point
    claim a feature is modular if adding it still requires unrelated DI/GI
        math edits


First Milestone
---------------

Milestone M1 is a no-visual-regression contract refactor:

    existing opaque diffuse behavior still matches current output
    primary surface exposes material kind/capability fields or reserved layout
    DI/GI consumers call material adapters instead of hard-coded shader-type
        branches
    unsupported material kinds fail closed and visibly in debug views
    no glass or liquid behavior is implemented before the opaque adapter
        migration is proven

Milestone M2 is the first new feature:

    liquid-pool decal modifier uses an idempotent max/union coverage law
    DI/GI see only the resolved receiver material
    overlapping 100 percent pool layers do not double tint or double darken

Milestone M3 is initial glass:

    path-tracer-only glass/transmission path event works behind explicit
        capability gates
    DI/GI still fail closed for transmissive surfaces unless a later task
        explicitly opens a transmissive direct/indirect lighting contract
