# MilkDrop texture pack (user-supplied)

This directory is the resolution target for `.milk` preset shader/wave/shape
references to `sampler_FILENAME` textures (e.g. `sampler_treble`,
`sampler_wood`), per `plans/PRD-zge-milkdrop-visualizer.md` §Pillar A
(FR-A7) and the projectM README's linked texture pack:
https://github.com/projectM-visualizer/presets-milkdrop-texture-pack/tree/master

**Not vendored here** — unlike the tiny MIT/LGPL files elsewhere in this
repo (`neo/framework/projectm-eval/`, `base/presets/milk/*.milk` test
fixtures), the texture pack is a large third-party community asset
collection meant to be installed by the end user, not redistributed as part
of this engine's source tree. To use it:

1. Download the pack from the URL above (or any MilkDrop-compatible texture
   pack — most preset packs recommend installing this one regardless of
   which preset pack you use).
2. Extract its images directly into this directory (`base/textures/milkdrop/`),
   flat or in subfolders — the sampler-resolution code (part of the M3
   HLSL-shader-preset milestone, not yet implemented) will search this
   directory tree by filename, matching MilkDrop's own `sampler_FILENAME`
   convention (`fw_`/`fc_`/`pw_`/`pc_` filter/wrap prefixes stripped before
   the filename lookup).

Until the M3 sampler-resolution code lands, presets that reference custom
textures will simply not find them — this is a known, documented gap (see
`plans/PRD-implementation-status.md` M1), not a bug.
