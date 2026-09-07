# Driving a running Unreal editor from Python

These scripts talk to a **live** editor session over `PythonScriptPlugin`'s remote
execution protocol. No need to close the editor, and no need for the headless
`-run=pythonscript` commandlet.

## One-time setup

1. `PythonScriptPlugin` enabled in the `.uproject` (already true for
   CitySubwayTrainModular).
2. Tick **Project Settings → Plugins → Python → Enable Remote Execution**.

That setting hot-applies — `bRemoteExecution` has a `PostEditChangeProperty` that
calls `SyncRemoteExecutionToSettings()`, so **no editor restart is needed**. It
persists to `[/Script/PythonScriptPlugin.PythonScriptPluginSettings]` in
`DefaultEngine.ini`.

## Running a script

```bash
python uepy.py <script.py>
```

`uepy.py` does UDP multicast discovery on `239.0.0.1:6766`, opens the TCP command
channel, runs the file inside the editor, and prints whatever the script logged.
It leans on `remote_execution.py`, which ships at
`Engine/Plugins/Experimental/PythonScriptPlugin/Content/Python/`.

If it reports `NO EDITOR FOUND`, check `Get-Process UnrealEditor` before assuming
the server is off — a busy editor can simply be slow to answer. Discovery waits
30s.

## What each script does

| Script | Purpose |
|---|---|
| `uepy.py` | The client. Everything else is run *through* this. |
| `deck_profile.py` | Traces straight down along the ship's length to find each deck. |
| `deck_map.py` | Compact walkable/obstacle grid for a deck, ignoring the character. |
| `measure_stairs.py` | Fall line, rise, run and pitch of each stair flight, by least squares. |
| `verify_route.py` | Checks an authored foot path against collision — floats and sinks. |
| `author_stairs.py` | Builds the Templar stair-descent sequence. |
| `attach_aura.py` | Parents `BP_Aura_Gold` to the Templar inside a level sequence. |
| `inspect_boundaries.py` | Audits anim-section boundaries against the transform keys. |
| `cleanup_temp_actors.py` | Sweeps `CDTEST_` / `CDPREVIEW_` / `CDMARK_` actors and scene captures. |

## Hard-won gotchas

- **An unfocused editor ticks but does not redraw.** Remote execution keeps
  answering, but `HighResShot` silently writes nothing and **no skeletal pose ever
  evaluates** — `set_current_time` moves transform tracks while the mesh stays in
  its reference pose, and `play_animation` + `set_position` never advances
  (`get_position()` stays at 0.00). Suspect `VisibilityBasedAnimTickOption`
  defaulting to *OnlyTickPoseWhenRendered*. Do not try to screenshot a posed
  character while the editor window is in the background.
- **Scene captures do work** (they are an explicit render command). But
  `create_render_target2d` defaults to a float format and `export_render_target`
  then writes **EXR bytes under a `.png` name** — pass
  `TextureRenderTargetFormat.RTF_RGBA8`. On a night map use
  `SceneCaptureSource.SCS_BASE_COLOR` or the image is unreadably dark.
- **Traces work here** but return zero hits in a headless commandlet, which has no
  physics scene.
- **Mesh bounds lie about walkable height.** A stair mesh's `zmax` is its handrail.
  Only line traces give the tread.
- **Transform keys are stored as display-rate FrameTime with subframes** even when
  added with `MovieSceneTimeUnit.TICK_RESOLUTION`, while section ranges come back
  in **ticks**. Convert before comparing:
  `ticks = (frame_number + sub_frame) * tick_rate / display_rate`.
- `SkeletalMeshComponent` has no `refresh_bone_transforms` in 5.8, and
  `MovieSceneEasingSettings` has no `auto_ease_in_duration`.
