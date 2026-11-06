#pragma once

#include <cstdint>

class PlayLayer;

// Automatic checkpoint precision test (debug setting "checkpoint-test").
// Runs during macro Playback in practice mode, so the run before and after a respawn gets
// exactly the same inputs:
//   1. at a tick where the macro isn't holding a button, place a checkpoint (markCheckpoint)
//   2. record the original run's end-of-step player state for WINDOW ticks
//   3. force a respawn (resetLevel) at that checkpoint
//   4. compare the replay against the original, tick by tick, and log the result
//   5. continue, and repeat every SPACING ticks
namespace db::cptest {
	void onFrameEnd(PlayLayer* pl);                  // PlayLayer::postUpdate
	void onStepEnd(PlayLayer* pl);                   // end of every processCommands
	void onAttemptStart(PlayLayer* pl, uint32_t tick); // end of resetLevel (after precise restore)
	void onLevelStart();                             // PlayLayer::init
	void logSummary(char const* reason);             // level complete / quit
}
