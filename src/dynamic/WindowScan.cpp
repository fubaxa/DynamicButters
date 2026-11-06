#include "dynamic/WindowScan.hpp"

#include <Geode/Geode.hpp>
#include <Geode/utils/file.hpp>

#include <algorithm>
#include <chrono>
#include <vector>

#include "macro/MacroManager.hpp"
#include "macro/MacroStore.hpp"
#include "physics/PlayerState.hpp"

using namespace geode::prelude;

namespace db::scan {
	namespace {
		using Clock = std::chrono::steady_clock;

		// Inputs that share the checkpoint placed at `checkpoint`.
		struct Job {
			uint32_t checkpoint = 0;
			std::vector<size_t> inputs;
		};

		enum class Phase { Advance, Trial };
		enum class Side { Baseline, Early, Late };

		struct State {
			bool pending = false;
			bool active = false;
			Phase phase = Phase::Advance;

			std::vector<MacroInput> base;   // the macro's inputs, unmodified
			std::vector<MacroInput> result; // base + windows
			std::vector<Job> jobs;
			size_t job = 0;
			size_t jobInput = 0;
			uint32_t checkpointTick = 0;    // where the current checkpoint actually is
			int range = 20;                 // steps per side

			// current input / trial
			size_t idx = 0;
			Side side = Side::Baseline;
			int k = 0;
			uint32_t trialFrame = 0;
			uint32_t horizon = 0;
			bool trialDead = false;
			bool trialCompleted = false;

			// progress
			size_t inputsDone = 0;
			uint64_t trials = 0;
			uint64_t ticksSimulated = 0;
			uint32_t lastTick = 0;
			unsigned stalledFrames = 0;
			Clock::time_point started;

			bool muted = false;
			float savedVolume = 1.f;
		} s;

		void mute(bool on) {
			if (on == s.muted) return;
			auto engine = FMODAudioEngine::sharedEngine();
			if (!engine || !engine->m_system) return;
			FMOD::ChannelGroup* master = nullptr;
			if (engine->m_system->getMasterChannelGroup(&master) != FMOD_OK || !master) return;
			if (on) {
				master->getVolume(&s.savedVolume);
				master->setVolume(0.f);
			} else {
				master->setVolume(s.savedVolume);
			}
			s.muted = on;
		}

		uint32_t prevFrame(size_t i) { return i > 0 ? s.base[i - 1].frame : 0; }
		uint32_t nextFrame(size_t i) {
			return i + 1 < s.base.size() ? s.base[i + 1].frame : s.base[i].frame + 480;
		}

		// Earliest frame a trial may move input i to: after the previous input and after the checkpoint.
		uint32_t minFrame(size_t i) {
			return std::max(prevFrame(i) + 2, s.checkpointTick + 2);
		}

		void buildJobs() {
			Macro tmp;
			tmp.inputs = s.base;
			std::vector<std::pair<uint32_t, size_t>> cps;
			for (size_t i = 0; i < s.base.size(); ++i) {
				uint32_t p = s.base[i].frame;
				uint32_t earliest = std::max<int64_t>(int64_t(prevFrame(i)) + 2, int64_t(p) - 2 * s.range) ;
				if (earliest > p) earliest = p;
				uint32_t cp = earliest >= 2 ? tmp.latestFreeTick(earliest - 2) : 0;
				cps.push_back({cp, i});
			}
			std::stable_sort(cps.begin(), cps.end());
			for (auto const& [cp, i] : cps) {
				if (s.jobs.empty() || s.jobs.back().checkpoint != cp) s.jobs.push_back({cp, {}});
				s.jobs.back().inputs.push_back(i);
			}
		}

		std::vector<MacroInput> movedInputs(size_t i, uint32_t frame) {
			auto in = s.base;
			in[i].frame = frame;
			std::stable_sort(in.begin(), in.end(),
				[](MacroInput const& a, MacroInput const& b) { return a.frame < b.frame; });
			return in;
		}

		void launchTrial(PlayLayer* pl, uint32_t frame) {
			s.trialFrame = frame;
			s.horizon = std::max(nextFrame(s.idx), std::max(frame, s.base[s.idx].frame)) + MARGIN_TICKS;
			s.trialDead = false;
			s.trialCompleted = false;
			s.phase = Phase::Trial;
			s.trials++;
			MacroManager::get().setTrialInputs(movedInputs(s.idx, frame));
			pl->resetLevel(); // respawn at the checkpoint, like the respawn key
		}

		void finishInput(PlayLayer* pl);
		void finishJob(PlayLayer* pl);

		void startInput(PlayLayer* pl) {
			s.idx = s.jobs[s.job].inputs[s.jobInput];
			s.side = Side::Baseline;
			s.k = 0;
			s.result[s.idx].window = {};
			launchTrial(pl, s.base[s.idx].frame);
		}

		// Next trial for the current input, or finish it.
		void nextCandidate(PlayLayer* pl) {
			auto& w = s.result[s.idx].window;
			uint32_t p = s.base[s.idx].frame;
			if (s.side == Side::Early) {
				s.k++;
				if (s.k > s.range) {
					w.flags |= InputWindow::EarlyMax;
				} else if (int64_t(p) - 2 * s.k < int64_t(minFrame(s.idx))) {
					w.flags |= InputWindow::EarlyBound;
				} else {
					launchTrial(pl, p - 2 * s.k);
					return;
				}
				s.side = Side::Late;
				s.k = 0;
			}
			// Late side
			s.k++;
			if (s.k > s.range) {
				w.flags |= InputWindow::LateMax;
			} else if (p + 2 * s.k > nextFrame(s.idx) - 2 || p + 2 * s.k >= nextFrame(s.idx)) {
				w.flags |= InputWindow::LateBound;
			} else {
				launchTrial(pl, p + 2 * s.k);
				return;
			}
			finishInput(pl);
		}

		void onTrialResult(PlayLayer* pl, bool pass) {
			auto& w = s.result[s.idx].window;
			switch (s.side) {
				case Side::Baseline:
					if (!pass) {
						w.flags |= InputWindow::BaselineFailed;
						log::warn("dynamic: input #{} (tick {}): the unmodified macro dies from the checkpoint at tick {}; skipped",
							s.idx, s.base[s.idx].frame, s.checkpointTick);
						finishInput(pl);
						return;
					}
					s.side = Side::Early;
					s.k = 0;
					nextCandidate(pl);
					return;
				case Side::Early:
					if (pass) {
						w.early = static_cast<uint8_t>(s.k);
						nextCandidate(pl);
					} else {
						s.side = Side::Late;
						s.k = 0;
						nextCandidate(pl);
					}
					return;
				case Side::Late:
					if (pass) {
						w.late = static_cast<uint8_t>(s.k);
						nextCandidate(pl);
					} else {
						finishInput(pl);
					}
					return;
			}
		}

		void placeCheckpoint(PlayLayer* pl, uint32_t tick) {
			if (s.jobs[s.job].checkpoint == 0) {
				s.checkpointTick = 0; // trials restart from the very start (no checkpoint)
				return;
			}
			pl->markCheckpoint();
			// Keep only the newest checkpoint (practice respawns go to the last one anyway).
			while (pl->m_checkpointArray && pl->m_checkpointArray->count() > 1) pl->removeCheckpoint(true);
			s.checkpointTick = tick;
			if (tick != s.jobs[s.job].checkpoint) {
				log::warn("dynamic: checkpoint for tick {} placed at tick {} (frame ran past it)", s.jobs[s.job].checkpoint, tick);
			}
		}

		void beginJob(PlayLayer* pl) {
			s.jobInput = 0;
			startInput(pl);
		}

		void writeReport();
		void finishScan(bool completed);

		void finishInput(PlayLayer* pl) {
			s.result[s.idx].window.scanned = true;
			s.inputsDone++;
			s.jobInput++;
			if (s.jobInput < s.jobs[s.job].inputs.size()) {
				startInput(pl);
			} else {
				finishJob(pl);
			}
		}

		void finishJob(PlayLayer* pl) {
			s.job++;
			MacroManager::get().setTrialInputs(s.base);
			if (s.job >= s.jobs.size()) {
				finishScan(true);
				return;
			}
			if (s.jobs[s.job].checkpoint == s.checkpointTick) {
				beginJob(pl);
				return;
			}
			// Play the unmodified macro on from the current checkpoint to the next one.
			s.phase = Phase::Advance;
			pl->resetLevel();
		}

		void stop() {
			s.active = false;
			s.pending = false;
			mute(false);
			auto& mgr = MacroManager::get();
			mgr.setQuiet(false);
			mgr.restoreInputs();
			mgr.endSeek(false, 0);
			runtime::quiet = false;
		}

		std::string tickTime(uint32_t tick) { return fmt::format("{:.3f}s", tick / 480.0); }

		void writeReport() {
			auto& mgr = MacroManager::get();
			auto const& play = mgr.playMacro();
			std::string path = mgr.playPath();
			std::string name = path.empty() ? std::string("unsaved macro") : store::fileNameOf(path);
			std::string reportPath = path.empty()
				? store::macrosDir() + "/dynamic_windows.txt"
				: path.substr(0, path.size() - std::string(".gdr2").size()) + ".windows.txt";

			std::string out;
			out += fmt::format("# DynamicButter window scan: {}\n", name);
			out += fmt::format("# level {} '{}', {} inputs, scan range {} steps each side, pass = alive {} ticks past the next input\n",
				play ? play->levelID : 0, play ? play->levelName : "", s.result.size(), s.range, MARGIN_TICKS);
			out += "# window: steps earlier / later the input still passes (1 step = 2 ticks = 1/240 s)\n";
			out += "# flags: <prev = stopped at the previous input, next> = stopped at the next input, max = scan range, FAIL = macro dies unmodified\n";
			out += "#   idx    tick      time  key action   early late  frames   flags\n";

			std::vector<std::pair<int, size_t>> tightest;
			for (size_t i = 0; i < s.result.size(); ++i) {
				auto const& in = s.result[i];
				auto const& w = in.window;
				std::string flags;
				if (w.flags & InputWindow::EarlyBound) flags += " <prev";
				if (w.flags & InputWindow::LateBound) flags += " next>";
				if (w.flags & (InputWindow::EarlyMax | InputWindow::LateMax)) flags += " max";
				if (w.flags & InputWindow::BaselineFailed) flags += " FAIL";
				out += fmt::format("{:>6} {:>7} {:>9}  {}   {:<7} {:>5} {:>4}  {:>6}  {}\n",
					i, in.frame, tickTime(in.frame), in.player2 ? "P2" : "P1", in.down ? "press" : "release",
					w.early, w.late, int(w.early) + int(w.late) + 1, flags);
				if (w.scanned && !(w.flags & InputWindow::BaselineFailed) && in.down) {
					tightest.push_back({int(w.early) + int(w.late) + 1, i});
				}
			}
			std::sort(tightest.begin(), tightest.end());
			out += "\n# tightest presses (window in frames):\n";
			for (size_t j = 0; j < std::min<size_t>(15, tightest.size()); ++j) {
				auto i = tightest[j].second;
				out += fmt::format("#   input {} at {} (tick {}): {} frame(s), {} early / {} late\n",
					i, tickTime(s.result[i].frame), s.result[i].frame, tightest[j].first,
					s.result[i].window.early, s.result[i].window.late);
			}
			if (auto res = file::writeString(reportPath, out); !res) {
				log::error("dynamic: writing the report failed: {}", res.unwrapErr());
			} else {
				log::info("dynamic: report written to {}", reportPath);
			}
		}

		void finishScan(bool completed) {
			auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - s.started).count();
			auto result = s.result;
			size_t scanned = s.inputsDone;
			uint64_t trials = s.trials, ticks = s.ticksSimulated;
			stop();
			log::info("dynamic: scan {} - {} of {} inputs, {} trials, {} ticks simulated in {:.1f} s ({:.0f} ticks/s)",
				completed ? "finished" : "stopped", scanned, result.size(), trials, ticks, ms / 1000.0,
				ms > 0 ? ticks * 1000.0 / ms : 0.0);
			s.result = result;
			if (completed) {
				MacroManager::get().saveWindows(result);
				writeReport();
				Notification::create(fmt::format("Window scan done: {} inputs", scanned), NotificationIcon::Success)->show();
			}
		}
	}

	bool enabled() {
		return Mod::get()->getSettingValue<bool>("dynamic-enabled");
	}

	bool active() { return s.active; }

	bool start() {
		auto pl = PlayLayer::get();
		if (!enabled() || !pl || s.active || s.pending) return false;
		auto& mgr = MacroManager::get();
		if (mgr.isSeeking()) return false;
		if (!mgr.ensurePlayMacro() || mgr.playMacro()->inputs.empty()) {
			Notification::create("No macro for this level to scan", NotificationIcon::Error)->show();
			return false;
		}
		s = State{};
		s.base = mgr.playMacro()->inputs;
		for (auto& in : s.base) in.window = {};
		s.result = s.base;
		s.range = static_cast<int>(std::clamp<int64_t>(Mod::get()->getSettingValue<int64_t>("dynamic-scan-range"), 1, 60));
		buildJobs();
		s.pending = true;
		log::info("dynamic: scanning {} inputs of {} ({} checkpoints, range {} steps each side)",
			s.base.size(), mgr.playSource(), s.jobs.size(), s.range);

		Loader::get()->queueInMainThread([] {
			auto pl = PlayLayer::get();
			if (!pl || !s.pending) { s.pending = false; return; }
			if (auto scene = CCDirector::get()->getRunningScene()) {
				if (auto pause = scene->getChildByType<PauseLayer>(0)) pause->onResume(nullptr);
			}
			if (!pl->m_isPracticeMode) pl->togglePracticeMode(true);
			auto& mgr = MacroManager::get();
			mgr.beginSeek(); // playback runs whatever the mode is
			mgr.setQuiet(true);
			runtime::quiet = true;
			mgr.setTrialInputs(s.base);
			s.pending = false;
			s.active = true;
			s.started = Clock::now();
			s.phase = Phase::Advance;
			s.job = 0;
			mute(true);
			pl->removeAllCheckpoints();
			pl->resetLevelFromStart();
		});
		return true;
	}

	void abort(char const* why) {
		if (!s.active && !s.pending) return;
		log::warn("dynamic: scan stopped ({}) after {} inputs", why, s.inputsDone);
		finishScan(false);
		Notification::create(fmt::format("Window scan stopped: {}", why), NotificationIcon::Warning)->show();
	}

	std::string status() {
		if (!s.active) return {};
		return fmt::format("Dynamic scan {}/{} ({} trials)", s.inputsDone, s.base.size(), s.trials);
	}

	bool wantsFastForward() {
		return s.active && PlayLayer::get();
	}

	void afterVirtualFrame() {
		if (!s.active) return;
		auto pl = PlayLayer::get();
		if (!pl) { abort("left the level"); return; }
		uint32_t tick = pl->m_gameState.m_currentProgress;

		if (tick == s.lastTick) {
			if (++s.stalledFrames >= 2400) { abort("the level stopped advancing"); return; }
		} else {
			if (tick > s.lastTick) s.ticksSimulated += tick - s.lastTick;
			s.stalledFrames = 0;
			s.lastTick = tick;
		}

		if (s.phase == Phase::Advance) {
			if (tick >= s.jobs[s.job].checkpoint) {
				placeCheckpoint(pl, tick);
				beginJob(pl);
			}
			return;
		}

		// Trial
		if (s.trialDead) {
			onTrialResult(pl, false);
		} else if (s.trialCompleted || tick >= s.horizon) {
			onTrialResult(pl, true);
		}
		if (s.active) s.lastTick = pl->m_gameState.m_currentProgress;
	}

	bool onPlayerDied(PlayLayer* pl, void* object) {
		if (!s.active || !pl) return false;
		if (object && object == pl->m_anticheatSpike) return false;
		if (s.phase == Phase::Trial) {
			s.trialDead = true;
			return true; // no GD death, no respawn timer: the next trial respawns right away
		}
		// The unmodified macro died on the way to a checkpoint.
		uint32_t tick = pl->m_gameState.m_currentProgress;
		Loader::get()->queueInMainThread([tick] {
			abort(fmt::format("the macro died at tick {} while advancing", tick).c_str());
		});
		return true;
	}

	bool onLevelComplete() {
		if (!s.active) return false;
		if (s.phase == Phase::Trial) {
			s.trialCompleted = true;
			return true;
		}
		Loader::get()->queueInMainThread([] { abort("the level ended while advancing"); });
		return true;
	}
}
