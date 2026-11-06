#pragma once

#include <cstdint>
#include <string>

class PlayLayer;
class GameObject;

// Dynamic window scan (PLAN.md §10, "sections between resting points").
//
// A click at frame f is valid when the run comes back to exactly the recorded run: identical
// player state (ExactState.hpp) for CONFIRM_STEPS steps in a row and the same level interactions
// (orbs, pads, touch triggers, pickups), at the latest at the next resting point (the icon resting
// on a block, where any run that lands the same way is identical from then on), or the run beats
// the level. "Exact" = back before the next input; "chained" = back later, with the inputs in
// between at their recorded ticks. Frames where the player only survives don't count.
//
//   Survey    play the whole macro once from the start, unbroken: the reference for every
//             comparison (state + interactions every step), and where the resting points are found
//   Per group of inputs sharing a checkpoint (one physics step per frame, like 240 FPS play):
//     Advance   play the unmodified macro to the checkpoint tick, place a GD checkpoint
//     Verify    respawn, replay unmodified: must match the survey every step, else the group's
//               inputs are UNMEASURED (the log names the fields that differ)
//     Trials    per input, move only that input: 1, 2, 3 ... steps earlier until the first failure,
//               then GAP_PROBE more to detect holes; the same later. A trial stops as soon as it
//               reconverges, dies, or passes the next resting point without reconverging.
// Results are saved after every group, so a stopped scan keeps what it found and can continue.
namespace db::scan {
	bool enabled();                 // `dynamic-enabled` setting (off = macro-only, no Dynamic at all)
	// From the popup (game paused). rescanAll = false continues: only inputs without a window (or
	// unmeasured ones) are scanned. False with a notification if impossible.
	bool start(bool rescanAll);
	bool active();
	void abort(char const* why);
	std::string status();           // progress for the overlay; empty when idle

	enum class Interaction : uint8_t { Ring, Pad, Trigger, Pickup };

	// --- called from hooks ---
	bool wantsFastForward();
	void afterVirtualFrame();       // drives the scan between virtual frames (outside GD's update)
	void onStepEnd(PlayLayer* pl, uint32_t tick);         // end of every physics step
	void onInteraction(Interaction kind, GameObject* obj); // orb / pad / touch trigger / pickup
	bool onPlayerDied(PlayLayer* pl, void* object);       // true: swallow the death
	bool onLevelComplete();         // true: swallow the completion
	// Reaching the end wall during a scan: GD's end animation (timed actions ending in levelComplete)
	// must not start, or the leftover animations complete the level after the scan and leave GD stuck
	// in its "completed" state. The PlayLayer hook skips playEndAnimationToPos and counts this as the
	// level being completed instead. The end text/effect/screen are also blocked while a scan runs.
	bool suppressEndUI();

	constexpr int CONFIRM_STEPS = 8; // identical steps that count as reconverged / as resting
	constexpr int GAP_PROBE = 4;     // extra frames tested past the first failure, to detect holes
}
