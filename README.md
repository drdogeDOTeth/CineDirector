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

The edit is described in plain language. The **Style** box alone drives the look
(the checkbox only enables trailer mode — it does **not** force found-footage).
Audio is separate (keep render audio / synthetic score toggles).

Examples:

> music video, scanlines, film grain, hard cuts, zoom in  
> horror, cold, vignette, heavy grain, slow burn, dutch  
> found footage, security cam, natural colors

The parser understands **kits**, **grades**, **camera effects**, **texture**,
**transitions**, and **pacing**. Full vocabulary is listed under the Style box in
the panel (and in `FCineTrailerProcessor::GetStyleVocabulary()`). Highlights:

| Category | Examples |
| --- | --- |
| Kits | `music video`, `found footage` / `cctv`, `horror`, `action`, `thriller`, `romance`, `documentary`, `fashion`, `vaporwave`, `cyberpunk`, `anime op`, `commercial` |
| Frame | `letterbox` / `cinematic`, `dutch`, `zoom in` / `push in`, `mirror`, `handheld` / `chaotic cam` |
| Grade | `b&w` / `noir`, `warm` / `sepia`, `cold`, `teal and orange`, `neon`, `bleach bypass`, `vintage` / `80s`, `high contrast`, `desaturated`, `overexposed`, `natural` |
| Texture | `film grain` / `grainy` / `16mm`, `scanlines` / `crt` / `vhs`, `flicker`, `chromatic`, `glitch`, `pixelate`, `vignette`, `bloom`, `soft` / `dreamy`, `sharpen` |
| Pacing | `fast cuts` / `montage` / `smash cuts`, `slow burn`, `hard cuts`, `soft fades` |
| Titles | `bold title` / `blockbuster` |
| Score mood* | `eerie`, `tense` / `heartbeat`, `somber`, `upbeat`, `drone only` (*only if synthetic score is checked) |

The parsed interpretation is logged (`LogCineDirectorTrailer`) so you can confirm
how the description was understood. Anything unmentioned gets a sensible default.
Title, three teaser card lines, and the camera tag are all editable in the panel,
and finished trailers never overwrite earlier ones.

## Body Performance

Describe a body performance in plain language and CineDirector authors a
keyframed animation from scratch — no source clips, no retargeting. Select a
character, **Use Selected Actor**, type e.g. `sitting on the bench smoking,
nervous, looking around, for 20 seconds`, and **Generate Body Performance**
bakes a looping bone-track animation (poses, two-bone arm IK with palm-facing
control, cigarette finger grips, breathing/sway, mood ticks like foot taps and
knee bounces, per-joint follow-through and overshoot) and layers it onto the
character in the open Level Sequence.

- **Base**: `sitting` (bench/stool/chair/couch) or standing (default)
- **Activities**: `smoking`, `looking around` — combined freely; none = idle
- **Mood**: `nervous`, `chill`, `alert` (+ `very`/`slightly`) — drives tempo,
  slouch, tick habits and how snappily joints move
- **Duration**: `for 20 seconds` (defaults picked from the activities)

Works on the void-family VRM rigs and Blender-Mixamo (`mixamorig_*`) rigs; the
parsed interpretation is logged (`LogCineDirectorBodyGrammar`). Body and face
output stack on the same actor — a full performance is body + face + lipsync +
audio from two text boxes. Also available headless:
`CineDirector.AuthorBody <MeshName> <description...>` bakes the asset and
writes a stick-figure preview sheet to `Saved/CineDirectorBody/`.

## Face & Lipsync

The **Face & Lipsync** section animates faces and blends them with body
animation. Select a character, and **Analyze Face** maps its morph targets onto
CineDirector's canonical face slots (ARKit blendshapes, Oculus/Reallusion
visemes, and MetaHuman `CTRL_expressions` controls are recognized by name;
anything else is fuzzy-matched). Then:

**Void / VRM characters — prefer the FBX for faces.** The void kits ship both
GLB and FBX. GLBs typically keep only the small VRM set (`A/I/U/E/O`, Joy/Angry,
blinks, look dirs). FBXs keep that set **plus** ~48 ARKit shapes (`jawOpen`,
`mouthPucker`, smiles, brows, teeth reveal, …). CineDirector detects the rich
ARKit set, prefers those curves over duplicate VRM vowels/full-face poses (so
lips/brows don't double-drive and stretch), and runs layered MetaHuman-style
lipsync. On GLB-only meshes it falls back to exclusive vowel mode and notes that
reimporting the FBX unlocks better face detail.

- **Audio-driven lipsync** — point at a dialogue file (WAV directly; MP3/OGG/
  FLAC/M4A via ffmpeg). The energy envelope drives the jaw, spectral balance
  shapes the mouth, dips become M/B/P closures. Pronunciation is built up
  MetaHuman-style: lips pre-shape ~50 ms ahead of the sound, rounded/wide
  vowels keep the jaw dropped underneath, teeth reveal on open shapes
  (upper-lip raise / lower-lip depress where the rig has them), and mouth-area
  emotion (smile/frown/press) yields while words are being spoken so visemes
  stay legible — brows and eyes keep carrying the emotion. The audio is
  imported and placed on the sequence's audio track, synced to the animation.
- **Articulation slider** — how crisply the mouth hits each shape: below 1 is
  soft and mumbly, above 1 snaps consonants and peaks vowels for full stage
  enunciation.
- **No audio yet** — "Talking" synthesizes natural syllables and pauses.
- **Emotions in plain language** — `scared`, `angry`, `happy`, `sad`,
  `surprised`, `disgusted`, `pain`, `suspicious`, `calm`, with `slightly`/`very`
  modifiers and `then` for arcs (`calm then very scared`), plus auto-blinks.

The result is a curves-only **additive** animation asset layered onto the
character in the open Level Sequence — it carries no bone data, so the body
animation underneath keeps playing and only the face moves. Face and body work
together with zero setup.

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
- `CineFaceAnalyzer` / `CineLipsync` / `CineFaceBaker` — morph mapping, audio analysis, and the additive face-animation bake.
- `CineDirectorSkeletonAnalyzer` / `CineDirectorIKRigGenerator` / `SCineDirectorAutoRetargetPanel` — the Auto Retarget workflow.
- `SCineDirectorPanel` / `CineDirectorModule` — the editor tab wiring.

The provider interface exists so an LLM-backed provider (e.g. Claude) can be dropped
in later without touching the executor or UI — and the trailer style description is
the same kind of seam on the editing side.
