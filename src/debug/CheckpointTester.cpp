#include "debug/CheckpointTester.hpp"

#include <Geode/Geode.hpp>

#include <map>

#include "debug/Settings.hpp"
#include "macro/MacroManager.hpp"
#include "physics/PlayerState.hpp"

using namespace geode::prelude;

namespace db::cptest {
	namespace {
		constexpr uint32_t FIRST_AT = 1200; // 2.5 s into the level (ticks are 480/s)
		constexpr uint32_t SPACING = 1200;  // a test every 2.5 s
		constexpr uint32_t WINDOW = 240;    // compare 0.5 s (120 steps) after each respawn

		enum class Phase { Idle, Reference, AwaitRespawn, Verify };

		struct State {
			Phase phase = Phase::Idle;
			uint32_t target = FIRST_AT;
			uint32_t testTick = 0;
			int testNo = 0;
			std::map<uint32_t, PlayerState> ref; // original run, end-of-step state per tick

			// current verification
			PlayerState::Diff atRespawn;
			PlayerState::Diff window;
			uint32_t firstDivergence = 0; // ticks after the checkpoint, 0 = none
			uint32_t compared = 0;

			// totals
			int exactTests = 0;
			PlayerState::Diff worstRespawn;
			PlayerState::Diff worstWindow;
		} s;

		bool active(PlayLayer* pl) {
			return settings::checkpointTest() && MacroManager::get().isPlaying() && pl->m_isPracticeMode;
		}

		void finishTest() {
			bool exact = s.atRespawn.exact() && s.window.exact();
			if (exact) s.exactTests++;
			s.worstRespawn.maxWith(s.atRespawn);
			s.worstWindow.maxWith(s.window);
			log::info("cptest #{} at tick {} [{}]: respawn {} | next {} steps max {} | first divergence {}{}",
				s.testNo, s.testTick, "GD restore",
				s.atRespawn.str(), s.compared, s.window.str(),
				s.firstDivergence ? fmt::format("+{} ticks", s.firstDivergence) : std::string("none"),
				exact ? "  => EXACT" : "  => DIFFERS");
			s.phase = Phase::Idle;
			s.target = s.testTick + SPACING;
			s.ref.clear();
		}
	}

	void onLevelStart() {
		s = State{};
	}

	void onFrameEnd(PlayLayer* pl) {
		if (!active(pl)) return;
		uint32_t tick = pl->m_gameState.m_currentProgress;

		switch (s.phase) {
			case Phase::Idle: {
				if (tick < s.target || MacroManager::get().isMacroHolding()) return;
				s.testNo++;
				s.testTick = tick;
				s.ref.clear();
				s.ref[tick] = PlayerState::capture(pl->m_player1);
				s.atRespawn = {};
				s.window = {};
				s.firstDivergence = 0;
				s.compared = 0;
				pl->markCheckpoint(); // our createCheckpoint hook saves the precise state
				s.phase = Phase::Reference;
				log::info("cptest #{}: checkpoint placed at tick {}", s.testNo, tick);
				break;
			}
			case Phase::Reference: {
				if (tick < s.testTick + WINDOW) return;
				s.phase = Phase::AwaitRespawn;
				// Respawn outside of the update loop, like pressing the respawn key would.
				Loader::get()->queueInMainThread([] {
					if (auto pl = PlayLayer::get(); pl && s.phase == Phase::AwaitRespawn) {
						pl->resetLevel();
					}
				});
				break;
			}
			default: break;
		}
	}

	void onStepEnd(PlayLayer* pl) {
		if (!active(pl)) return;
		uint32_t tick = pl->m_gameState.m_currentProgress;

		if (s.phase == Phase::Reference) {
			s.ref[tick] = PlayerState::capture(pl->m_player1);
			return;
		}
		if (s.phase != Phase::Verify) return;

		auto it = s.ref.find(tick);
		if (it != s.ref.end()) {
			auto d = PlayerState::diff(PlayerState::capture(pl->m_player1), it->second);
			if (!d.exact() && s.firstDivergence == 0) {
				s.firstDivergence = tick - s.testTick;
				log::info("cptest #{}: first divergence at tick {} (+{}): {}", s.testNo, tick, s.firstDivergence, d.str());
			}
			s.window.maxWith(d);
			s.compared++;
		}
		if (tick >= s.testTick + WINDOW) finishTest();
	}

	void onAttemptStart(PlayLayer* pl, uint32_t tick) {
		if (!active(pl)) return;
		if (s.phase != Phase::AwaitRespawn) {
			if (tick == 0) s = State{}; // full restart: start the schedule over
			return;
		}
		if (tick != s.testTick) {
			log::warn("cptest #{}: respawned at tick {} instead of {}, test skipped", s.testNo, tick, s.testTick);
			s.phase = Phase::Idle;
			s.target = tick + SPACING;
			return;
		}
		s.atRespawn = PlayerState::diff(PlayerState::capture(pl->m_player1), s.ref[tick]);
		s.phase = Phase::Verify;
	}

	void logSummary(char const* reason) {
		if (s.testNo == 0) return;
		log::info("cptest summary ({}): {} tests, {} exact [{}]; worst at respawn {} | worst in window {}",
			reason, s.testNo, s.exactTests, "GD restore",
			s.worstRespawn.str(), s.worstWindow.str());
	}
}
