#pragma once

#include <cstdint>
#include <string>

// Safe mode (PLAN.md §7.5, M7): an attempt that used playback or a speedhack other than 1.0x
// (later also Dynamic and seek snapshots) is "unsafe". With the `safe-mode` setting on (default),
// GD is put in test mode while it would save progress from an unsafe attempt, so no new best %,
// completion, orbs/stars/coins or leaderboard submission. Test mode is GD's own no-saving state
// (start positions use it).
namespace db::safe {
	bool enabled();                      // `safe-mode` setting
	float speedhackFactor();             // speed the game runs at in a level (1.0 when the speedhack is off)

	void onAttemptStart(uint32_t tick);  // after resetLevel; a fresh attempt (tick 0) starts clean
	void check();                        // marks the attempt unsafe if anything unsafe is active right now
	void markUnsafe(std::string reason); // for other features (Dynamic, seek snapshots)

	bool unsafe();                       // this attempt used something unsafe
	std::string const& reason();         // first reason, e.g. "playback" or "speedhack 0.50x"
	bool blocking();                     // enabled() && unsafe(): progress won't be saved
}
