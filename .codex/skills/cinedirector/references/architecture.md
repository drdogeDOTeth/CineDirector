# CineDirector architecture

## Data and provider layer

- `ShotPlanTypes.h`: `FCineShotPlan`, `FCineShotSegment`, and `FCineSceneContext`; contract between parsing and execution.
- `IShotPlanProvider.h`: provider abstraction. An LLM-backed provider can replace the built-in parser without executor/UI changes.
- `ShotGrammarParser.*`: deterministic offline parser. It splits clauses, resolves actor labels, applies grammar/defaults, and reports assumptions in `ParseNotes`.

## Editor execution layer

- `ShotPlanExecutor.*`: snapshots actors and editor viewport; spawns `ACineCameraActor`s; authors transform, lens, focus, and camera-cut tracks. It appends after existing cuts and runs in a single undoable transaction.
- `SCineDirectorPanel.*`: Slate tab, prompt text, one-take toggle, builder presets/chips, parser/executor result display, and render controls.
- `CineDirectorModule.cpp`: editor-tab and provider wiring.

## Rendering layer

- `CineRenderLauncher.*`: queues the open saved Level Sequence and saved map in Movie Render Queue using PIE. Supports PNG, JPEG, EXR (reflectively loaded, with PNG fallback), BMP, MP4 (MRQ command-line encoder; ffmpeg discovered via project settings → PATH → WinGet packages), resolution, and temporal anti-aliasing sample count.
- `CineTrailerProcessor.*`: `ParseStyle` maps a plain-language edit description to `FCineTrailerStyle` (grade filter, letterbox, found-footage kit, grain level, beat fractions, score mood, title font); `ProcessAsync` runs the ffmpeg pipeline on a background thread — beat slicing with conditional filter chains, title/card generation, per-mood synthesized score with an original whistle motif, final mux. `FindNewestMp4` locates the render that just finished. The panel chains it off `OnExecutorFinished` when Trailer mode is on.

## Face layer

- `CineFaceTypes.h`: 15 canonical face slots (JawOpen ... EyeSquint), `FCineFaceProfile` (mesh → slot curve mapping), `FCineVisemeFrame`.
- `CineFaceAnalyzer.*`: maps a mesh's morph targets to slots — exact tables for ARKit/Oculus/Reallusion names, keyword fuzzy fallback, MetaHuman detection (Face_Archetype skeleton or CTRL_expressions curves) with a rig-control preset.
- `CineLipsync.*`: RIFF WAV parser (+ ffmpeg conversion for other formats), 30fps energy/3-band Goertzel analysis into viseme frames, and procedural talking synthesis.
- `CineFaceBaker.*`: emotion keyword poses with `then` arcs and auto-blinks; bakes slot timelines into a curves-only additive UAnimSequence (AAT_LocalSpaceBase/ABPT_RefPose, zero bone tracks — safe to layer over body animation); imports audio and adds both to the current Level Sequence in one transaction.
- `SCineDirectorFacePanel.*`: the panel section.

## Auto Retarget layer

- `CineDirectorSkeletonAnalyzer.*` / `CineDirectorIKRigGenerator.*` / `CineDirectorAutoRetargetTypes.h` / `SCineDirectorAutoRetargetPanel.*`: interactive Skeletal Mesh → IK Rig workflow (analyze skeleton, edit chain mappings, generate rig, save/load profiles).

## Change checklist

1. Add a segment field only if parser and executor require durable shared state.
2. Parse the requested language, define default behavior, and emit notes for uncertainty.
3. Execute it by authoring the appropriate camera, track, or world change.
4. Expose it in vocabulary help; add builder affordances if common.
5. Keep changes undoable and compatible with appending to existing sequences.
