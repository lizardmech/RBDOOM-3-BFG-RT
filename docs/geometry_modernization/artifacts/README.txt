Geometry modernization - durable artifact index
================================================

PURPOSE
-------
This directory contains bounded, machine-diffable evidence produced by the GEO
tickets. Do not place complete console logs, screenshots, shader binaries, or
executables here.

GEO-00 FILES
------------
  baseline_2026-07-23.csv
    Accepted numeric baseline extracted from geometry_plan.log. `NA` means the
    original capture did not contain that field; it is not equivalent to zero.

  diagnostic_source_map.csv
    Maps every baseline field group to its current log prefix, source owner, and
    console trigger.

  capture_checklist.txt
    Exact bounded console sequence and scene requirements for refreshing the
    four baseline scenarios.

  GEO-00_handoff.txt
    Completion record, limitations, build status, and rollback.

GEO-01 FILES
------------
  GEO-01_instrumentation_handoff.txt
    Current-code contract audit, the discarded active-prefix hypothesis,
    one-shot capture command, output interpretation, build/deploy identity,
    and the runtime work still required to localize the earliest mismatch.

  GEO-01_runtime_capture_2026-07-23.txt
    Paired comm1 runtime tuple values, the discarded mistaken block, both
    shader samples, the localized opaque brush receiver, and the final retained
    cache versus static-BLAS traversal A/B.

  MONSTER_FIRE_composite_ab_2026-07-23.txt
    Frame-window A/B showing the stalls require the screen-space composite but
    not CPU sort or particle lighting, the aligned trace, and the descriptor-
    cache A/B which rules out per-frame binding creation as a required cause.

NAMING
------
Use:

  GEO-XX_<scenario>_<YYYY-MM-DD>.<ext>

Future CSV files must preserve existing column meanings. Add a new column rather
than silently changing the interpretation of an old one. Use decimal integers,
plain decimal milliseconds, `NA` for absent data, and `0` only for a measured
zero.

LARGE RAW LOGS
--------------
Keep raw logs outside git and record their absolute path, byte size, timestamp,
executable identity, and the labels used to derive the bounded artifacts.

GEO-00 source:

  C:/Users/lizard/Saved Games/id Software/RBDOOM 3 BFG/base/geometry_plan.log
  bytes: 2778132
  modified: 2026-07-23 01:06:53 Australia/Sydney
  executable: RBDOOM 3 BFG 1.6.0.1403 win-x64 Jul 22 2026 00:29:49
