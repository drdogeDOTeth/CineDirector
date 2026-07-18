---
name: cinedirector
description: Create cinematic sequences with the CineDirector Unreal Engine editor plugin and develop, debug, or extend its UE 5 C++ code. Use when writing CineDirector natural-language shot prompts, troubleshooting shots, cameras, Sequencer tracks or Movie Render Queue renders, or changing files under CineDirector/.
---

# CineDirector

Use CineDirector as a local UE editor plugin, not as a generic video-production tool. It converts a description into cameras, Sequencer animation, focus, effects, lighting changes, and optional Movie Render Queue output.

## Create a shot sequence

1. Confirm a Level Sequence is open in Sequencer. Use exact or distinctive Outliner labels for people, props, and locations.
2. Write one self-contained clause per intended shot. Join cuts with `then`, `cut to`, `next`, `after that`, a period, or a semicolon.
3. Put target, move, framing, timing, and artistic modifiers in the same clause. Use `looking at <actor>` when the move pivots around one subject but should aim at another.
4. Say `one take` or `continuous` (or enable **One continuous take**) only when all clauses should chain on a single camera with no cuts.
5. Create the shots. Check the panel's notes for fuzzy-match or parser assumptions, then use one undo if the result is unsuitable.

Read [references/prompt-grammar.md](references/prompt-grammar.md) for vocabulary, defaults, and prompt patterns.

## Render the sequence

Before starting a render, save both the level and the Level Sequence. CineDirector's Movie Render Queue integration cannot load unsaved `/Temp/` assets. In the panel choose resolution, format (PNG/JPEG/EXR/BMP/MP4), quality, and an optional output folder, then use **Render Sequence**. MP4 encodes through MRQ's command-line encoder and requires ffmpeg on the machine (auto-discovered via project settings, PATH, or a WinGet install).

## Cut a trailer

Enable **Trailer mode** on an MP4 render to have the finished render cut into a trailer automatically (beats + title cards + synthesized score, all via ffmpeg on a background thread, never overwriting earlier trailers).

Describe the edit in the **Style** box in plain language — look (`found footage`, `security cam`, `vhs`, `cinematic letterbox`), grade (`black and white`, `green/alien`, `warm`, `cold`, `natural` — default is the footage's own colors), texture (`handheld`, `flicker`, `grainy`, `no grain`), pacing (`fast cuts` vs `slow`), music (`eerie`, `tense/heartbeat`, `somber`, `no music`, `drone only`), and `bold title`. Unmentioned aspects get defaults; the parsed interpretation is logged to `LogCineDirectorTrailer`. Title, three card lines, and the camera tag are editable fields. See [references/prompt-grammar.md](references/prompt-grammar.md) for the full style vocabulary.

## Animate a face

In **Face & Lipsync**: select the character, **Use Selected Actor**, then **Analyze Face** to confirm morph-target mapping before generating (paste unmapped morph names into the analyzer's tables if slots are missing). Provide dialogue audio (.wav directly; mp3/ogg/flac/m4a require ffmpeg) for audio-driven lipsync, or leave it empty with "Talking" checked for synthesized speech motion. Describe emotion in plain language (`scared`, `angry`, ... with `slightly`/`very` and `then` arcs). The bake is a curves-only additive UAnimSequence layered onto the character in the open Level Sequence — body animation is untouched; audio lands on the sequence's audio track.

## Develop the plugin

Preserve the separation between interpretation and execution:

- Keep natural-language recognition and defaults in `ShotGrammarParser` or another `IShotPlanProvider`.
- Represent parsed features in `FCineShotSegment` / `FCineShotPlan` before using them in the executor.
- Put world, CineCamera, and Sequencer track authoring in `ShotPlanExecutor`.
- Keep Slate controls and user-facing status in `SCineDirectorPanel`.
- Keep Movie Render Queue configuration in `CineRenderLauncher`.
- Keep trailer style parsing and the ffmpeg pipeline in `CineTrailerProcessor` (style words → `FCineTrailerStyle` knobs, mirroring the shot-grammar philosophy).

For each new prompt feature, update the parser, data model only when required, vocabulary help, and shot-builder chips/presets where useful. Ensure parser fallbacks result in a clear `ParseNotes` entry rather than silently changing intent.

## Safeguards and verification

- Append new shots after existing camera cuts; do not overwrite a sequence.
- Retain the single scoped transaction so an entire operation is reversible with one undo.
- Keep target-less shots anchored to the editor viewport camera.
- Verify a changed parser with a single shot, a multi-shot prompt, fuzzy target labels, a target-less prompt, and a continuous take.
- Verify render changes with a saved map and saved sequence; test the requested output format and a non-default quality tier.

Read [references/architecture.md](references/architecture.md) before modifying a subsystem boundary or adding a provider.
