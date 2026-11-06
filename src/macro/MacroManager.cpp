#include "macro/MacroManager.hpp"

#include <Geode/Geode.hpp>
#include <Geode/utils/file.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>

#include "debug/Settings.hpp"
#include "macro/MacroStore.hpp"
#include "physics/PlayerState.hpp"

using namespace geode::prelude;

namespace db {
	namespace {
		char const* modeName(MacroMode m) {
			switch (m) {
				case MacroMode::Record: return "Record";
				case MacroMode::Playback: return "Playback";
				default: return "Off";
			}
		}

		double angleDiff(double a, double b) {
			double d = std::fmod(std::abs(a - b), 360.0);
			return std::min(d, 360.0 - d);
		}
	}

	MacroManager& MacroManager::get() {
		static MacroManager instance;
		return instance;
	}

	bool MacroManager::shouldForceCbsOff() const {
		return m_mode != MacroMode::Off || m_seeking || settings::forceDisableCbs();
	}

	namespace {
		MacroMode modeFromString(std::string const& value) {
			return value == "Record" ? MacroMode::Record
				: value == "Playback" ? MacroMode::Playback
				: MacroMode::Off;
		}
	}

	MacroMode MacroManager::pendingMode() const {
		return modeFromString(Mod::get()->getSettingValue<std::string>("macro-mode"));
	}

	void MacroManager::setPendingMode(MacroMode mode) {
		Mod::get()->setSettingValue<std::string>("macro-mode", modeName(mode));
	}

	void MacroManager::selectFile(std::string const& path) {
		m_selectedPath = path;
		m_reloadPending = true;
		log::info("macro: selected {} for playback (applies at the next attempt)", store::fileNameOf(path));
	}

	void MacroManager::clearSelection() {
		if (m_selectedPath.empty()) return;
		m_selectedPath.clear();
		m_reloadPending = true;
	}

	void MacroManager::applyPending() {
		auto mode = pendingMode();
		if (mode == m_mode && !m_reloadPending) return;
		if (mode != m_mode) log::info("macro: mode {} -> {}", modeName(m_mode), modeName(mode));
		m_reloadPending = false;
		m_mode = mode;
		m_play.reset();
		m_playSource.clear();
		m_playPath.clear();
		if (m_mode == MacroMode::Playback || m_seeking) loadPlayback();
	}

	void MacroManager::loadPlayback() {
		auto const& id = m_macro.levelID;
		auto const& name = m_macro.levelName;
		if (!m_selectedPath.empty()) {
			auto res = store::load(m_selectedPath);
			if (res) {
				m_play = std::move(res).unwrap();
				m_playSource = store::fileNameOf(m_selectedPath);
				m_playPath = m_selectedPath;
				if (!m_play->isFor(id, name)) {
					log::warn("playback: {} was recorded on level {} '{}', not this one ({} '{}')",
						m_playSource, m_play->levelID, m_play->levelName, id, name);
				}
				log::info("playback: using selected file {} ({} inputs, bot {})",
					m_playSource, m_play->inputs.size(), m_play->botName);
				return;
			}
			log::error("playback: could not load {}: {}; using the newest recording instead",
				m_selectedPath, res.unwrapErr());
		}
		if (m_lastRecording && m_lastRecording->isFor(id, name)) {
			m_play = m_lastRecording;
			m_playSource = m_lastSaved.empty() ? "this session's recording (not saved)" : m_lastSaved;
			m_playPath = m_lastSavedPath;
			log::info("playback: using the recording made this session ({} inputs)", m_play->inputs.size());
			return;
		}
		if (auto entry = store::latestFor(id, name)) {
			m_play = std::move(entry->macro);
			m_playSource = entry->fileName;
			m_playPath = entry->path;
			log::info("playback: loaded {} ({} inputs, bot {})", entry->fileName, m_play->inputs.size(), m_play->botName);
			return;
		}
		log::warn("playback: no recording found for level {} '{}', playback is inactive", id, name);
	}

	void MacroManager::onLevelStart(GJGameLevel* level) {
		m_macro = Macro{};
		m_macro.levelID = level->m_levelID.value();
		m_macro.levelName = level->m_levelName;
		m_held[0] = m_held[1] = false;
		m_dropped = 0;
		// Force a fresh mode read (and playback load) for this level.
		m_mode = MacroMode::Off;
		m_play.reset();
		m_playSource.clear();
		m_playPath.clear();
		m_selectedPath.clear();
		m_reloadPending = true;
		m_stats = {};
		m_cursor = 0;
		applyPending();
	}

	void MacroManager::onAttemptStart(PlayLayer* pl, uint32_t tick, bool practice) {
		// A fresh attempt starts at tick 0, or at a level start position (StartPosObject) when it
		// isn't a practice respawn at a checkpoint.
		bool atStartPos = pl && pl->m_startPosObject;
		bool fromCheckpoint = practice && pl && pl->m_checkpointArray && pl->m_checkpointArray->count() > 0;
		bool fresh = tick == 0 || (atStartPos && !fromCheckpoint);

		// Mode changes apply here, at the start of an attempt, so an attempt is never half recorded/played.
		if (fresh) {
			if (isPlaying() && m_stats.dispatched > 0) logPlaybackSummary("attempt ended");
			applyPending();
		}

		if (isPlaying()) {
			auto& in = m_play->inputs;
			if (fresh) {
				m_stats = {};
				m_tickOffset = 0;
				m_startPosAttempt = atStartPos;
				m_startPress[0] = m_startPress[1] = false;
				m_pathBuilder.reset();
				if (atStartPos) setupStartPos(pl, tick);
				else if (pl && pl->m_player1) m_pathBuilder.add(tick, pl->m_player1->m_position.x);
			} else if (tracksPath() && pl && pl->m_player1) {
				m_pathBuilder.cutBack(tick, pl->m_player1->m_position.x);
			}
			int64_t macroTick = int64_t(tick) + m_tickOffset;
			if (fresh && !atStartPos) {
				m_cursor = 0;
			} else {
				// Continue with the first input after this point (checkpoint or start position).
				m_cursor = std::upper_bound(in.begin(), in.end(), macroTick,
					[](int64_t t, MacroInput const& mi) { return t < int64_t(mi.frame); }) - in.begin();
			}
			m_playHeld[0] = m_playHeld[1] = false;
			for (size_t i = 0; i < m_cursor; ++i) m_playHeld[in[i].player2 ? 1 : 0] = in[i].down;
			if (fresh && atStartPos) {
				// The macro is holding at the start position: press right away (dispatchDue).
				for (int k = 0; k < 2; ++k) m_startPress[k] = m_playHeld[k];
			} else if (!fresh && (m_playHeld[0] || m_playHeld[1])) {
				log::warn("playback: macro was holding a button at checkpoint tick {}; may desync", tick);
			}
			log::info("playback: attempt from tick {} (macro tick {}, practice={}{}), next input #{} of {}",
				tick, macroTick, practice, atStartPos ? ", start position" : "", m_cursor, in.size());
			return;
		}
		if (atStartPos && fresh && isRecording()) {
			log::warn("record: recording from a level start position isn't supported; the recording "
				"would start mid-level and can't be played from the start");
		}

		if (!isRecording()) return;

		if (tick == 0) {
			// Fresh attempt from the start (normal mode, or practice with no checkpoints).
			if (!m_macro.inputs.empty()) {
				log::info("record: new attempt, discarding {} inputs from the previous one", m_macro.inputs.size());
			}
			m_macro.clear();
			m_held[0] = m_held[1] = false;
			m_dropped = 0;
			m_startPosAttempt = false;
			m_pathBuilder.reset();
			if (pl && pl->m_player1) m_pathBuilder.add(tick, pl->m_player1->m_position.x);
			return;
		}

		// Practice respawn: drop everything recorded after the checkpoint.
		if (pl && pl->m_player1) m_pathBuilder.cutBack(tick, pl->m_player1->m_position.x);
		auto before = m_macro.inputs.size();
		std::erase_if(m_macro.inputs, [tick](MacroInput const& in) { return in.frame > tick; });
		m_macro.endFrame = 0;
		recomputeHeld();
		m_respawnRelease = {};
		log::info("record: respawn at tick {} (practice={}), removed {} inputs, {} left",
			tick, practice, before - m_macro.inputs.size(), m_macro.inputs.size());

		// A key was held at the checkpoint. After a respawn GD always releases the player at tick + 2,
		// so the recording has to release that key there as well (see emitRespawnRelease).
		if (m_held[0] || m_held[1]) {
			m_respawnRelease.active = true;
			m_respawnRelease.tick = tick + 2;
			m_respawnRelease.fallbackPhys = snapshotOf(pl ? pl->m_player1 : nullptr);
			log::info("record: key held at the checkpoint (P1={} P2={}), release expected at tick {}",
				m_held[0], m_held[1], m_respawnRelease.tick);
		}
	}

	PhysSnapshot MacroManager::snapshotOf(PlayerObject* player) {
		PhysSnapshot s;
		if (!player) return s;
		s.x = player->m_position.x;
		s.y = player->m_position.y;
		s.rotation = player->getRotation();
		s.xVel = player->getCurrentXVelocity();
		s.yVel = player->m_yVelocity;
		return s;
	}

	void MacroManager::emitRespawnRelease(PhysSnapshot const& phys, char const* how) {
		for (int k = 0; k < 2; ++k) {
			if (!m_held[k]) continue;
			MacroInput in;
			in.frame = m_respawnRelease.tick;
			in.button = 1;
			in.down = false;
			in.player2 = k == 1;
			in.phys = phys;
			m_macro.inputs.push_back(in);
			m_held[k] = false;
			log::info("record: added release for key {} at tick {} ({})", k == 1 ? "P2" : "P1", in.frame, how);
		}
		m_respawnRelease.active = false;
	}

	bool MacroManager::tracksPath() const {
		return isRecording() || (isPlaying() && !m_startPosAttempt);
	}

	void MacroManager::onStepEnd(uint32_t tick, PlayerObject* player) {
		if (player && tracksPath()) m_pathBuilder.add(tick, player->m_position.x);
		if (!isRecording() || !m_respawnRelease.active) return;
		if (tick < m_respawnRelease.tick) return;
		// GD's own release didn't arrive. Use the respawn state: nothing moves between the respawn
		// and the first step, so it's the state the release is applied to.
		log::warn("record: GD's release after the respawn did not arrive by tick {}", tick);
		emitRespawnRelease(m_respawnRelease.fallbackPhys, "fallback, respawn state");
	}

	void MacroManager::recomputeHeld() {
		m_held[0] = m_held[1] = false;
		for (auto const& in : m_macro.inputs) m_held[in.player2 ? 1 : 0] = in.down;
	}

	void MacroManager::onInput(uint32_t tick, bool down, int button, bool player2, PlayerObject* player) {
		if (!isRecording()) return;
		if (button != 1) return; // platformer left/right: out of scope for now

		// First input at/after the post-respawn tick (normally GD's own release): end the held
		// key(s) here, with the state this input is applied to.
		if (m_respawnRelease.active && tick >= m_respawnRelease.tick) {
			emitRespawnRelease(snapshotOf(player), down ? "before a press" : "GD's respawn release");
		}

		bool& held = m_held[player2 ? 1 : 0];
		if (held == down) {
			// Press while already pressed / release while already released. This also drops GD's
			// own release at the start of every attempt.
			m_dropped++;
			return;
		}
		held = down;

		MacroInput in;
		in.frame = tick;
		in.button = static_cast<uint8_t>(button);
		in.down = down;
		in.player2 = player2;
		in.phys = snapshotOf(player);
		m_macro.inputs.push_back(in);
	}

	void MacroManager::onLevelComplete(PlayLayer* pl, uint32_t tick) {
		if (isPlaying()) {
			logPlaybackSummary("level complete");
			if (m_play->path.empty() && !m_startPosAttempt && !m_pathBuilder.empty()) {
				// A macro without a path (recorded before paths existed, or by another bot) learns it
				// from a complete playback from the start.
				m_play->path = m_pathBuilder.finish();
				log::info("playback: learned the macro's path ({} points)", m_play->path.size());
				if (m_lastRecording && m_lastRecording->recordedAt == m_play->recordedAt
					&& m_lastRecording->isFor(m_play->levelID, m_play->levelName)) {
					m_lastRecording->path = m_play->path;
				}
				if (!m_playPath.empty()) {
					if (auto res = store::overwrite(m_playPath, *m_play); !res) {
						log::error("playback: saving the path to {} failed: {}", m_playPath, res.unwrapErr());
					} else {
						log::info("playback: saved the path to {}", store::fileNameOf(m_playPath));
					}
				}
			}
			return;
		}
		if (!isRecording()) return;
		m_macro.endFrame = tick;
		m_macro.path = m_pathBuilder.finish();
		uint32_t presses = 0;
		for (auto const& in : m_macro.inputs) presses += in.down;
		log::info("record: level complete at tick {} ({:.2f} s): {} inputs ({} presses), {} duplicates dropped, path {} points",
			tick, tick / (2.0 * m_macro.framerate), m_macro.inputs.size(), presses, m_dropped, m_macro.path.size());
		saveRecording();
	}

	void MacroManager::onQuit() {
		if (isPlaying() && m_stats.dispatched > 0) logPlaybackSummary("left the level");
		if (isRecording() && !m_macro.inputs.empty() && m_macro.endFrame == 0) {
			log::info("record: left the level without completing it ({} inputs, not saved)", m_macro.inputs.size());
		}
	}

	// ---------------------------------------------------------------- playback

	bool MacroManager::shouldBlockExternalInput(bool down, bool player2) const {
		if (!isPlaying()) return false;
		// Block every press that isn't the macro's. Let a release through only if the macro isn't
		// holding that key: that keeps GD's own release at the start of each attempt (which also
		// happened while recording), but stops a stray key release from cutting a macro hold short.
		if (down) return true;
		return m_playHeld[player2 ? 1 : 0];
	}

	void MacroManager::dispatchDue(GJBaseGameLayer* gl, uint32_t tick, bool late) {
		if (!isPlaying() || !gl) return;
		auto const& inputs = m_play->inputs;
		bool twoPlayer = gl->m_levelSettings && gl->m_levelSettings->m_twoPlayerMode;
		bool correct = settings::physicsCorrection();
		bool logEach = settings::logInputs();

		// Start position where the macro is mid-hold: press first.
		for (int k = 0; k < 2; ++k) {
			if (!m_startPress[k]) continue;
			m_startPress[k] = false;
			m_playHeld[k] = true;
			log::info("playback: start position is inside a hold, pressing key {} at tick {}", k ? "P2" : "P1", tick);
			m_dispatching = true;
			gl->handleButton(true, 1, k == 0);
			m_dispatching = false;
		}

		int64_t macroTick = int64_t(tick) + m_tickOffset;
		while (m_cursor < inputs.size() && int64_t(inputs[m_cursor].frame) <= macroTick) {
			auto const& mi = inputs[m_cursor++];
			if (late || int64_t(mi.frame) < macroTick) {
				m_stats.late++;
				log::warn("playback: input #{} for tick {} applied late at tick {} ({})",
					m_cursor - 1, mi.frame, tick, late ? "after the step" : "missed step");
			}

			PlayerObject* p = (!mi.player2 || !twoPlayer) ? gl->m_player1 : gl->m_player2;
			// Recordings are made at 1 step per frame (240 FPS) where every input is on the first
			// step of its frame. On a later step of a multi-step frame (speedhack > 1x, frame drops)
			// the player's position at this point reads one step off even though the run is
			// identical (M8a: the next input is back to 0 drift). So those inputs are neither measured
			// nor corrected; correcting them would move a correctly placed player.
			unsigned step = runtime::stepInFrame;
			bool measurable = step <= 1;
			if (!measurable) m_stats.unmeasured++;
			if (p) {
				double dx = std::abs(p->m_position.x - mi.phys.x);
				double dy = std::abs(p->m_position.y - mi.phys.y);
				double dyv = std::abs(p->m_yVelocity - mi.phys.yVel);
				double drot = angleDiff(p->getRotation(), mi.phys.rotation);
				if (measurable) {
					if (dx > m_stats.maxDx) { m_stats.maxDx = dx; m_stats.maxAtTick = mi.frame; }
					m_stats.maxDy = std::max(m_stats.maxDy, dy);
					m_stats.maxDyVel = std::max(m_stats.maxDyVel, dyv);
					m_stats.maxDrot = std::max(m_stats.maxDrot, drot);
				}
				if (logEach) {
					log::info("play: {} key={} tick={} step={} drift dx={:.6f} dy={:.6f} dyVel={:.6f} drot={:.4f}{}",
						mi.down ? "PRESS  " : "RELEASE", mi.player2 ? "P2" : "P1", mi.frame, step,
						dx, dy, dyv, drot,
						!measurable ? " (not measured: later step of frame)" : correct ? " (corrected)" : "");
				}
				if (correct && measurable) {
					cocos2d::CCPoint pos{mi.phys.x, mi.phys.y};
					p->m_position = pos;
					p->setPosition(pos);
					p->setRotation(mi.phys.rotation);
					p->m_yVelocity = mi.phys.yVel;
					m_stats.corrected++;
				}
			}

			m_playHeld[mi.player2 ? 1 : 0] = mi.down;
			m_dispatching = true;
			gl->handleButton(mi.down, mi.button, !mi.player2);
			m_dispatching = false;
			m_stats.dispatched++;
		}
	}

	void MacroManager::logPlaybackSummary(char const* reason) {
		if (!m_play) return;
		log::info(
			"playback: {} - {} of {} inputs applied, {} late, {} corrected, {} not measured; max drift before correction "
			"dx={:.6f} (at tick {}) dy={:.6f} dyVel={:.6f} drot={:.4f}",
			reason, m_stats.dispatched, m_play->inputs.size(), m_stats.late, m_stats.corrected, m_stats.unmeasured,
			m_stats.maxDx, m_stats.maxAtTick, m_stats.maxDy, m_stats.maxDyVel, m_stats.maxDrot);
	}

	// ---------------------------------------------------------------- level start positions

	// Tick at which the recorded run passed `x`, interpolated between the two recorded inputs around
	// it (positions are stored with every input). Assumes constant speed between those two inputs;
	// before the first input the run starts at x = 0, tick 0.
	std::optional<uint32_t> MacroManager::macroTickAtX(float x) const {
		if (!m_play) return std::nullopt;
		double prevT = 0.0, prevX = 0.0;
		for (auto const& in : m_play->inputs) {
			double t = in.frame, px = in.phys.x;
			if (px >= x && px > prevX) {
				double tick = prevT + (x - prevX) * (t - prevT) / (px - prevX);
				log::info("startpos: x={:.3f} lies between ticks {} (x={:.3f}) and {} (x={:.3f}), {:.4f} x/tick",
					x, prevT, prevX, t, px, (px - prevX) / std::max(1.0, t - prevT));
				return static_cast<uint32_t>(std::max(0.0, std::round(tick / 2.0) * 2.0));
			}
			if (px > prevX) { prevT = t; prevX = px; }
		}
		return std::nullopt; // start position is past the macro's last input
	}

	// The start position is trusted to put the player in the same state as a real playthrough passing
	// that point (they're normally placed where the player is stable, e.g. landed on a block). Only
	// the time is unknown: the macro's path gives the exact tick the recorded run passed the spawn x.
	// The player is nudged by less than a step so x matches that tick exactly; nothing else is changed.
	void MacroManager::setupStartPos(PlayLayer* pl, uint32_t tick) {
		auto p = pl->m_player1;
		// The start position object's own x: GD reuses the first spawn's state for later attempts,
		// so the player's x would already include an earlier nudge.
		float spawnX = p ? p->m_position.x : 0.f;
		float x = pl->m_startPosObject ? pl->m_startPosObject->getPositionX() : spawnX;
		log::info("startpos: attempt at start position x={:.3f} (player spawned at x={:.3f}), game tick {}, levelTime {:.4f}",
			x, spawnX, tick, pl->m_gameState.m_levelTime);

		auto const& pathPts = m_play->path;
		if (!pathPts.empty()) {
			if (auto exact = path::tickAtX(pathPts, x)) {
				// Nearest physics step (even tick) to where the run was exactly at x.
				uint32_t lo = static_cast<uint32_t>(std::floor(*exact / 2.0)) * 2;
				uint32_t best = lo;
				double bestErr = 1e9;
				for (uint32_t cand : {lo, lo + 2}) {
					if (auto cx = path::xAt(pathPts, cand)) {
						double err = std::abs(*cx - x);
						if (err < bestErr) { bestErr = err; best = cand; }
					}
				}
				auto runX = path::xAt(pathPts, best);
				m_tickOffset = int64_t(best) - int64_t(tick);
				log::info("startpos: recorded run passed x={:.3f} at tick {:.2f}; using step tick {} ({:.2f} s), offset {}",
					x, *exact, best, best / 480.0, m_tickOffset);
				if (runX && p) {
					double nudge = *runX - spawnX;
					if (std::abs(nudge) < 5.0) {
						p->m_position.x = static_cast<float>(*runX);
						p->setPositionX(static_cast<float>(*runX));
						log::info("startpos: nudged x by {:.4f} to {:.3f} (the run's x at tick {})", nudge, *runX, best);
					} else {
						log::warn("startpos: run's x at tick {} is {:.3f}, {:.3f} away; not nudging", best, *runX, nudge);
					}
				}
				return;
			}
			log::warn("startpos: x={:.3f} isn't on the macro's path", x);
		} else {
			log::warn("startpos: this macro has no path yet (play it once from the start in Playback to "
				"learn it); estimating from the inputs, which can be off by many ticks");
		}

		auto macroTick = macroTickAtX(x);
		if (!macroTick) {
			log::warn("startpos: the macro never reaches x={:.3f}; nothing to play", x);
			m_tickOffset = int64_t(m_play->lastInputFrame()) + 2 - int64_t(tick);
			return;
		}
		m_tickOffset = int64_t(*macroTick) - int64_t(tick);
		log::info("startpos: estimated macro tick {} ({:.2f} s) at the spawn, offset {}", *macroTick, *macroTick / 480.0, m_tickOffset);
	}

	// ---------------------------------------------------------------- start positions / seek (M8)

	bool MacroManager::ensurePlayMacro() {
		if (!m_play) loadPlayback();
		return m_play.has_value();
	}

	void MacroManager::saveStartTicks() {
		if (!m_play) return;
		auto& ticks = m_play->startTicks;
		std::sort(ticks.begin(), ticks.end());
		ticks.erase(std::unique(ticks.begin(), ticks.end()), ticks.end());
		if (m_lastRecording && m_lastRecording->isFor(m_play->levelID, m_play->levelName)
			&& m_lastRecording->recordedAt == m_play->recordedAt) {
			m_lastRecording->startTicks = ticks;
		}
		if (m_playPath.empty()) {
			log::warn("startpos: macro has no file, start positions are kept for this session only");
			return;
		}
		if (auto res = store::overwrite(m_playPath, *m_play); !res) {
			log::error("startpos: saving {} failed: {}", m_playPath, res.unwrapErr());
		} else {
			log::info("startpos: saved {} start positions to {}", ticks.size(), store::fileNameOf(m_playPath));
		}
	}

	void MacroManager::addStartTick(uint32_t tick) {
		if (!m_play) return;
		m_play->startTicks.push_back(tick);
		saveStartTicks();
	}

	void MacroManager::removeStartTick(uint32_t tick) {
		if (!m_play) return;
		std::erase(m_play->startTicks, tick);
		saveStartTicks();
	}

	void MacroManager::clearStartTicks() {
		if (!m_play) return;
		m_play->startTicks.clear();
		saveStartTicks();
	}

	void MacroManager::beginSeek() {
		m_seeking = true;
		m_stats = {};
	}

	void MacroManager::endSeek(bool reached, uint32_t tick) {
		m_seeking = false;
		if (!reached || !isRecording() || !m_play) return;
		// Re-record from a start position: the recording starts as the macro up to that tick.
		m_macro.clear();
		for (auto const& in : m_play->inputs) {
			if (in.frame > tick) break;
			m_macro.inputs.push_back(in);
		}
		recomputeHeld();
		m_dropped = 0;
		m_respawnRelease = {};
		log::info("record: continuing from start position tick {} with the macro's {} inputs before it",
			tick, m_macro.inputs.size());
	}

	// ---------------------------------------------------------------- saving

	void MacroManager::saveRecording() {
		m_macro.recordedAt = std::chrono::duration_cast<std::chrono::seconds>(
			std::chrono::system_clock::now().time_since_epoch()).count();
		if (auto gm = GameManager::get()) m_macro.author = gm->m_playerName;
		m_lastRecording = m_macro;
		m_lastSaved.clear();
		m_lastSavedPath.clear();

		auto res = store::save(m_macro);
		if (!res) {
			log::error("record: saving failed: {}", res.unwrapErr());
			Notification::create("DynamicButter: saving the macro failed (see log)", NotificationIcon::Error)->show();
			return;
		}
		m_lastSaved = store::fileNameOf(res.unwrap());
		m_lastSavedPath = res.unwrap();
		log::info("record: saved {}", res.unwrap());
		Notification::create(fmt::format("Macro saved: {}", m_lastSaved), NotificationIcon::Success)->show();
	}
}
