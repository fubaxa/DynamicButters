#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>

#include <algorithm>

#include "debug/Settings.hpp"
#include "debug/RespawnProbe.hpp"
#include "macro/MacroManager.hpp"
#include "debug/CheckpointTester.hpp"
#include "debug/SpeedProbe.hpp"
#include "physics/PlayerState.hpp"
#include "physics/PreciseCheckpoints.hpp"

using namespace geode::prelude;

// Per-step hooks: CBS enforcement, tick probe, input recording and macro playback.
//
// Tick probe summary (once per 240 processCommands calls, about 1 s of gameplay):
//   calls      processCommands calls in the window
//   advanced   how far m_currentProgress moved (expect 2 x calls: +2 per step)
//   inside     how often the tick changed *during* the original call
//   gaps       calls where the tick jumped by more than one step since the last call
//   halfTicks  calls with isHalfTick = true (only happens with Click Between Steps on)
//
// Playback timing: while recording, the player's input reached handleButton from inside the
// step, at a moment when m_currentProgress already held that step's tick. Macro inputs are
// applied at the same point: right after GD's processQueuedButtons, when tick == input frame.
// If processQueuedButtons doesn't run in a step, the fallback after processCommands applies them
// and counts them as "late" (shows up in the playback summary).

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

		// Frame grouping (M8a): how many steps each rendered frame ran, and how many steps GD
		// flagged as the frame's last. Logged with the probe summary.
		unsigned stepsThisFrame = 0;
		unsigned frameHist[6] = {}; // 0, 1, 2, 3, 4, 5+ steps in a frame
		unsigned windowLastTicks = 0;
	};

	bool isPlayLayer() {
		return PlayLayer::get() == static_cast<GJBaseGameLayer*>(this);
	}

	// GD can turn m_clickBetweenSteps back on mid-level (e.g. toggling the option from the
	// pause menu), so it's enforced before every frame's physics. update() decides whether a
	// step gets split (it passes isHalfTick to processCommands).
	void update(float dt) {
		if (isPlayLayer() && db::MacroManager::get().shouldForceCbsOff() && m_clickBetweenSteps) {
			m_clickBetweenSteps = false;
			log::info("force-cbs-off: CBS was on at tick {}, forced off", m_gameState.m_currentProgress);
		}
		auto f = m_fields.self();
		f->stepsThisFrame = 0;
		db::runtime::stepInFrame = 0;
		GJBaseGameLayer::update(dt);
		if (isPlayLayer()) f->frameHist[std::min(f->stepsThisFrame, 5u)]++;
	}

	void processQueuedButtons(float dt, bool clearInputQueue) {
		GJBaseGameLayer::processQueuedButtons(dt, clearInputQueue);
		if (isPlayLayer()) {
			db::MacroManager::get().dispatchDue(this, m_gameState.m_currentProgress, false);
		}
	}

	void processCommands(float dt, bool isHalfTick, bool isLastTick) {
		if (!isPlayLayer()) {
			GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
			return;
		}
		if (isHalfTick && db::MacroManager::get().shouldForceCbsOff()) {
			// Should never happen once update() enforces the flag.
			log::warn("force-cbs-off: half-step at tick {} despite CBS being forced off",
				m_gameState.m_currentProgress);
		}

		auto f = m_fields.self();
		bool probe = db::settings::tickProbe();
		unsigned before = m_gameState.m_currentProgress;

		if (probe) {
			// Tick movement since the previous call (detects resets, checkpoint loads, skips).
			if (f->hasLast) {
				unsigned sinceLast = before - f->lastTickAtCall;
				if (before < f->lastTickAtCall) {
					log::debug("probe: tick went back {} -> {} (reset/checkpoint)", f->lastTickAtCall, before);
					f->windowCalls = 0;
				} else if (sinceLast > 2) {
					f->windowGaps++;
					log::warn("probe: tick jumped by {} between calls ({} -> {})", sinceLast, f->lastTickAtCall, before);
				}
			}
			if (f->windowCalls == 0) {
				f->windowStartTick = before;
				f->windowInside = f->windowGaps = f->windowHalfTicks = 0;
			}
		}

		f->stepsThisFrame++;
		db::runtime::stepInFrame = f->stepsThisFrame;
		if (isLastTick) f->windowLastTicks++;

		f->inProcessCommands = true;
		db::runtime::inStep = true;
		GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
		db::runtime::inStep = false;
		f->inProcessCommands = false;

		unsigned after = m_gameState.m_currentProgress;

		// Fallback: anything still due here missed processQueuedButtons this step.
		db::MacroManager::get().dispatchDue(this, after, true);

		auto pl = static_cast<PlayLayer*>(static_cast<GJBaseGameLayer*>(this));
		db::MacroManager::get().onStepEnd(after, m_player1);
		db::checkpoints::onStepEnd(pl);
		db::speedprobe::onStepEnd(pl, after);
		db::cptest::onStepEnd(pl);

		if (db::debug::probeAtTick != 0 && after >= db::debug::probeAtTick) {
			db::debug::logPlayer(
				fmt::format("respawn: +{} after tick {}", after - db::debug::respawnTick, db::debug::respawnTick).c_str(),
				after, m_player1);
			db::debug::probeAtTick = 0;
		}

		if (!probe) return;
		if (after != before) f->windowInside++;
		if (isHalfTick) {
			f->windowHalfTicks++;
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
			auto& h = f->frameHist;
			log::info("probe: frames by steps 0:{} 1:{} 2:{} 3:{} 4:{} 5+:{} | lastTick={} of {} steps",
				h[0], h[1], h[2], h[3], h[4], h[5], f->windowLastTicks, f->windowCalls);
			for (auto& n : h) n = 0;
			f->windowLastTicks = 0;
			f->windowCalls = 0;
		}
	}

	void handleButton(bool down, int button, bool isPlayer1) {
		if (!isPlayLayer()) {
			GJBaseGameLayer::handleButton(down, button, isPlayer1);
			return;
		}

		auto& mgr = db::MacroManager::get();
		bool fromMacro = mgr.isDispatching();

		// During playback only the macro plays: drop the player's own inputs.
		if (!fromMacro && mgr.shouldBlockExternalInput(down, !isPlayer1)) {
			if (db::settings::logInputs()) {
				log::info("input: blocked {} key={} tick={} (playback)",
					down ? "press" : "release", isPlayer1 ? "P1" : "P2", m_gameState.m_currentProgress);
			}
			return;
		}

		// P1/P2 is the *key* that was pressed (e.g. up arrow = P2 key). In a 1-player
		// level both keys control player 1, so snapshot/log the player that actually moves.
		bool twoPlayer = m_levelSettings && m_levelSettings->m_twoPlayerMode;
		auto p = (isPlayer1 || !twoPlayer) ? m_player1 : m_player2;

		// Record before calling the original, so the snapshot is the state the input was applied to.
		mgr.onInput(m_gameState.m_currentProgress, down, button, !isPlayer1, p);

		if (db::settings::logInputs() && !fromMacro) {
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
