#include "macro/Seek.hpp"

#include <Geode/Geode.hpp>

#include <algorithm>
#include <chrono>
#include <vector>

#include "macro/MacroManager.hpp"
#include "safety/SafeMode.hpp"

using namespace geode::prelude;

namespace db::seek {
	namespace {
		using Clock = std::chrono::steady_clock;

		struct State {
			bool pending = false;  // start queued, waiting for the next frame
			bool active = false;
			std::vector<uint32_t> targets; // checkpoint ticks, sorted; the last one is where we start
			size_t next = 0;
			Clock::time_point started;
			uint32_t lastTick = 0;
			unsigned stalledFrames = 0;
			unsigned virtualFrames = 0;
			bool muted = false;
			float savedVolume = 1.f;
		} s;

		FMOD::ChannelGroup* masterGroup() {
			auto engine = FMODAudioEngine::sharedEngine();
			if (!engine || !engine->m_system) return nullptr;
			FMOD::ChannelGroup* master = nullptr;
			if (engine->m_system->getMasterChannelGroup(&master) != FMOD_OK) return nullptr;
			return master;
		}

		void mute(bool on) {
			if (on == s.muted) return;
			auto master = masterGroup();
			if (!master) return;
			if (on) {
				master->getVolume(&s.savedVolume);
				master->setVolume(0.f);
			} else {
				master->setVolume(s.savedVolume);
			}
			s.muted = on;
		}

		void stop() {
			s.active = false;
			s.pending = false;
			mute(false);
		}

		void finish(PlayLayer* pl, uint32_t tick) {
			auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - s.started).count();
			stop();
			MacroManager::get().endSeek(true, tick);
			safe::markUnsafe("macro start position");
			log::info("seek: reached tick {} ({:.2f} s) in {} ms, {} virtual frames ({:.0f} ticks/s); respawning there",
				tick, tick / 480.0, ms, s.virtualFrames, ms > 0 ? tick * 1000.0 / ms : 0.0);
			// Respawn at the checkpoint just placed, outside the update loop like the respawn key,
			// so this start is the same as every later respawn.
			Loader::get()->queueInMainThread([] {
				if (auto pl = PlayLayer::get()) pl->resetLevel();
			});
		}
	}

	bool active() { return s.active; }

	bool start(uint32_t tick) {
		auto pl = PlayLayer::get();
		if (!pl || s.active || s.pending) return false;
		auto& mgr = MacroManager::get();
		if (!mgr.ensurePlayMacro()) {
			Notification::create("No macro for this level to start from", NotificationIcon::Error)->show();
			return false;
		}
		auto const& macro = *mgr.playMacro();

		uint32_t target = macro.latestFreeTick(tick);
		if (target != tick) {
			log::info("seek: tick {} is inside a hold, using tick {} (before the press)", tick, target);
		}
		if (target == 0) {
			Notification::create("That start position is at the very start", NotificationIcon::Warning)->show();
			return false;
		}

		// Every saved start position on the way gets a checkpoint too, so earlier ones are one
		// "remove checkpoint" away; the last checkpoint is the target.
		s = State{};
		for (auto t : macro.startTicks) {
			if (t < target) s.targets.push_back(macro.latestFreeTick(t));
		}
		s.targets.push_back(target);
		std::sort(s.targets.begin(), s.targets.end());
		s.targets.erase(std::unique(s.targets.begin(), s.targets.end()), s.targets.end());
		std::erase(s.targets, 0u);
		s.pending = true;
		log::info("seek: to tick {} ({:.2f} s), {} checkpoints, macro {} ({} inputs)",
			target, target / 480.0, s.targets.size(), mgr.playSource(), macro.inputs.size());

		// Next frame, outside the popup's touch handler: close the pause menu, switch to practice
		// mode, restart from the very start and fast-forward.
		Loader::get()->queueInMainThread([] {
			auto pl = PlayLayer::get();
			if (!pl || !s.pending) { s.pending = false; return; }
			if (auto scene = CCDirector::get()->getRunningScene()) {
				if (auto pause = scene->getChildByType<PauseLayer>(0)) pause->onResume(nullptr);
			}
			if (!pl->m_isPracticeMode) pl->togglePracticeMode(true);
			MacroManager::get().beginSeek();
			s.pending = false;
			s.active = true;
			s.started = Clock::now();
			mute(true);
			pl->removeAllCheckpoints();
			pl->resetLevelFromStart();
		});
		return true;
	}

	bool wantsFastForward() {
		return s.active && PlayLayer::get();
	}

	void afterVirtualFrame() {
		if (!s.active) return;
		s.virtualFrames++;
		auto pl = PlayLayer::get();
		if (!pl) { abort("left the level"); return; }
		uint32_t tick = pl->m_gameState.m_currentProgress;
		if (tick == s.lastTick) {
			// Paused, or GD isn't advancing for another reason: don't spin forever.
			if (++s.stalledFrames >= 2400) abort("the level stopped advancing");
		} else {
			s.stalledFrames = 0;
			s.lastTick = tick;
		}
	}

	void onFrameEnd(PlayLayer* pl) {
		if (!s.active || !pl) return;
		uint32_t tick = pl->m_gameState.m_currentProgress;
		while (s.next < s.targets.size() && tick >= s.targets[s.next]) {
			if (tick != s.targets[s.next]) {
				log::warn("seek: start position {} placed at tick {} (frame ran past it)", s.targets[s.next], tick);
			}
			pl->markCheckpoint();
			log::info("seek: checkpoint {} of {} at tick {}", s.next + 1, s.targets.size(), tick);
			s.next++;
		}
		if (s.next == s.targets.size()) finish(pl, tick);
	}

	void onPlayerDied() {
		if (!s.active) return;
		auto pl = PlayLayer::get();
		uint32_t tick = pl ? pl->m_gameState.m_currentProgress : 0;
		Notification::create("Seek failed: the macro died on the way", NotificationIcon::Error)->show();
		abort(fmt::format("player died at tick {}", tick).c_str());
	}

	void abort(char const* why) {
		if (!s.active && !s.pending) return;
		log::warn("seek: stopped ({}), {} of {} checkpoints placed", why, s.next, s.targets.size());
		stop();
		MacroManager::get().endSeek(false, 0);
	}
}
