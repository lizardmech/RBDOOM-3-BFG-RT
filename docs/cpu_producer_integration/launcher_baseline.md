# Match the user's launchUTP baseline

2026-09-07 follow-up: the user reports the candidate functions but is slower and visually different from E:/prog/rbdoom-3-BFG-prebuilt/launchUTP.bat. Main promotion remains pending.

The original candidate launcher was not an equivalent comparison: it executed upt.cfg, which the user's batch never executes. Live startup probes of the original executable show UPT family=1 (direct-only), splitInitial=0, compactLights=0, compactGeometry=0, temporalIndirect=0, replayCompaction=0 and sharedSpatial=0. The added preset selected family=0 (direct+indirect) and enabled those paths. FakePBRSpecular was 1 in both, so its omission from the candidate's command line was not an effective mismatch.

The original executable reports a Jul 31 2026 build date; its filesystem timestamp is Aug 21. The integration executable reports Sep 7. Matching launch flags/settings does not establish binary or shader equivalence.

## Correction

- launch-rewrite.cmd and launch-legacy.cmd now copy the renderer arguments from the user's exact batch and no longer execute upt.cfg.
- Both use the existing matched I2 executable/shaders and isolated save path. The rewrite launcher requests producer=1, legacy requests 0. GPU skinning=1, parallel AddModels=1 and retireFrames=6 were confirmed equal to the original executable's live values.
- The candidate's saved settings were backed up and refreshed from a snapshot of the original Saved Games D3BFGConfig.cfg. The original settings, batch and binaries were not edited.
- No renderer source, shader or executable was changed for this correction.

## Verification and limits

Each executable was started with the original batch flags and identical saved settings, using its own probe save directory; listCvars r_ and condump recorded live values, then the process quit without loading a map. Among all shared renderer CVars, there are zero value differences. Candidate-only producer controls are recorded separately; the active rewrite switch is 1. Original settings still match the pre-probe hash.

The earlier native smoke runs used upt.cfg. They validate that configuration's worker/commit and route/reload operation, not equivalence to the user's launchUTP baseline. The 36/36 code tests remain applicable; no rebuild was necessary for launcher/settings edits. Visual/performance acceptance is still pending a settled comparison with these corrected launchers. If a gap remains, use launch-legacy.cmd with the same candidate executable to distinguish producer behavior from executable/shader differences before changing implementation.

Evidence: E:/prog/cpu-producer-integration-20260907/launcher-comparison contains original/candidate launcher and settings backups, separate original/candidate probe logs and shared-renderer-cvar-differences.json (zero differences). Current candidate launchers are also saved here after correction.