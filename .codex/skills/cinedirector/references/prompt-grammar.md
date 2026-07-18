# CineDirector prompt grammar

Each split clause becomes one shot segment. The parser recognizes these terms; phrase target actor labels distinctly so fuzzy matching can resolve them.

| Category | Supported language |
| --- | --- |
| Moves | `orbit/circle around`, `dolly/push in`, `pull back`, `truck/track left/right`, `crane/boom/pedestal up/down`, `pan`, `tilt`, `zoom`, `flyover/drone/aerial`, `static/locked` |
| Framing | `extreme close-up`, `close-up`, `medium close-up`, `medium`, `wide`, `extreme wide`, `establishing` |
| Angle/side | `low angle`, `high angle`, `overhead/top-down/bird's eye`, `eye level`, `from behind/front/left/right`, `from its left/right`, `over the shoulder`, `looking at <actor>` |
| Lens/focus | `<number>mm`, `wide-angle`, `portrait`, `telephoto`, `f/<number>`, `shallow focus`, `deep focus`, `focus on <actor>`, `rack focus from <actor> to <actor>`, `track` |
| Effects | `slightly handheld`, `handheld`, `very shaky`, `dutch/canted angle`, `film grain`, `vignette`, `chromatic aberration`, `bloom`, `lens flares` |
| Lighting | `dawn`, `morning`, `noon`, `afternoon`, `golden hour`, `sunset`, `dusk`, `night`, `midnight`, `overcast`, `light/heavy/no fog`, `god rays`, `volumetric fog` |
| Timing/amount | `over 8 seconds`, `for 3s`, `slow`, `fast`, `90 degrees`, `half/full orbit`, `by 5 meters` |

## Defaults and semantics

- Default shot duration: 5 seconds; lens: 35mm; aperture: f/2.8.
- Orbit, pan, and tilt use degrees. Dolly, truck, crane, and flyover use centimeters; meters in prompts are converted. Zoom amount is target focal length in mm.
- `from its left/right` and `from behind it` are actor-relative; `from the left/right` is relative to the current viewport.
- `rack focus from A to B` needs two resolvable targets. `track` locks autofocus on the target.
- `it` in a clause refers to the previous clause's subject ("push in on the Knight, then orbit around it").
- `one take` / `continuous` chains moves on one camera and suppresses cuts; otherwise every segment receives a camera and a cut. In a continuous take, a framing word mid-take becomes an implicit dolly to that framing.

## Prompt patterns

```text
Wide low-angle push in on the Hero over 4 seconds, 50mm, shallow focus.

Orbit around the Tower looking at the Hero, half orbit, from its left, at sunset.

One continuous take: flyover of the Castle for 5 seconds, then crane down 4 meters looking at the Gate, then push in on the Gate, handheld.
```

## Trailer style grammar

The trailer Style box uses the same philosophy: plain words → concrete knobs. Combine freely; unmentioned aspects default (natural colors, light grain, 4 long beats, eerie score with whistle motif).

| Aspect | Supported language |
| --- | --- |
| Look | `found footage` / `security cam` / `cctv` / `vhs` / `bodycam` (full kit: shake + flicker + chroma fringing + REC/tag/timecode overlays), `handheld` / `shaky` (shake only), `cinematic` / `letterbox` (cinemascope bars) |
| Grade | `black and white` / `noir`, `green` / `alien` / `sickly`, `warm` / `sepia`, `cold` / `icy`, `natural` / `no grade` (default: footage's own colors) |
| Texture | `flicker`, `grainy` / `heavy grain`, `no grain` / `crisp` |
| Pacing | `fast cuts` / `frantic` (6 escalating shrinking beats) vs `slow` (4 long beats) |
| Music | `eerie` (drone + whistle), `tense` / `heartbeat` (pulsing bed, frequent motif), `somber` / `mournful` (slower, softer), `no music`, `drone only` / `no whistle` |
| Titles | `bold title` / `blockbuster` (heavy bold instead of letterspaced serif) |

```text
found footage, security cam, natural colors, eerie music

cinematic letterbox, black and white, somber, slow burn

green alien grade, fast cuts, tense heartbeat, bold title
```
