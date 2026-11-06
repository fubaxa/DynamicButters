#pragma once

#include <cstdint>

class PlayLayer;

// Debug (M8, start positions): logs every change of the player's speed with the player's position
// and hitbox and the nearest speed portal's position and hitbox, so a normal run and a start
// position run can be compared portal by portal. Tests the idea that speed portals trigger on the
// player's hitbox edge rather than its centre, and whether a start position run triggers them at
// the same player x as the real run. Runs while the tick probe setting is on.
namespace db::speedprobe {
	void onAttemptStart(PlayLayer* pl, uint32_t tick);
	void onStepEnd(PlayLayer* pl, uint32_t tick);
}
