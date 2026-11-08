#pragma once

#include <cstdint>
#include <string>

class PlayLayer;
class GJBaseGameLayer;

// Dynamic click assist (M9, macro mode "Dynamic").
//
// The player plays; the macro only provides the click pattern and each click's scanned window
// (WindowScan). The macro is reduced to the moments the player's hold state changes (press /
// release; in 1-player levels both jump keys count as one button). Assuming the player's click
// pattern matches the macro, each player press/release belongs to the next such moment:
//   - inside its window                       -> goes through unchanged
//   - up to `dynamic-leniency` steps too early -> held, applied on the first step of the window
//   - earlier, late, no window (ship part, unscanned, unmeasured) -> goes through unchanged
// Only valid at 240 FPS and 1x speed with Click Between Steps off (the windows were measured so);
// the assist switches itself off when the speedhack isn't 1x.
namespace db::dynamic {
	bool active();                                       // Dynamic mode with a macro loaded, assist allowed
	void onAttemptStart(PlayLayer* pl, uint32_t tick);   // after resetLevel (fresh attempt or respawn)
	// A real player input (not the macro's, not the assist's own). True = consumed (held for later).
	bool onPlayerInput(GJBaseGameLayer* gl, bool down, int button, bool isPlayer1, uint32_t tick);
	void dispatchDue(GJBaseGameLayer* gl, uint32_t tick); // right after processQueuedButtons
	bool isDispatching();
	std::string status();                                // overlay text; empty when not active
}
