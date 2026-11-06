#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>

#include "debug/Settings.hpp"
#include "debug/RespawnProbe.hpp"
#include "macro/MacroManager.hpp"

using namespace geode::prelude;

// M1: answers "is m_currentProgress exactly one physics tick per processCommands call?"
// and logs every input GD actually applies, with the tick and player state.
//
// The probe prints a summary once per 240 processCommands calls (about 1 s of gameplay):
//   calls        processCommands calls in the window
//   advanced     how far m_currentProgress moved over the window
//   inside       how often the tick changed *during* the original call (vs between calls)
//   gaps         calls where the tick moved by something other than 0 or 1 since the last call
//   halfTicks    calls with isHalfTick = true
// If advanced == calls and gaps == 0, the tick counter is safe to use as the macro frame.

class $modify(DBBaseGameLayer, GJBaseGameLayer) {
	struct Fields {
		bool inProcessCommands = false;
		bool hasLast = false;
		unsigned lastTickAtCall = 0;

		unsigned windowCalls = 0;
		unsigned windowStartTick = 0;
		unsigned windowInside = 0;
		unsigned windowGaps = 0;
		unsigned windowHalfTicks = 0;
	};

	bool isPlayLayer() {
		return PlayLayer::get() == static_cast<GJBaseGameLayer*>(this);
	}

	// GD can turn m_clickBetweenSteps back on mid-level (e.g. toggling the option from the
	// pause menu), so forcing it once at init is not enough. update() decides whether a step
	// gets split (it passes isHalfTick to processCommands), so the flag is enforced here,
	// before every frame's physics.
	void update(float dt) {
		if (isPlayLayer() && db::MacroManager::get().shouldForceCbsOff() && m_clickBetweenSteps) {
			m_clickBetweenSteps = false;
			log::info("force-cbs-off: CBS was on at tick {}, forced off", m_gameState.m_currentProgress);
		}
		GJBaseGameLayer::update(dt);
	}

	void processCommands(float dt, bool isHalfTick, bool isLastTick) {
		if (isHalfTick && isPlayLayer() && db::MacroManager::get().shouldForceCbsOff()) {
			// Should never happen once update() enforces the flag; logged so the test shows it.
			log::warn("force-cbs-off: half-step at tick {} despite CBS being forced off",
				m_gameState.m_currentProgress);
		}
		if (!isPlayLayer() || !db::settings::tickProbe()) {
			auto f = m_fields.self();
			f->inProcessCommands = true;
			GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
			f->inProcessCommands = false;
			return;
		}

		auto f = m_fields.self();
		unsigned before = m_gameState.m_currentProgress;

		// Tick movement since the previous call (detects resets, checkpoint loads, skips).
		if (f->hasLast) {
			unsigned sinceLast = before - f->lastTickAtCall;
			if (before < f->lastTickAtCall) {
				log::debug("probe: tick went back {} -> {} (reset/checkpoint)", f->lastTickAtCall, before);
				f->windowCalls = 0;
			} else if (sinceLast > 1) {
				f->windowGaps++;
				log::warn("probe: tick jumped by {} between calls ({} -> {})", sinceLast, f->lastTickAtCall, before);
			}
		}
		if (f->windowCalls == 0) {
			f->windowStartTick = before;
			f->windowInside = f->windowGaps = f->windowHalfTicks = 0;
		}

		f->inProcessCommands = true;
		GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
		f->inProcessCommands = false;

		unsigned after = m_gameState.m_currentProgress;
		if (after != before) f->windowInside++;

		if (db::debug::probeAtTick != 0 && after >= db::debug::probeAtTick) {
			db::debug::logPlayer(
				fmt::format("respawn: +{} after tick {}", after - db::debug::respawnTick, db::debug::respawnTick).c_str(),
				after, m_player1);
			db::debug::probeAtTick = 0;
		}
		if (isHalfTick) {
			f->windowHalfTicks++;
			// Logged individually so half-steps can be lined up with the input lines.
			log::info("probe: HALF step tick {} -> {} (cbs={} betweenSteps={} lastTick={})",
				before, after, m_clickBetweenSteps, m_isBetweenSteps, isLastTick);
		}

		f->hasLast = true;
		f->lastTickAtCall = after;
		f->windowCalls++;

		if (f->windowCalls >= 240) {
			log::info(
				"probe: calls={} advanced={} inside={} gaps={} halfTicks={} (tick {}..{})",
				f->windowCalls, after - f->windowStartTick, f->windowInside,
				f->windowGaps, f->windowHalfTicks, f->windowStartTick, after
			);
			f->windowCalls = 0;
		}
	}

	void handleButton(bool down, int button, bool isPlayer1) {
		if (!isPlayLayer()) {
			GJBaseGameLayer::handleButton(down, button, isPlayer1);
			return;
		}

		// P1/P2 is the *key* that was pressed (e.g. up arrow = P2 key). In a 1-player
		// level both keys control player 1, so snapshot/log the player that actually moves.
		bool twoPlayer = m_levelSettings && m_levelSettings->m_twoPlayerMode;
		auto p = (isPlayer1 || !twoPlayer) ? m_player1 : m_player2;

		// Record before calling the original, so the snapshot is the state the input was applied to.
		db::MacroManager::get().onInput(m_gameState.m_currentProgress, down, button, !isPlayer1, p);

		if (db::settings::logInputs()) {
			log::info(
				"input: {} btn={} key={} tick={} inStep={} betweenSteps={} x={:.4f} y={:.4f} yVel={:.6f}",
				down ? "PRESS  " : "RELEASE", button, isPlayer1 ? "P1" : "P2",
				m_gameState.m_currentProgress, m_fields->inProcessCommands, m_isBetweenSteps,
				p ? p->m_position.x : 0.f, p ? p->m_position.y : 0.f, p ? p->m_yVelocity : 0.0
			);
		}
		GJBaseGameLayer::handleButton(down, button, isPlayer1);
	}
};
