#include "dynamic/WindowScan.hpp"

#include <Geode/Geode.hpp>
#include <Geode/utils/file.hpp>

#include <algorithm>
#include <chrono>
#include <map>
#include <optional>
#include <vector>

#include "dynamic/ExactState.hpp"
#include "macro/MacroManager.hpp"
#include "macro/MacroStore.hpp"
#include "physics/PlayerState.hpp"

using namespace geode::prelude;

namespace db::scan {
	namespace {
		using Clock = std::chrono::steady_clock;

		struct Event {
			uint32_t tick = 0;
			int object = 0; // unique object ID
			Interaction kind = Interaction::Ring;
		};

		// Inputs that share the checkpoint placed at `checkpoint`.
		struct Job {
			uint32_t checkpoint = 0;
			uint32_t verifyEnd = 0; // the replay from the checkpoint is checked up to here
			std::vector<size_t> inputs;
		};

		enum class Phase { Survey, Advance, Verify, Trial };
		enum class Side { Early, Late };
		enum class Outcome { Exact, Chained, Survived, Died };

		struct State {
			bool pending = false;
			bool active = false;
			Phase phase = Phase::Survey;

			std::vector<MacroInput> base;   // the macro's inputs, unmodified
			std::vector<MacroInput> result; // base + windows
			std::vector<bool> toScan;
			std::vector<Job> jobs;
			size_t job = 0;
			size_t jobInput = 0;
			uint32_t checkpointTick = 0;    // where the current checkpoint is (0 = level start)
			// GD's checkpoints round the y position and y-velocity (seen in ship sections), so the scan
			// keeps the exact player state from the moment it placed the checkpoint and puts it back
			// after every respawn there.
			PlayerState cpP1, cpP2;
			bool cpDual = false;
			int range = 20;

			// survey (the unbroken reference run)
			std::vector<ExactState> ref;    // state at the end of step tick 2, 4, 6 ... (index tick/2 - 1)
			std::vector<Event> refEvents;
			std::vector<uint32_t> anchors;  // ticks where a resting period starts
			std::vector<uint32_t> shipStarts; // ticks where the reference run turns into a ship
			bool surveyDone = false;
			bool levelEnded = false;        // the survey reached the end of the level
			uint32_t refEndTick = 0;

			// verify / trial run
			std::vector<Event> runEvents;
			size_t eventOffset = 0;         // reference events before the checkpoint
			bool eventsDiverged = false;
			bool verifyFailed = false;
			uint32_t verifyMismatchTick = 0;
			std::string verifyMismatch;
			bool runDone = false;

			// current input / trial
			size_t idx = 0;
			Side side = Side::Early;
			int k = 0;
			bool sideFailed = false;
			int probesLeft = 0;
			bool exactRun = true;
			uint32_t trialFrame = 0;
			uint32_t horizon = 0;
			bool trialDead = false;
			bool trialCompleted = false;
			uint32_t completeTick = 0;
			int equalRun = 0;
			bool reconverged = false;
			uint32_t reconvergeTick = 0;

			// progress
			size_t inputsTotal = 0;
			size_t inputsDone = 0;
			uint64_t trials = 0;
			uint64_t ticksSimulated = 0;
			uint32_t lastTick = 0;
			unsigned stalledFrames = 0;
			Clock::time_point started;
			unsigned outcomes[4] = {};

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
		uint32_t lastFrame() { return s.base.empty() ? 0 : s.base.back().frame; }

		std::optional<size_t> refIndex(uint32_t tick) {
			if (tick < 2) return std::nullopt;
			size_t i = tick / 2 - 1;
			if (i >= s.ref.size()) return std::nullopt;
			return i;
		}

		size_t refEventsUpTo(uint32_t tick) {
			return std::upper_bound(s.refEvents.begin(), s.refEvents.end(), tick,
				[](uint32_t t, Event const& e) { return t < e.tick; }) - s.refEvents.begin();
		}

		// ---------------------------------------------------------------- resting points

		bool restingAt(size_t i, Macro const& macro) {
			if (i + CONFIRM_STEPS > s.ref.size()) return false;
			// Settled: grounded with the same height, y-velocity, gamemode, size and speed for
			// CONFIRM_STEPS steps (only x moves), no button held. Any run that lands here the same way
			// is identical from then on.
			auto const& a = s.ref[i].p1;
			if (!a.resting()) return false;
			for (int j = 1; j < CONFIRM_STEPS; ++j) {
				if (!s.ref[i + j].p1.settledWith(a)) return false;
			}
			return !macro.heldAt(static_cast<uint32_t>((i + 1) * 2));
		}

		void findAnchors() {
			Macro tmp;
			tmp.inputs = s.base;
			// Diagnostics: grounded steps, how many are settled, and the y-velocities GD keeps on the ground.
			size_t ground = 0, candidate = 0, settled = 0;
			std::map<std::string, size_t> changed;
			std::map<double, size_t> groundYVel;
			for (size_t i = 0; i < s.ref.size(); ++i) {
				auto const& p = s.ref[i].p1;
				if (!p.basic.onGround) continue;
				ground++;
				groundYVel[p.basic.yVel]++;
				if (!p.resting()) continue;
				candidate++;
				if (i == 0) continue;
				auto const& q = s.ref[i - 1].p1;
				if (p.settledWith(q)) { settled++; continue; }
				// Which value broke "same state as the step before" (first one found).
				if (!q.resting()) changed["prev not resting"]++;
				else if (p.basic.y != q.basic.y) changed["y"]++;
				else if (p.basic.yVel != q.basic.yVel) changed["yVel"]++;
				else if (p.basic.gamemode != q.basic.gamemode) changed["mode"]++;
				else if (p.basic.upsideDown != q.basic.upsideDown) changed["gravity"]++;
				else if (p.basic.vehicleSize != q.basic.vehicleSize) changed["size"]++;
				else if (p.basic.playerSpeed != q.basic.playerSpeed) changed["speed"]++;
				else if (p.onSlope != q.onSlope) changed["slope"]++;
				else if (p.jumpBuffered != q.jumpBuffered) changed["jumpBuffer"]++;
				else if (p.ringJump != q.ringJump) changed["ringJump"]++;
				else changed["other"]++;
			}
			std::vector<std::pair<size_t, double>> common;
			for (auto const& [v, n] : groundYVel) common.push_back({n, v});
			std::sort(common.rbegin(), common.rend());
			std::string values;
			for (size_t j = 0; j < std::min<size_t>(6, common.size()); ++j) {
				values += fmt::format(" {}x{}", common[j].second, common[j].first);
			}
			std::string why;
			for (auto const& [name, n] : changed) why += fmt::format(" {}x{}", name, n);
			log::info("dynamic: resting check over {} steps: on ground {}, not holding/dashing {}, same state as the step before {}; ground yVel values (value x steps):{}; changed from the step before:{}",
				s.ref.size(), ground, candidate, settled, values, why);
			bool prev = false;
			for (size_t i = 0; i < s.ref.size(); ++i) {
				bool r = restingAt(i, tmp);
				if (r && !prev) s.anchors.push_back(static_cast<uint32_t>((i + 1) * 2));
				bool ship = s.ref[i].p1.basic.gamemode & 1;
				bool wasShip = i > 0 && (s.ref[i - 1].p1.basic.gamemode & 1);
				if (ship && !wasShip) s.shipStarts.push_back(static_cast<uint32_t>((i + 1) * 2));
				prev = r;
			}
		}

		// The trial must have reconverged by the first resting point after both the recorded and the
		// moved input (plus the confirmation steps), else it can't be shown to rejoin the macro.
		uint32_t horizonFor(size_t i, uint32_t frame) {
			uint32_t after = std::max(frame, s.base[i].frame);
			auto it = std::upper_bound(s.anchors.begin(), s.anchors.end(), after);
			uint32_t h = it != s.anchors.end() ? *it + 2 * CONFIRM_STEPS + 2 : s.refEndTick;
			// Ship parts are out of scope: a trial must have rejoined the run before the next ship
			// section starts (the ship has no resting points, so it couldn't be shown later anyway).
			auto ship = std::upper_bound(s.shipStarts.begin(), s.shipStarts.end(), after);
			if (ship != s.shipStarts.end()) h = std::min(h, *ship);
			return std::min(h, s.refEndTick);
		}

		uint32_t minFrame(size_t i) {
			return std::max(prevFrame(i) + 2, s.checkpointTick + 2);
		}

		void buildJobs() {
			Macro tmp;
			tmp.inputs = s.base;
			std::vector<std::pair<uint32_t, size_t>> cps;
			for (size_t i = 0; i < s.base.size(); ++i) {
				if (!s.toScan[i]) continue;
				uint32_t p = s.base[i].frame;
				int64_t earliest = std::max<int64_t>(int64_t(prevFrame(i)) + 2, int64_t(p) - 2 * (s.range + GAP_PROBE));
				if (earliest > p) earliest = p;
				uint32_t cp = earliest >= 2 ? tmp.latestFreeTick(uint32_t(earliest - 2)) : 0;
				cps.push_back({cp, i});
			}
			std::stable_sort(cps.begin(), cps.end());
			for (auto const& [cp, i] : cps) {
				if (s.jobs.empty() || s.jobs.back().checkpoint != cp) s.jobs.push_back({cp, 0, {}});
				auto& job = s.jobs.back();
				job.inputs.push_back(i);
				uint32_t latest = std::min<uint32_t>(nextFrame(i), s.base[i].frame + 2 * (s.range + GAP_PROBE));
				job.verifyEnd = std::max(job.verifyEnd, horizonFor(i, latest));
			}
		}

		std::vector<MacroInput> movedInputs(size_t i, uint32_t frame) {
			auto in = s.base;
			in[i].frame = frame;
			std::stable_sort(in.begin(), in.end(),
				[](MacroInput const& a, MacroInput const& b) { return a.frame < b.frame; });
			return in;
		}

		// ---------------------------------------------------------------- runs

		void respawn(PlayLayer* pl) {
			pl->resetLevel(); // respawn at the checkpoint, like the respawn key
			if (s.checkpointTick == 0) return; // level start: nothing rounded
			s.cpP1.restore(pl->m_player1);
			if (s.cpDual) s.cpP2.restore(pl->m_player2);
		}

		void resetRun() {
			s.runEvents.clear();
			s.eventOffset = refEventsUpTo(s.checkpointTick);
			s.eventsDiverged = false;
			s.runDone = false;
			s.trialDead = false;
			s.trialCompleted = false;
			s.completeTick = 0;
			s.equalRun = 0;
			s.reconverged = false;
			s.reconvergeTick = 0;
		}

		void launchTrial(PlayLayer* pl, uint32_t frame) {
			s.trialFrame = frame;
			s.horizon = horizonFor(s.idx, frame);
			resetRun();
			s.phase = Phase::Trial;
			s.trials++;
			MacroManager::get().setTrialInputs(movedInputs(s.idx, frame));
			respawn(pl);
		}

		void finishInput(PlayLayer* pl);
		void finishJob(PlayLayer* pl);

		void startSide(Side side) {
			s.side = side;
			s.k = 0;
			s.sideFailed = false;
			s.probesLeft = GAP_PROBE;
			s.exactRun = true;
		}

		std::optional<uint32_t> nextFrameOnSide() {
			auto& w = s.result[s.idx].window;
			uint32_t p = s.base[s.idx].frame;
			s.k++;
			if (!s.sideFailed && s.k > s.range) {
				w.flags |= s.side == Side::Early ? InputWindow::EarlyMax : InputWindow::LateMax;
				return std::nullopt;
			}
			if (s.sideFailed && s.probesLeft-- <= 0) return std::nullopt;
			if (s.side == Side::Early) {
				int64_t f = int64_t(p) - 2 * s.k;
				if (f < int64_t(minFrame(s.idx))) {
					if (!s.sideFailed) w.flags |= InputWindow::EarlyBound;
					return std::nullopt;
				}
				return uint32_t(f);
			}
			uint32_t f = p + 2 * s.k;
			if (f + 2 > nextFrame(s.idx) || f + 2 * CONFIRM_STEPS > s.refEndTick) {
				if (!s.sideFailed) w.flags |= InputWindow::LateBound;
				return std::nullopt;
			}
			return f;
		}

		void continueInput(PlayLayer* pl) {
			while (true) {
				if (auto f = nextFrameOnSide()) {
					launchTrial(pl, *f);
					return;
				}
				if (s.side == Side::Early) {
					startSide(Side::Late);
					continue;
				}
				finishInput(pl);
				return;
			}
		}

		void startInput(PlayLayer* pl) {
			s.idx = s.jobs[s.job].inputs[s.jobInput];
			s.result[s.idx].window = {};
			startSide(Side::Early);
			continueInput(pl);
		}

		Outcome trialOutcome() {
			if (s.reconverged || s.trialCompleted) {
				uint32_t at = s.reconverged ? s.reconvergeTick : s.completeTick;
				// Exact when no later input was needed before the run was back on the macro.
				bool laterApplied = s.idx + 1 < s.base.size() && s.base[s.idx + 1].frame <= at;
				return laterApplied ? Outcome::Chained : Outcome::Exact;
			}
			return s.trialDead ? Outcome::Died : Outcome::Survived;
		}

		void onTrialDone(PlayLayer* pl) {
			auto outcome = trialOutcome();
			s.outcomes[static_cast<int>(outcome)]++;
			bool pass = outcome == Outcome::Exact || outcome == Outcome::Chained;
			auto& w = s.result[s.idx].window;
			if (!s.sideFailed) {
				if (pass) {
					auto steps = static_cast<uint8_t>(s.k);
					bool early = s.side == Side::Early;
					(early ? w.early : w.late) = steps;
					if (outcome == Outcome::Exact && s.exactRun) (early ? w.earlyExact : w.lateExact) = steps;
					if (outcome != Outcome::Exact) s.exactRun = false;
				} else {
					s.sideFailed = true;
				}
			} else if (pass) {
				w.flags |= s.side == Side::Early ? InputWindow::EarlyGap : InputWindow::LateGap;
				s.probesLeft = 0;
			}
			continueInput(pl);
		}

		void placeCheckpoint(PlayLayer* pl, uint32_t tick) {
			auto const& job = s.jobs[s.job];
			if (job.checkpoint == 0) {
				s.checkpointTick = 0; // the level start is the "checkpoint"
				return;
			}
			pl->markCheckpoint(); // created immediately
			while (pl->m_checkpointArray && pl->m_checkpointArray->count() > 1) pl->removeCheckpoint(true);
			s.checkpointTick = tick;
			s.cpDual = pl->m_gameState.m_isDualMode;
			s.cpP1 = PlayerState::capture(pl->m_player1);
			s.cpP2 = PlayerState::capture(pl->m_player2);
			if (tick != job.checkpoint) {
				log::warn("dynamic: checkpoint for tick {} placed at tick {} (frame ran past it)", job.checkpoint, tick);
			}
		}

		void startVerify(PlayLayer* pl) {
			resetRun();
			s.verifyFailed = false;
			s.verifyMismatch.clear();
			s.phase = Phase::Verify;
			respawn(pl);
		}

		void writeReport();
		void finishScan(bool completed);
		void savePartial();

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
			savePartial(); // a stopped scan keeps every finished group
			s.job++;
			MacroManager::get().setTrialInputs(s.base);
			if (s.job >= s.jobs.size()) {
				finishScan(true);
				return;
			}
			// Play the unmodified macro on from the current checkpoint to the next one.
			s.phase = Phase::Advance;
			resetRun();
			respawn(pl);
		}

		void restoreLevel() {
			// Leave the level in a clean state: a trial may have stopped past the end wall with the
			// completion swallowed, which leaves GD stuck. Restart from the very start.
			Loader::get()->queueInMainThread([] {
				if (auto pl = PlayLayer::get()) {
					pl->removeAllCheckpoints();
					pl->resetLevelFromStart();
				}
			});
		}

		unsigned s_endUIBlocked = 0;

		void stop() {
			if (s_endUIBlocked) log::info("dynamic: blocked {} end-of-level animations/popups during the scan", s_endUIBlocked);
			s_endUIBlocked = 0;
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
			out += fmt::format("# level {} '{}', {} inputs, scan range {} steps each side, measured at 240 FPS (1 physics step per frame)\n",
				play ? play->levelID : 0, play ? play->levelName : "", s.result.size(), s.range);
			out += fmt::format("# {} resting points (sections){}\n", s.anchors.size(), s.levelEnded ? ", reference run reached the end of the level" : "");
			out += fmt::format("# pass = the run comes back to exactly the recorded run ({} identical steps, same orbs/pads/triggers/pickups)\n", CONFIRM_STEPS);
			out += "#   by the next resting point, or beats the level. exact = before the next input; chained = later\n";
			out += "# early/late: steps (1 step = 2 ticks = 1/240 s) that pass; (x) = how many of those nearest the recorded frame are exact\n";
			out += "# flags: <prev / next> = scan reached the neighbouring input or checkpoint, max = scan range, gap< / gap> = a frame past the\n";
			out += "#        first failure passed again, UNMEASURED = the checkpoint replay didn't match the recorded run, - = not scanned,\n";
			out += "#        ship (skipped) = ship parts are left out of Dynamic for now\n";
			out += "#   idx    tick      time  key action    early     late  frames   flags\n";

			std::vector<std::pair<int, size_t>> tightest;
			for (size_t i = 0; i < s.result.size(); ++i) {
				auto const& in = s.result[i];
				auto const& w = in.window;
				std::string flags;
				if (!w.scanned) flags += " -";
				if (w.flags & InputWindow::EarlyBound) flags += " <prev";
				if (w.flags & InputWindow::LateBound) flags += " next>";
				if (w.flags & (InputWindow::EarlyMax | InputWindow::LateMax)) flags += " max";
				if (w.flags & InputWindow::EarlyGap) flags += " gap<";
				if (w.flags & InputWindow::LateGap) flags += " gap>";
				if (w.flags & InputWindow::BaselineFailed) flags += " UNMEASURED";
				if (w.flags & InputWindow::ShipSkipped) flags += " ship (skipped)";
				out += fmt::format("{:>6} {:>7} {:>9}  {}   {:<7} {:>3}({:>2}) {:>3}({:>2})  {:>6}  {}\n",
					i, in.frame, tickTime(in.frame), in.player2 ? "P2" : "P1", in.down ? "press" : "release",
					w.early, w.earlyExact, w.late, w.lateExact, int(w.early) + int(w.late) + 1, flags);
				if (w.scanned && !(w.flags & (InputWindow::BaselineFailed | InputWindow::ShipSkipped)) && in.down) {
					tightest.push_back({int(w.early) + int(w.late) + 1, i});
				}
			}
			std::sort(tightest.begin(), tightest.end());
			out += "\n# tightest presses (window in frames):\n";
			for (size_t j = 0; j < std::min<size_t>(15, tightest.size()); ++j) {
				auto i = tightest[j].second;
				auto const& w = s.result[i].window;
				out += fmt::format("#   input {} at {} (tick {}): {} frame(s), {} early ({} exact) / {} late ({} exact)\n",
					i, tickTime(s.result[i].frame), s.result[i].frame, tightest[j].first,
					w.early, w.earlyExact, w.late, w.lateExact);
			}
			if (auto res = file::writeString(reportPath, out); !res) {
				log::error("dynamic: writing the report failed: {}", res.unwrapErr());
			} else {
				log::info("dynamic: report written to {}", reportPath);
			}
		}

		void savePartial() {
			if (s.inputsDone == 0) return;
			MacroManager::get().saveWindows(s.result);
		}

		void finishScan(bool completed) {
			auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - s.started).count();
			unsigned exact = s.outcomes[0], chained = s.outcomes[1], survived = s.outcomes[2], died = s.outcomes[3];
			log::info("dynamic: scan {} - {} of {} inputs, {} sections, {} trials (exact {}, chained {}, survived only {}, died {}), {} ticks simulated in {:.1f} s ({:.0f} ticks/s)",
				completed ? "finished" : "stopped", s.inputsDone, s.inputsTotal, s.anchors.size(), s.trials,
				exact, chained, survived, died, s.ticksSimulated, ms / 1000.0, ms > 0 ? s.ticksSimulated * 1000.0 / ms : 0.0);
			bool any = s.inputsDone > 0;
			if (any) savePartial();
			stop();
			restoreLevel();
			if (any) writeReport();
			if (completed) {
				Notification::create(fmt::format("Window scan done: {} inputs", s.inputsDone), NotificationIcon::Success)->show();
			}
		}

		void onRunEvent(Event const& e) {
			if (s.phase == Phase::Survey) {
				s.refEvents.push_back(e);
				return;
			}
			if (s.phase != Phase::Verify && s.phase != Phase::Trial) return;
			size_t i = s.eventOffset + s.runEvents.size();
			s.runEvents.push_back(e);
			if (s.eventsDiverged) return;
			if (i >= s.refEvents.size()) { s.eventsDiverged = true; return; }
			auto const& r = s.refEvents[i];
			bool same = r.kind == e.kind && r.object == e.object;
			// Orbs, pads and pickups change the level the same way whenever they're used; touch
			// triggers start things (moves, spawns) whose timing matters, so their tick must match.
			if (e.kind == Interaction::Trigger && r.tick != e.tick) same = false;
			if (!same) s.eventsDiverged = true;
		}

		// Ship parts are left out of Dynamic for now (user's call): a click where the reference run is
		// a ship isn't scanned and is flagged so Dynamic leaves it alone.
		void skipShipInputs() {
			size_t skipped = 0;
			for (size_t i = 0; i < s.base.size(); ++i) {
				if (!s.toScan[i]) continue;
				auto r = refIndex(s.base[i].frame);
				if (!r || !(s.ref[*r].p1.basic.gamemode & 1)) continue; // bit 0 = ship
				s.toScan[i] = false;
				s.result[i].window = {};
				s.result[i].window.scanned = true;
				s.result[i].window.flags = InputWindow::ShipSkipped;
				skipped++;
			}
			if (skipped) {
				s.inputsTotal -= skipped;
				log::info("dynamic: {} inputs are in ship parts and are skipped", skipped);
			}
		}

		void finishSurvey(PlayLayer* pl) {
			s.refEndTick = static_cast<uint32_t>(s.ref.size() * 2);
			findAnchors();
			skipShipInputs();
			buildJobs();
			log::info("dynamic: reference run recorded to tick {} ({:.2f} s{}), {} interactions, {} resting points, {} ship sections; {} inputs to scan in {} groups",
				s.refEndTick, s.refEndTick / 480.0, s.levelEnded ? ", level end" : "", s.refEvents.size(),
				s.anchors.size(), s.shipStarts.size(), s.inputsTotal, s.jobs.size());
			if (s.jobs.empty()) {
				finishScan(true);
				return;
			}
			// Back to the start; every group then advances from wherever the last one stopped.
			s.job = 0;
			s.checkpointTick = 0;
			s.phase = Phase::Advance;
			resetRun();
			pl->removeAllCheckpoints();
			pl->resetLevelFromStart();
		}
	}

	bool enabled() {
		return Mod::get()->getSettingValue<bool>("dynamic-enabled");
	}

	bool active() { return s.active; }

	bool suppressEndUI() {
		bool block = s.active || s.pending;
		if (block) s_endUIBlocked++;
		return block;
	}

	bool start(bool rescanAll) {
		auto pl = PlayLayer::get();
		if (!enabled() || !pl || s.active || s.pending) return false;
		auto& mgr = MacroManager::get();
		if (mgr.isSeeking()) return false;
		if (!mgr.ensurePlayMacro() || mgr.playMacro()->inputs.empty()) {
			Notification::create("No macro for this level to scan", NotificationIcon::Error)->show();
			return false;
		}
		s = State{};
		s.result = mgr.playMacro()->inputs;
		s.base = s.result;
		for (auto& in : s.base) in.window = {};
		s.toScan.assign(s.base.size(), true);
		if (!rescanAll) {
			for (size_t i = 0; i < s.result.size(); ++i) {
				auto const& w = s.result[i].window;
				s.toScan[i] = !w.scanned || (w.flags & InputWindow::BaselineFailed);
			}
		}
		for (size_t i = 0; i < s.result.size(); ++i) {
			if (s.toScan[i]) s.result[i].window = {};
		}
		s.inputsTotal = std::count(s.toScan.begin(), s.toScan.end(), true);
		if (s.inputsTotal == 0) {
			Notification::create("Every input already has a window (use Rescan all)", NotificationIcon::Info)->show();
			return false;
		}
		s.range = static_cast<int>(std::clamp<int64_t>(Mod::get()->getSettingValue<int64_t>("dynamic-scan-range"), 1, 60));
		s.pending = true;
		log::info("dynamic: scanning {} of {} inputs of {} (range {} steps each side, exact reconvergence by the next resting point)",
			s.inputsTotal, s.base.size(), mgr.playSource(), s.range);

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
			s.phase = Phase::Survey;
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
		Notification::create(fmt::format("Window scan stopped: {} ({} inputs kept)", why, s.inputsDone), NotificationIcon::Warning)->show();
	}

	std::string status() {
		if (!s.active) return {};
		if (s.phase == Phase::Survey) return fmt::format("Dynamic scan: reference run {:.1f} s", s.ref.size() / 240.0);
		return fmt::format("Dynamic scan {}/{}  group {}/{}  ({} trials)",
			s.inputsDone, s.inputsTotal, s.job + 1, s.jobs.size(), s.trials);
	}

	bool wantsFastForward() {
		return s.active && PlayLayer::get();
	}

	void onInteraction(Interaction kind, GameObject* obj) {
		if (!s.active || !obj) return;
		auto pl = PlayLayer::get();
		if (!pl) return;
		onRunEvent({pl->m_gameState.m_currentProgress, obj->m_uniqueID, kind});
	}

	void onStepEnd(PlayLayer* pl, uint32_t tick) {
		if (!s.active || !pl) return;
		switch (s.phase) {
			case Phase::Advance:
				return;
			case Phase::Survey: {
				if (s.surveyDone || tick < 2) return;
				size_t want = tick / 2 - 1;
				if (want != s.ref.size()) {
					log::warn("dynamic: reference run skipped from step {} to {}", s.ref.size(), want);
					if (want > s.ref.size()) s.ref.resize(want, s.ref.empty() ? ExactState{} : s.ref.back());
					else s.ref.resize(want);
				}
				s.ref.push_back(ExactState::capture(pl));
				// A few seconds past the last input is enough when the level doesn't end.
				if (tick >= lastFrame() + 960) s.surveyDone = true;
				return;
			}
			case Phase::Verify: {
				if (s.runDone) return;
				auto i = refIndex(tick);
				uint32_t end = s.jobs[s.job].verifyEnd;
				if (!i || tick > end) { s.runDone = true; return; }
				if (!s.verifyFailed) {
					auto now = ExactState::capture(pl);
					if (!(now == s.ref[*i])) {
						s.verifyFailed = true;
						s.verifyMismatchTick = tick;
						s.verifyMismatch = now.diff(s.ref[*i]);
					} else if (s.eventsDiverged) {
						s.verifyFailed = true;
						s.verifyMismatchTick = tick;
						s.verifyMismatch = "different orb/pad/trigger/pickup";
					}
				}
				if (tick >= end) {
					s.runDone = true;
					size_t expected = refEventsUpTo(end) - s.eventOffset;
					if (!s.verifyFailed && s.runEvents.size() != expected) {
						s.verifyFailed = true;
						s.verifyMismatchTick = tick;
						s.verifyMismatch = fmt::format("{} interactions instead of {}", s.runEvents.size(), expected);
					}
				}
				return;
			}
			case Phase::Trial: {
				if (s.runDone || s.trialDead || s.reconverged) return;
				auto i = refIndex(tick);
				// A trial whose horizon is the end of the level keeps going until it completes the level
				// (a pass) or dies: a slightly different run can reach the end wall a little later.
				bool toLevelEnd = s.levelEnded && s.horizon >= s.refEndTick;
				if (!i || tick > s.horizon) {
					if (!(toLevelEnd && tick <= s.refEndTick + 960)) s.runDone = true;
					return;
				}
				if (tick < std::max(s.trialFrame, s.base[s.idx].frame)) return; // moved input not applied yet
				bool eq = !s.eventsDiverged && ExactState::capture(pl) == s.ref[*i]
					&& s.runEvents.size() == refEventsUpTo(tick) - s.eventOffset;
				if (!eq) {
					s.equalRun = 0;
					return;
				}
				if (++s.equalRun >= CONFIRM_STEPS) {
					s.reconverged = true;
					s.reconvergeTick = tick - 2 * (CONFIRM_STEPS - 1);
				}
				return;
			}
		}
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

		switch (s.phase) {
			case Phase::Survey:
				if (s.surveyDone || s.levelEnded) finishSurvey(pl);
				break;
			case Phase::Advance:
				if (tick >= s.jobs[s.job].checkpoint) {
					placeCheckpoint(pl, tick);
					startVerify(pl);
				}
				break;
			case Phase::Verify:
				if (s.runDone || s.trialCompleted) {
					if (s.verifyFailed) {
						auto const& job = s.jobs[s.job];
						log::warn("dynamic: checkpoint at tick {} doesn't replay exactly: first difference at tick {}:{}; {} inputs unmeasured",
							s.checkpointTick, s.verifyMismatchTick, s.verifyMismatch, job.inputs.size());
						for (auto i : job.inputs) {
							s.result[i].window = {};
							s.result[i].window.flags = InputWindow::BaselineFailed;
							s.result[i].window.scanned = true;
							s.inputsDone++;
						}
						finishJob(pl);
					} else {
						s.jobInput = 0;
						startInput(pl);
					}
				}
				break;
			case Phase::Trial:
				if (s.trialDead || s.reconverged || s.runDone || s.trialCompleted) onTrialDone(pl);
				break;
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
		// The unmodified macro died (survey, advance or verify): it isn't a valid run.
		uint32_t tick = pl->m_gameState.m_currentProgress;
		Loader::get()->queueInMainThread([tick] {
			abort(fmt::format("the unmodified macro died at tick {}", tick).c_str());
		});
		return true;
	}

	bool onLevelComplete() {
		if (!s.active) return false;
		auto pl = PlayLayer::get();
		uint32_t tick = pl ? pl->m_gameState.m_currentProgress : 0;
		switch (s.phase) {
			case Phase::Survey:
				s.levelEnded = true;
				return true;
			case Phase::Trial:
				s.trialCompleted = true;
				s.completeTick = tick;
				return true;
			case Phase::Verify:
				s.trialCompleted = true; // the replay ended where the reference ended
				return true;
			case Phase::Advance:
				Loader::get()->queueInMainThread([] { abort("the level ended while advancing"); });
				return true;
		}
		return true;
	}
}
