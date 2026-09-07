# Handoff — 6 Sept 2026

Two threads: a parser fix in the plugin, and the Templar stair-descent animation
in `CitySubwayTrainModular`. The tooling built along the way is in
[`Tools/UnrealPython/`](../Tools/UnrealPython/README.md).

---

## 1. Plugin — meters/mm parser fix

`ShotGrammarParser.cpp` dropped a legitimate distance whenever a clause named both
a distance and a lens: *"pull back 3 meters, 55mm"* parsed with no move amount.

The old code guarded the metres match with `!MatchNumber(... "mm" ...)`. That guard
was never needed — the metres pattern requires a word boundary after `m`, so
`55mm` cannot match it — and it actively threw away good input. Removed the guard
and the duplicate re-match.

Built and deployed to UE 5.8.2. **A restart is required for it to take effect**,
since it ships in the editor DLL.

### Also outstanding in the plugin

- `WastelandGame` has not been rebuilt since July — likely stale against 5.8.2.
- Face bakes are ~12.7 MB each; ~2.60 GB of orphaned face anims remain. Purge with
  the **Purge Unused Face Anims** button in the Face panel, or the
  `CineDirector.PurgeFaceAnims` console command (editor only — it needs a fully
  scanned asset registry, which rules out headless).
- `EngineVersion` in the `.uplugin` stays at `5.8.0`. UAT hardcodes `Major.Minor.0`
  (`BuildPluginCommand.Automation.cs:442`), so patch bumps are cosmetic.

---

## 2. Templar stair descent — `Ship_Demo_Night`

Sequence `/Game/TemplarDoge`, actor `void_4003_Templar`, 30.31s.
Authored by `Tools/UnrealPython/author_stairs.py`. **Level and sequence are saved.**

### The route

Upper deck → flight 1 → mid landing → flight 2 → lower deck. All heights are line
traces, not mesh bounds.

| Segment | Clip | Path | Z | Foot slip |
|---|---|---|---|---|
| walk-in | `walk_cautious_Templar` | (-12820,-470) → (-12827,-5) | 1981.6 | +1% |
| flight 1 | `climb_stairs_down_Templar` | (-12827,-5) → (-12720,249) | 1981.6→1789.4 | **+15%** |
| landing | `walk_cautious_Templar` | (-12720,249) → (-12781,470) | 1789.4 | +5% |
| flight 2 | `climb_stairs_down_Templar` | (-12781,470) → (-12794,756) | 1789.4→1689.6 | +4% |
| walk-out | `walk_cautious_Templar` | (-12794,756) → (-12930,900) → (-12950,1380) | ~1687.4 | −3% |

Verified against collision: worst deviation **19.2 units**, which is one stair
riser — expected for a linear ramp over discrete steps.

### Geometry worth keeping

Decks sit at **1981.6 / 1789.4 / 1687.3**, stepped along +Y rather than stacked.

Stairs are **two mirrored pairs**, each a two-flight descent: port
`SM_Stairs_00`→`SM_Stairs_2`, starboard `SM_Stairs_1`→`SM_Stairs_3`. Every stair
actor has yaw 10, which is *not* the descent direction — the starboard fall line is
**66.5°**. Flight 1's mesh rise reads 264.6 but the **walkable** drop is 192.1; the
extra is handrail.

The mid landing is only ~200 units, so a single-flight descent has nowhere to walk
out to. That is why this is a two-flight move. The lower deck has a clear ~700-unit
lane at X≈-12950.

Two obstacles sit in the obvious line: helm/candelabra clutter at X -12870…-12840,
Y -70…-10 on the upper deck (stay at X ≥ -12830), and a candelabra on the lower
deck near X -12920…-12840, Y≈1350.

### Three bugs fixed, in order

1. **Wrong surface.** He was at Z 2042.4 on a deck that is 1981.6 — floating ~60
   units — and at Y -821.5, which has no floor at all. Cause: measuring the stair
   mesh's `zmax`, which is its handrail.
2. **Double motion.** The clips have root motion disabled and carry travel in the
   **hips** (`walk_cautious` 835.6 units / 9.97s; `climb_stairs_down` 291.1 / 6.97s).
   The earlier script keyed the actor along that same vector, so he moved twice.
   Fixed by subtracting the clip's own travel out of the actor keys:

   ```
   actor(t) = path(t) − R(yaw) · u · dot(hips_local(t), u)
   ```

   where `u` is the clip's unit direction of travel. The character lands on
   `path(t)` exactly while keeping the clip's bob and sway. Per-segment
   `yaw = path_heading − clip_local_heading` (walk 74.3°, stair 94.4°).
3. **Teleports at every segment boundary.** Because each section restarts its clip
   with the hips back at the origin, the compensated actor position is *inherently*
   discontinuous at a boundary — it must jump forward by the whole travel offset at
   exactly the tick the section switches. Writing the outgoing key at `t1` let the
   incoming key overwrite it, so the jump degraded into a ~460-unit slide across the
   previous 0.15s key gap. On screen: a teleport starting at frame 163.
   Fixed by placing the outgoing key at `t1 − 1 tick` carrying the segment's full
   travel. Gap is now 0.0013 frames; audit with `inspect_boundaries.py`.

### Known imperfection

Flight 1 has **+15% foot slip** — the path is 336 units of slope but the clip only
carries 291. Two ways to close it:

- Let the walk-in carry him onto the first step or two, shortening the descent to
  the 291 the clip actually covers. The handoff hides inside the turn.
- Or loop the stair clip to fill the flight.

Everything else is within ±5%.

### Not visually verified

The transform math was checked by hand (at t=9.0 the actor plus rotated travel
gives (-12774.2, 120.7, 1886.0) against an intended (-12773.0, 121.0, 1886.0)) and
the path was checked against collision. **The moving result was never seen from
script** — an unfocused editor never evaluates a skeletal pose, so no screenshot
could confirm it. Judge it by scrubbing in Sequencer.

---

## Assets

- Retarget pipeline: `/Game/CineDirector/Retarget/` — `IK_Animpack2_Source`,
  `IK_TemplarVoid_Target`, `RTG_Animpack2_to_Templar`; output in `Retarget/Anims/`
  with a `_Templar` suffix.
- `void_4003_Templar` shares `void_4003GasMask_Skeleton`, so one target rig covers
  both voids.
- CineDirector's `CineBodyGrammar` has **no locomotion** (sit/stand, smoking,
  look-around, mood dials only), so `AuthorBody` cannot produce a walk. Locomotion
  has to come from a retargeted clip.

## Epic's built-in MCP

`ModelContextProtocol` is enabled and serving on `http://localhost:8000/mcp`. UE 5.8
ships ~27 toolset plugins in `Engine/Plugins/Experimental/Toolsets/`, all disabled
but prebuilt; only `AgentSkillToolset` is on, which is why `list_toolsets` returns
one entry. **None of them can move an actor** — across all 27 the only actor tools
are `GetSelectedActors` / `SelectActors` / `FocusOnActors` / `GetVisibleActors`.
`EditorToolset` is still worth enabling for `CaptureViewport`. Actor and Sequencer
work goes through Python remote execution instead.

Note the plugin's startup EULA warning: data sent through it is Licensed Technology
under UE EULA §6(e), and keeping your LLM provider from training on it is on you.
