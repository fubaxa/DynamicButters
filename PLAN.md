# DynamicButter — Macro System Plan (rev. 2)

Target: **GD 2.2091**, **Geode v6.0.0-alpha** (Windows first).
Scope of this phase: a frame-accurate **record / playback macro** controlled from the **pause menu**,
plus **precise practice checkpoints**, built so the **Dynamic** trainer can reuse the same input pipeline.

Out of scope for now: platformer. 2-player comes later, so the data model and file format already store
player 1 and player 2 inputs separately.

---

## 0. Toolchain: Geode alpha for 2.2091

Status after M0:
- SDK checkout is on `v6.0.0-alpha.1` (targets GD 2.2091), with the v6 binaries installed. The 5.6.1
  binaries are still in `bin/` for BumpscosityMod.
- **Geode CLI 4.0.0 or newer is required.** It was updated from 3.7.4 with `scoop update geode-sdk-cli`.
- Build: Visual Studio 18 2026 generator, x64, same as BumpscosityMod.
- `mod.json`: `"geode": "6.0.0-alpha.1"`, `"gd": { "win": "2.2091" }`.
- v6 API changes found so far: `geode::Popup` is no longer a template (`init(w, h)`). Expect more alpha
  changes, so the UI is written last.

---

## 1. Core idea

```
 user input ──► [Input Interceptor] ──► [Input Scheduler] ──► handleButton() at exact tick
                       │                       ▲
                       ▼                       │
                  [Recorder]            [Playback] (fills the schedule from a Macro)
                                        [Dynamic]  (later: moves a user click to a later tick)
```

- **Frame = `m_gameState.m_currentProgress`.** Measured in M1: it goes up by **2 per physics step**
  (about 480/s) and by 1 on half-steps (`isHalfTick`), so it counts in half-steps. It only changes inside
  `processCommands`, and never jumped. Store it as-is: the extra resolution costs nothing.
- **Confirmed in M1: CBS is the only cause of half-steps.** With CBS on, a press that lands mid-step
  splits that step into two halves (e.g. 716→717→718), and the input is applied between them. The
  logged tick is still the step's end tick (718), so **the recorded tick doesn't capture where in the
  step the click landed**. With CBS forced off: 0 half-steps over 2 runs (~100 inputs), and every input
  fell on an even tick. So recording and playback **must** run with CBS off (§7.6).
- **Confirmed in M1: GD restores the tick on checkpoint load** (`resetLevel` → 0, then
  `loadFromCheckpoint` → the checkpoint's tick). Precise checkpoints don't need to manage the tick themselves.
- **M1 respawn data:** respawning from the same checkpoint gives exactly the same state every time,
  down to 6 decimals, and the state 60 steps later is identical too (when there were no clicks). But the
  respawn state is **not** the state logged at `createCheckpoint`. x is consistently about +1.298
  higher (roughly one step's worth of movement), and y differs when airborne (by −0.40 and −0.98);
  y-velocity and rotation match. This may be real GD offset or just where in the step the checkpoint
  log runs. Settle it in M4 by comparing a respawned run against the original run at the same tick.
- **Recording** saves the tick at which GD *actually applied* the input (inside `handleButton`).
- **Playback** fires `handleButton` at the start of the matching tick and blocks the player's own inputs.
- **Physics correction:** the most precise bots also save player position, rotation and velocity at
  every input, and write it back during playback right after the input is applied. Any float drift is
  wiped out at each click, so long macros and macros recorded in practice mode stay in sync. We do the same.
- **Dynamic** (later) = a new producer for the same scheduler.

---

## 2. Hook points (re-check against the 2.2091 bindings)

**Checked against the 2.2091 bindings during M0:**
- `queueButton` is **inlined on Windows**, so it can't be hooked. We intercept user input in
  `processQueuedButtons` (which drains GD's input queue each tick) and `handleButton` instead.
- `PlayLayer::init` is now `(GJGameLevel*, bool dontCreateObjects)`. The `useReplay` argument is gone.
- `PlayerObject::loadFromCheckpoint(PlayerCheckpoint*)` exists. GD's own `PlayerCheckpoint` is a good
  starting point for the snapshot in §5.
- All other hooks below exist on Windows with the signatures shown.

| Hook | Purpose |
|---|---|
| `GJBaseGameLayer::processQueuedButtons(float dt, bool clearInputQueue)` | Where user inputs leave GD's queue. Block them during playback; Dynamic intercepts here. (`queueButton` is inlined on Windows.) |
| `GJBaseGameLayer::handleButton(bool down, int button, bool isPlayer1)` | Record the input, its tick and the physics snapshot. |
| `GJBaseGameLayer::processCommands(float dt, bool isHalfTick, bool isLastTick)` | Per-tick driver: fire the inputs scheduled for this tick and apply physics corrections. |
| `PlayLayer::init(GJGameLevel*, bool)` / `onQuit` | Bind the macro to the level, reset state. |
| `PlayLayer::resetLevel` | Reset the input cursor, release held buttons, cut the recording back. |
| `PlayLayer::createCheckpoint` / `markCheckpoint` / `loadFromCheckpoint` / `removeCheckpoint` | Precise checkpoints (§5) and cutting the recording back to the checkpoint. |
| `PlayLayer::levelComplete` (+ progress-saving paths) | Safe mode. |
| `PauseLayer::customSetup` | Pause-menu button. |

---

## 3. File format: GDR2 (`.gdr2`)

**What the precise bots use:** Mega Hack and xdBot save **`.gdr2`**, maxnut's *GD Replay Format 2*
(zBot uses the older `.gdr`; xdBot also had its own `.xd`). GDR2 is a public, compact binary format,
and it is the closest thing to a shared standard: converters and clickbots (ZCB3, GucciBot) read it.
**Decision: save as GDR2.** Other tools can then read our macros, and the format has a defined place
for physics correction data.

Layout (per the `gdr2` branch source; varints are LEB128):

```
"GDR" | version=2 (varint) | inputTag (string, e.g. "Phys")
metadata: author, description, duration(float s), gameVersion, framerate(double, 240),
          seed, coins, ldm, platformer, botName, botVersion, levelID, levelName,
          replayExtension (varint size + bytes)            ◄── our replay-level data goes here
deaths:   count + frame deltas
inputs:   totalCount, p1Count, [p1 records...], [p2 records...]   ◄── P2 already separate (good for later)
record:   varint (frameDelta << 1 | down)  [+ varint extSize + extBytes if inputTag set]
```

- **Per-input extension "Phys"** (the standard one): `xPos, yPos, rotation (float), xVel, yVel (double)`.
  Using the standard tag means other bots can read our physics data too.
- **Replay extension (ours, size-prefixed so other readers skip it):** DynamicButter version,
  safe-mode flag, checkpoint/seek snapshots (§6), and later the Dynamic per-input windows (`earliest/latest`).
- **Implementation:** the reference library is header-only C++20 and installs with CPM
  (`CPMAddPackage("gh:maxnut/GDReplayFormat#<commit>")`). **Its licence is unclear** (no LICENSE file was
  found on the `gdr2` branch). Either ask the author or write our own ~200-line reader and writer from the
  spec. Our own version also makes it easy to add the Dynamic extension. Test round-tripping against
  files made by another bot.

---

## 4. Components / file layout

```
src/
├─ main.cpp
├─ macro/
│  ├─ Macro.hpp                // MacroInput{frame, down, player2, PhysSnapshot}, Macro{meta, p1, p2}
│  ├─ Gdr2.hpp/.cpp            // GDR2 read/write (+ our replay extension)
│  ├─ MacroManager.hpp/.cpp    // singleton: Off / Record / Playback, current macro, cursor
│  └─ InputScheduler.hpp/.cpp  // scheduled inputs, dispatch(tick), releaseAll()
├─ physics/
│  ├─ PlayerSnapshot.hpp/.cpp  // capture/restore full PlayerObject state
│  └─ PreciseCheckpoints.cpp   // CheckpointObject $modify + PlayLayer checkpoint hooks
├─ hooks/
│  ├─ GJBaseGameLayer.cpp
│  ├─ PlayLayer.cpp
│  └─ PauseLayer.cpp
└─ ui/
   ├─ MacroPopup.cpp           // mode, stats, save/load, clear
   └─ MacroListPopup.cpp
```

Mode changes take effect on the next `resetLevel`; Stop takes effect immediately.

---

## 5. Precise practice checkpoints (built into the mod)

The problem: vanilla checkpoints don't fully restore player state (position, velocity and rotation lose
precision, and some flags aren't saved), so the run after loading a checkpoint is slightly different from
the original run. That breaks practice-recorded macros, and later it would make Dynamic's frame windows
wrong after a respawn.

Fix:
- `$modify(CheckpointObject)` with a `PlayerSnapshot` field for P1 (and P2 later), plus the tick.
- `createCheckpoint` → call the original, then capture: position, x/y velocity (double), rotation,
  gravity, gamemode, mini/dual, speed, on-ground/jump-buffer flags, and anything else that turns up as
  missing while testing.
- `loadFromCheckpoint` → call the original, then restore the snapshot and set the tick back to
  the checkpoint's tick. In record mode, cut the recording back to that tick and re-press any held button.
- Test: put a checkpoint down, load it 20 times, and log `(tick, x, y, yVel)` 60 ticks later. The values
  must be identical every time and match the original run.
- Setting: **Precise checkpoints** (on by default). It works without macros too.

---

## 6. Start positions (needed for Dynamic only)

You're right that *predicting* where the macro is at a start position doesn't work: the speed changes,
the gamemode affects the hitbox, and a start position spawns with a different state than the real run.
So we **never predict; we simulate**:

**Primary: "seek" (macro start positions).**
1. When Dynamic loads a level with a macro, play the macro from tick 0 once in **fast-forward**:
   several hundred ticks per rendered frame, rendering and audio off, with a progress bar.
2. On the way, save precise snapshots (§5's `PlayerSnapshot` + the game's own checkpoint state) at the
   spots the user picked. A spot can be "where a vanilla start position is", "every N seconds", or
   "here" chosen in a timeline UI.
3. "Start from here" = load that snapshot as a checkpoint. The tick is known exactly, the player
   state is exact, and the speed portals have already been passed for real.
4. Snapshots go in the replay extension, so the seek only happens once per macro (as long as their
   size stays reasonable).

**Fallback: vanilla start position objects.** If the user insists on a real start position: the macro
already stores each input's x-position in "Phys", so find the macro tick where the player first crosses
the start position's x and use that as the tick offset. Speed portals and gamemodes are already in the
recorded data, so nothing is predicted. Accuracy is ±1 tick, and the start position's spawn state
(y-velocity 0, etc.) can still make the run differ. Show this as "approximate".

Open item: fast-forward speed. A 2-minute level is ~29k ticks; check how many ticks per frame GD can run
when triggers and objects are dense.

---

## 7. Hard parts

1. **Held buttons across reset/death:** `releaseAll()` on reset; re-press when loading a checkpoint while held.
2. **Duplicate inputs:** ignore a press while already pressed, and a release while already released.
3. **Determinism log:** a debug toggle that logs `(tick, x, y, yVel)` every 240 ticks for record vs playback.
4. **Physics bypass / TPS:** save the framerate in the file; refuse playback if it differs.
6. **Click Between Steps (vanilla CBS) and Click Between Frames (CBF mod).** Both change when inputs
   land inside a step, and CBS is confirmed as the cause of the half-steps seen in M1. While recording or
   playing back, the mod turns CBS off by setting `GJBaseGameLayer::m_clickBetweenSteps = false` on
   that PlayLayer only. The saved GD option and the level's `m_cbsOverride` are left alone.
   **This has to be enforced every frame** (in `GJBaseGameLayer::update`, before physics runs). In M1
   GD turned the flag back on mid-level, probably when the option was toggled from the pause menu,
   and setting it once at init didn't hold. Every half-step seen so far happened with the flag on, so
   it is the right switch. CBF is
   skipped for now because it hasn't been updated to 2.2091; once it is, it gets disabled or detected
   the same way.
7. **P1/P2 keys.** `handleButton`'s third argument is the *key* used (the up arrow is the P2 key by
   default). In 1-player levels both keys control player 1. Macros store the flag exactly as GD gives
   it, which maps directly onto GDR2's separate P1 and P2 input lists, and playback passes it back
   unchanged. That gives 2-player support almost for free later.
5. **Safe mode** (mod setting, **on by default**, can be turned off): any attempt that used a speedhack other than 1.0, playback,
   Dynamic or a seek snapshot can't save progress or completion or submit to leaderboards. Show a small
   "Safe mode" label in-game while it's active.

---

## 8. Settings (`mod.json`)

| Key | Default | Notes |
|---|---|---|
| `safe-mode` | `true` | Turn off at your own risk |
| `precise-checkpoints` | `true` | |
| `checkpoint-test` | `false` | Debug, M4: automatic checkpoint precision test |
| `speedhack` | `1.0` | 0.1–2.0, game + music speed while in a level. Any attempt not at 1.0 is safe-mode only (M7) |
| `macro-mode` | `Off` | Off / Record / Playback. Temporary until the M6 popup; applies at the next attempt |
| `physics-correction` | `true` | Apply "Phys" snapshots during playback |
| `show-tick-counter` | `true` | Overlay (top-left). Added in M1 |
| `log-inputs` | `true` | Debug, M1. Turn the default off before release |
| `tick-probe` | `true` | Debug, M1. Turn the default off before release |
| `macro-folder` | save dir `/macros` | |

---

## 9. Milestones

| # | Milestone | Done when |
|---|---|---|
| M0 ✅ | Geode v6 alpha SDK + scaffold, check the hooks against the 2.2091 bindings | Mod loads in 2.2091 and logs a message |
| M1 ✅ | Tick counter overlay + log every applied input | Tick = +2 per step; CBS confirmed as the only source of half-steps and forced off every frame |
| M2 ✅ | Recording (normal mode) with Phys snapshots: `MacroManager` + `Macro`, temporary `macro-mode` setting (Off/Record), duplicate filter, practice cut-back, text dump on completion | Stereo Madness: 324 inputs, end tick 41138 (85.70 s at 480 ticks/s). Matches the log exactly apart from GD's own tick-2 release (dropped on purpose). No odd ticks, press/release always alternate |
| M3 ✅ | Playback + physics correction. Verified: Stereo Madness replayed to 100% with correction off and on; 324/324 inputs, 0 late, max drift 0.000000 in x, y and y-velocity. Built: macro inputs applied right after `processQueuedButtons` when tick == frame (fallback after `processCommands`, counted as "late"); player's presses blocked; drift measured before every correction; plays the latest completed recording (memory, else newest dump file). Also added: pause-menu "DB" button that opens the settings, and the tick overlay freezes at level complete and shows REC/PLAY. The scheduler is still a cursor over the macro; the general InputScheduler for Dynamic comes in M9 | Easy level replays to 100% several times in a row, first with correction off (pure frame accuracy), then on |
| M4 ✅ | Precise checkpoints. **Result: on Stereo Madness GD's own checkpoints are already exact**: 31/31 tests EXACT with precise checkpoints OFF, 33/33 with ON (respawn state and the next 120 steps identical). The +1.298 x offset seen in M1 came from the old log reading the state mid-step, not from GD. Precise checkpoints stay as a safety net for levels and gamemodes not tested yet. Known issue (debug only): the test keeps running after the level is complete. Built: `PlayerState` snapshot taken at the end of the step a checkpoint is created in (stored on `CheckpointObject` via `$modify` fields), compared against GD's respawn and restored when `precise-checkpoints` is on. Plus an automatic test (`checkpoint-test`): during practice-mode playback, every 2.5 s it places a checkpoint, records 0.5 s of the original run, respawns, and compares the replay tick by tick | Checkpoint test reports EXACT for every test with precise checkpoints on (and shows whether GD alone is exact with it off) |
| ➕ Speedhack ✅ | `speedhack` setting (0.1–2.0): scales `CCScheduler::update`'s dt and the music pitch while a level is open | Verified: record and playback at different speeds both worked |
| ➕ Pause fix ⏸ deferred | Pausing during a macro hold breaks the hold. See "Open issues" at the end | — |
| M5 | Practice-mode recording (cut-back + re-press) | Practice-recorded macro replays in normal mode |
| M6 | GDR2 save/load + pause-menu UI | Round-trip works; a `.gdr2` from another bot plays back |
| M7 | Safe mode + settings | Playback completion doesn't save progress; setting turns it off |
| M8 | Seek / macro start positions | Start from a snapshot, play the macro the rest of the way, beat the level |
| M9 | Dynamic hook-in point | Scheduler takes "delay this user input to tick X" |

---

## 10. Dynamic preview (not built now)

- `leniency = N` frames. A user press at tick `f` is matched to the next unused macro press `p` with
  `p − N ≤ f ≤ p`. On a match the click is held and fired at `p`; otherwise it goes through unchanged.
- Releases get the same treatment (or their own leniency setting).
- Real windows (e.g. 5136–5138) can later be saved per input in the replay extension.
- The cursor of used inputs resets on reset and moves back when a checkpoint or seek snapshot is loaded.

---

## 11. Open issues

- **Pausing during a macro hold breaks the hold (deferred, low priority).** Found in M4 testing.
  Pausing (for example to change a setting) during a long hold in playback cuts the hold, and the
  player dies or drifts.
  - What we know: pausing and resuming call `PlayerObject::releaseAllButtons()` directly (4× on
    pause, 2× on resume), not through `handleButton`, so the macro never sees the release.
  - Tried: skipping `releaseAllButtons()` during `PlayLayer::pauseGame`/`resume` while a button is
    held. The calls were blocked, but the hold still broke (the next macro input after resuming had
    dy=62, dyVel=3.8 of drift). So GD also clears the hold somewhere else, possibly
    `GJBaseGameLayer::removeReleasedButtons`, the input queue, or `PlayerObject::m_holdingButtons`.
  - Ideas for later: find the other place GD clears the hold, or re-press the held key on resume
    (that adds an extra press edge, which could trigger an orb/jump buffer, so it needs care).
  - Recording is affected the same way: a pause mid-hold cuts the hold in-game but not in the recording.
  - `PlayerObject.cpp` keeps a debug log of the calls for when this is picked up.
- **Checkpoint test keeps running after the level is complete** (debug tool only, not fixed on purpose).
