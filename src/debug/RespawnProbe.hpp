#pragma once

#include <Geode/Geode.hpp>

// M1 diagnostic: is the player's state after respawning from a checkpoint identical every time?
// After each respawn we log the state at the respawn tick and again PROBE_DELAY ticks later.
// Compare those lines across respawns from the same checkpoint (and against createCheckpoint).

namespace db::debug {
	constexpr unsigned PROBE_DELAY = 120; // in m_currentProgress units = 60 physics steps

	inline unsigned respawnTick = 0;
	inline unsigned probeAtTick = 0;   // 0 = no probe pending

	inline void logPlayer(char const* what, unsigned tick, PlayerObject* p) {
		if (!p) return;
		geode::log::info("{} tick={} x={:.6f} y={:.6f} yVel={:.6f} rot={:.4f}",
			what, tick, p->m_position.x, p->m_position.y, p->m_yVelocity, p->getRotation());
	}
}
