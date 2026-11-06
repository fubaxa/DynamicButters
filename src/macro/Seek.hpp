#pragma once

#include <cstdint>

class PlayLayer;

// Seek to a macro start position (M8, PLAN.md §6 "M8 design").
//
// Restarts the level in practice mode and plays the macro in fast-forward: each real frame runs
// many virtual 240-FPS frames (the whole game update, one physics step each, like the 240 FPS the
// macros are recorded at; M8a showed the step grouping doesn't change the run anyway). At each
// start position up to the target a GD checkpoint is placed from the end of a frame (where the
// checkpoint test showed they're exact). At the target the player respawns at that checkpoint, so
// the first start is the same as every later respawn. Then playback continues (Playback), the
// recording continues from there (Record), or the player plays (Off).
namespace db::seek {
	// Starts a seek to `tick` (moved back to before a hold if needed). Call while paused; the pause
	// menu is closed. False (with a notification) if there's no macro or the tick is unusable.
	bool start(uint32_t tick);
	bool active();

	// --- called from hooks ---
	bool wantsFastForward();             // CCScheduler::update: run virtual frames instead
	void afterVirtualFrame();            // after each virtual frame (stall detection)
	void onFrameEnd(PlayLayer* pl);      // PlayLayer::postUpdate: place due checkpoints
	void onPlayerDied();                 // a death during a seek: the macro doesn't fit, stop
	void abort(char const* why);         // level complete, quit, ...

	constexpr float VIRTUAL_FRAME_DT = 1.f / 240.f;
	constexpr int BUDGET_MS = 14;        // real time spent on virtual frames per rendered frame
}
