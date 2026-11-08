#pragma once

#include <cstdint>

class PlayLayer;
class PlayerObject;
class GameObject;

// Physics trace (setting `physics-trace`, off by default): writes the player state at the end of
// every physics step, plus inputs, interactions, deaths and completions, to a CSV file per level
// session in `<save dir>/traces/`. Used as ground truth for a separate GD physics simulator.
//
// Logging only: nothing here changes the game. While a Dynamic scan or a seek runs, nothing is
// written (those replay the level many times in fast-forward).
//
// File layout (one record per line, first column = record type):
//   # ...                                  comments / header (level, start settings, legacy flags)
//   A,attempt,tick,practice                attempt start (also after every practice respawn)
//   S,tick,step,x,y,xVel,yVel,rot,...      end of a physics step (columns listed in the header)
//   I,tick,down,button,p2key,macro,x,y,yVel  input applied by GD (state right before it)
//   O,tick,kind,objectID,ox,oy,rx,ry,rw,rh   interaction: orb / pad / trigger / pickup + its hitbox
//   D,tick,objectID,ox,oy,rx,ry,rw,rh,dead   destroyPlayer (dead = the player actually died)
//   C,tick                                 level complete
// Floats are written with 9 significant digits and doubles with 17, so values round-trip exactly.
namespace db::trace {
	bool enabled();

	// --- called from hooks ---
	void onAttemptStart(PlayLayer* pl, uint32_t tick, bool practice);
	void onStepEnd(PlayLayer* pl, uint32_t tick);
	void onInput(PlayLayer* pl, uint32_t tick, bool down, int button, bool p2key, bool fromMacro, PlayerObject* p);
	void onInteraction(char const* kind, GameObject* obj);
	void onDestroyPlayer(PlayLayer* pl, PlayerObject* player, GameObject* obj);
	void onLevelComplete(uint32_t tick);
	void onQuit();
}
