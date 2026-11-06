#pragma once

#include <string>

class PlayLayer;

// Dynamic window scan (PLAN.md §10).
//
// For every input of the loaded macro, finds how many physics steps earlier and later it can
// happen with the rest of the macro unchanged and the player still alive shortly after the next
// input. Restarts in practice mode and fast-forwards the macro (like Seek). Shortly before each
// input (never inside a hold) one GD checkpoint is placed; each trial respawns there and plays the
// macro with only that input moved. The scan steps outward from the recorded frame (1, 2, 3 ...
// steps) until the first failure, on each side: pass/fail over frames isn't guaranteed to be one
// connected range, and the range around the recorded frame is what a player experiences. Deaths
// during trials are caught before GD handles them. Results are saved in the macro file and a
// readable report next to it.
namespace db::scan {
	bool enabled();           // `dynamic-enabled` setting (off = macro-only, no Dynamic at all)
	bool start();             // from the popup (game paused); false with a notification if impossible
	bool active();
	void abort(char const* why);
	std::string status();     // progress for the overlay; empty when idle

	// --- called from hooks ---
	bool wantsFastForward();
	void afterVirtualFrame(); // drives the scan between virtual frames (outside GD's update)
	bool onPlayerDied(PlayLayer* pl, void* object); // true: swallow the death (trial failed)
	bool onLevelComplete();   // true: swallow the completion (trial passed)

	constexpr unsigned MARGIN_TICKS = 120; // a trial must survive this long past the next input (0.25 s)
}
