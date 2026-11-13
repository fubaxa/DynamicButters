#pragma once

class PlayLayer;

// SmartStartPos (setting `smart-startpos`): before a start position spawn, its settings are set
// from the gameplay portals the player would have passed to get there: the last speed, gamemode,
// size, dual and mirror portal before it (gravity portals are ignored), or the level's own start
// setting when there is no such portal before it. Every gameplay portal is
// trusted to affect the player, except speed portals with preview turned off (unused ones). A
// portal whose hitbox the start position is inside counts as passed, even if its centre is ahead.
namespace db::smartstartpos {
	void apply(PlayLayer* pl); // PlayLayer::resetLevel, before the original
}
