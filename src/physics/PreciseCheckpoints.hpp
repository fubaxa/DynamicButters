#pragma once

#include <cstdint>

class CheckpointObject;
class PlayLayer;

// Precise practice checkpoints (PLAN.md §5).
// Each checkpoint also stores an exact end-of-step PlayerState. After GD finishes a respawn,
// the difference between GD's restored state and ours is logged (diagnostic only; GD's state is kept).
namespace db::checkpoints {
	void onCreated(PlayLayer* pl, CheckpointObject* cp); // PlayLayer::createCheckpoint
	void onStepEnd(PlayLayer* pl);                       // end of every processCommands
	void onLoad(CheckpointObject* cp);                   // PlayLayer::loadFromCheckpoint
	void afterReset(PlayLayer* pl, uint32_t tick);       // end of PlayLayer::resetLevel
}
