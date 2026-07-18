# CineDirector

Describe a shot in plain language and CineDirector sets up cine cameras, keyframes,
focus, effects, lighting and camera cuts in the Level Sequence currently open in
Sequencer — then renders it out through Movie Render Queue, and can even cut the
render into a styled movie trailer.

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
finds `Knight_2`), and "it" refers back to the previous clause's subject. Everything
lands in a single undo step, and re-running appends after the last existing camera
cut instead of overwriting.

The full vocabulary (moves, framing, angles, lenses, focus, handheld, dutch,
effects, lighting, timing, amounts) is documented in the panel's "What can I
write?" expander, and the Shot Builder offers presets and clickable phrase chips
grouped by category.

Highlights beyond the basics:

- **One continuous take** — say "one take" / "continuous" (or tick the checkbox)
  and every move chains onto a single camera with no cuts; rotation stays smooth
  across the whole take.
- **Look-at and tracking** — "orbit around the Tower looking at the Hero" moves
  relative to one actor while aiming at another; "track the Hero" locks focus on
  a subject.
- **Sides that make sense** — "from the right" is relative to your current
  viewport view; "from its right" is relative to the actor itself.
- **Post-process effects** — film grain, vignette, chromatic aberration, bloom,
  lens flares, with slight/heavy modifiers.
- **Lighting & atmosphere** — time-of-day words (dawn through midnight) key the
  sun, fog words key height fog, plus god rays and volumetric fog; with a
  SkyAtmosphere in the level the sky is driven physically.

## Rendering

The **Render (Movie Render Queue)** section renders the open sequence with your
choice of resolution, format (PNG / JPEG / EXR / BMP / MP4) and quality, into a
folder you pick. Save the level and the sequence first — MRQ cannot load unsaved
assets. MP4 output uses Movie Render Queue's command-line encoder and needs
`ffmpeg` on the machine (CineDirector finds it via project settings, PATH, or a
WinGet install automatically).

## Trailer mode

Tick **Trailer mode** on an MP4 render and, when the render finishes, CineDirector
cuts it into a finished trailer: beats sliced from the footage, letterspaced title
cards between them, a big title reveal, "COMING SOON", and a fully synthesized
score — no external assets needed.

The edit is described in plain language, same as shots. The **Style** box accepts
descriptions like:

> found footage, security cam, natural colors, eerie music

and maps them onto a concrete treatment:

| You write... | You get |
| --- | --- |
| `found footage` / `security cam` / `cctv` / `vhs` / `bodycam` | handheld shake, exposure flicker, chroma fringing, blinking REC + camera tag + running timecode |
| `handheld` / `shaky` | shake without the camera overlays |
| `cinematic` / `letterbox` | cinemascope bars (neutral grade unless a color is named) |
| `black and white` / `noir`, `green` / `alien` / `sickly`, `warm` / `sepia`, `cold` / `icy`, `natural` / `no grade` | color grade (default: the footage's own colors) |
| `grainy` / `heavy grain` / `no grain` / `crisp` | film grain level |
| `fast cuts` / `frantic` vs. `slow` | escalating short beats vs. long beats |
| `eerie`, `tense` / `heartbeat`, `somber`, `no music`, `drone only` | the synthesized score's mood (drone bed + an original 4-note whistle motif) |
| `bold title` / `blockbuster` | heavy bold title instead of the letterspaced serif |

The parsed interpretation is logged (`LogCineDirectorTrailer`) so you can confirm
how the description was understood. Anything unmentioned gets a sensible default.
Title, three teaser card lines, and the camera tag are all editable in the panel,
and finished trailers never overwrite earlier ones.

## Auto Retarget (IK Rig)

The panel also includes an interactive Skeletal Mesh → IK Rig workflow: analyze a
selected mesh's skeleton, review/edit the detected chain mappings, generate the IK
Rig, and save/load mapping profiles for reuse.

## Architecture

- `ShotPlanTypes.h` — the plan data model (`FCineShotPlan`, `FCineShotSegment`, `FCineSceneContext`).
- `IShotPlanProvider.h` — anything that can turn a description + scene snapshot into a plan.
- `ShotGrammarParser` — the built-in rule-based provider (offline, deterministic).
- `ShotPlanExecutor` — spawns the cameras and authors Sequencer tracks from a plan.
- `CineRenderLauncher` — Movie Render Queue job setup (formats, quality, ffmpeg discovery for MP4).
- `CineTrailerProcessor` — style parsing + the ffmpeg trailer pipeline (background thread).
- `CineDirectorSkeletonAnalyzer` / `CineDirectorIKRigGenerator` / `SCineDirectorAutoRetargetPanel` — the Auto Retarget workflow.
- `SCineDirectorPanel` / `CineDirectorModule` — the editor tab wiring.

The provider interface exists so an LLM-backed provider (e.g. Claude) can be dropped
in later without touching the executor or UI — and the trailer style description is
the same kind of seam on the editing side.
