# CineDirector

Describe a shot in plain language and CineDirector sets up cine cameras, keyframes,
focus and camera cuts in the Level Sequence currently open in Sequencer.

## Usage

1. Copy the `CineDirector` folder into your project's `Plugins/` directory and rebuild.
2. Open a Level Sequence in Sequencer.
3. Open the panel: **Window ▸ Cinematics ▸ CineDirector**.
4. Type a description and press **Create Shots in Sequencer**.

Example:

> Establishing drone shot over the Castle for 6 seconds. Then a slow 180 degree
> orbit around the Knight, close-up, 85mm, shallow focus. Then push in on the
> Door, handheld, then rack focus from the Knight to the Door.

Each clause (split on periods, semicolons, or "then" / "cut to") becomes one shot:
one `ACineCameraActor`, a transform track with baked keys, lens tracks where needed
(zoom → focal length keys, rack focus → manual focus distance keys), and a camera
cut section. Target actors are fuzzy-matched against outliner labels ("the knight"
finds `Knight_2`). Everything lands in a single undo step, and re-running appends
after the last existing camera cut instead of overwriting.

The full vocabulary (moves, framing, angles, lenses, focus, handheld, dutch,
timing, amounts) is documented in the panel's "What can I write?" expander.

## Architecture

- `ShotPlanTypes.h` — the plan data model (`FCineShotPlan`, `FCineShotSegment`, `FCineSceneContext`).
- `IShotPlanProvider.h` — anything that can turn a description + scene snapshot into a plan.
- `ShotGrammarParser` — the built-in rule-based provider (offline, deterministic).
- `ShotPlanExecutor` — spawns the cameras and authors Sequencer tracks from a plan.
- `SCineDirectorPanel` / `CineDirectorModule` — the editor tab wiring.

The provider interface exists so an LLM-backed provider (e.g. Claude) can be dropped
in later without touching the executor or UI.
