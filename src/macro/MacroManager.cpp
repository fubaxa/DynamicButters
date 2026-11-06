#include "macro/MacroManager.hpp"

#include <Geode/Geode.hpp>
#include <Geode/utils/file.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <sstream>

#include "debug/Settings.hpp"

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

		auto recordingsDir() {
			return Mod::get()->getSaveDir() / "recordings";
		}
	}

	MacroManager& MacroManager::get() {
		static MacroManager instance;
		return instance;
	}

	bool MacroManager::shouldForceCbsOff() const {
		return m_mode != MacroMode::Off || settings::forceDisableCbs();
	}

	void MacroManager::readModeSetting() {
		auto value = Mod::get()->getSettingValue<std::string>("macro-mode");
		auto mode = value == "Record" ? MacroMode::Record
			: value == "Playback" ? MacroMode::Playback
			: MacroMode::Off;
		if (mode == m_mode) return;

		log::info("macro: mode {} -> {}", modeName(m_mode), modeName(mode));
		m_mode = mode;
		m_play.reset();
		if (m_mode == MacroMode::Playback) {
			// Most recent completed recording of this level: from memory, else from disk.
			auto it = m_completed.find(m_macro.levelID);
			if (it != m_completed.end()) {
				m_play = it->second;
				log::info("playback: using the recording made this session ({} inputs)", m_play->inputs.size());
			} else if (auto loaded = loadLatestDump(m_macro.levelID)) {
				m_play = std::move(loaded);
			} else {
				log::warn("playback: no completed recording found for level {}, playback is inactive", m_macro.levelID);
			}
		}
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
		m_stats = {};
		m_cursor = 0;
		readModeSetting();
	}

	void MacroManager::onAttemptStart(PlayLayer* pl, uint32_t tick, bool practice) {
		// Mode changes apply here, at the start of an attempt, so an attempt is never half recorded/played.
		if (tick == 0) {
			if (isPlaying() && m_stats.dispatched > 0) logPlaybackSummary("attempt ended");
			readModeSetting();
		}

		if (isPlaying()) {
			auto& in = m_play->inputs;
			if (tick == 0) {
				m_cursor = 0;
				m_stats = {};
			} else {
				// Practice respawn: continue with the first input after the checkpoint.
				m_cursor = std::upper_bound(in.begin(), in.end(), tick,
					[](uint32_t t, MacroInput const& mi) { return t < mi.frame; }) - in.begin();
			}
			m_playHeld[0] = m_playHeld[1] = false;
			for (size_t i = 0; i < m_cursor; ++i) m_playHeld[in[i].player2 ? 1 : 0] = in[i].down;
			if (tick != 0 && (m_playHeld[0] || m_playHeld[1])) {
				// GD may not treat the button as held after a respawn; handled properly in M5.
				log::warn("playback: macro was holding a button at checkpoint tick {}; may desync (M5)", tick);
			}
			log::info("playback: attempt from tick {} (practice={}), next input #{} of {}",
				tick, practice, m_cursor, in.size());
			return;
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
			return;
		}

		// Practice respawn: drop everything recorded after the checkpoint.
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

	void MacroManager::onStepEnd(uint32_t tick, PlayerObject* player) {
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
			return;
		}
		if (!isRecording()) return;
		m_macro.endFrame = tick;
		uint32_t presses = 0;
		for (auto const& in : m_macro.inputs) presses += in.down;
		log::info("record: level complete at tick {} ({:.2f} s): {} inputs ({} presses), {} duplicates dropped",
			tick, tick / (2.0 * m_macro.framerate), m_macro.inputs.size(), presses, m_dropped);
		m_completed[m_macro.levelID] = m_macro;
		writeDump("complete");
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

		while (m_cursor < inputs.size() && inputs[m_cursor].frame <= tick) {
			auto const& mi = inputs[m_cursor++];
			if (late || mi.frame < tick) {
				m_stats.late++;
				log::warn("playback: input #{} for tick {} applied late at tick {} ({})",
					m_cursor - 1, mi.frame, tick, late ? "after the step" : "missed step");
			}

			PlayerObject* p = (!mi.player2 || !twoPlayer) ? gl->m_player1 : gl->m_player2;
			if (p) {
				double dx = std::abs(p->m_position.x - mi.phys.x);
				double dy = std::abs(p->m_position.y - mi.phys.y);
				double dyv = std::abs(p->m_yVelocity - mi.phys.yVel);
				double drot = angleDiff(p->getRotation(), mi.phys.rotation);
				if (dx > m_stats.maxDx) { m_stats.maxDx = dx; m_stats.maxAtTick = mi.frame; }
				m_stats.maxDy = std::max(m_stats.maxDy, dy);
				m_stats.maxDyVel = std::max(m_stats.maxDyVel, dyv);
				m_stats.maxDrot = std::max(m_stats.maxDrot, drot);
				if (logEach) {
					log::info("play: {} key={} tick={} drift dx={:.6f} dy={:.6f} dyVel={:.6f} drot={:.4f}{}",
						mi.down ? "PRESS  " : "RELEASE", mi.player2 ? "P2" : "P1", mi.frame,
						dx, dy, dyv, drot, correct ? " (corrected)" : "");
				}
				if (correct) {
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
			"playback: {} - {} of {} inputs applied, {} late, {} corrected; max drift before correction "
			"dx={:.6f} (at tick {}) dy={:.6f} dyVel={:.6f} drot={:.4f}",
			reason, m_stats.dispatched, m_play->inputs.size(), m_stats.late, m_stats.corrected,
			m_stats.maxDx, m_stats.maxAtTick, m_stats.maxDy, m_stats.maxDyVel, m_stats.maxDrot);
	}

	// ---------------------------------------------------------------- debug dump (until GDR2 in M6)

	// Temporary human-readable dump so recordings can be checked by eye and replayed after a restart.
	// Replaced by real .gdr2 save/load in M6.
	void MacroManager::writeDump(char const* reason) const {
		auto dir = recordingsDir();
		if (auto res = file::createDirectoryAll(dir); !res) {
			log::error("record: could not create {}: {}", dir.string(), res.unwrapErr());
			return;
		}
		auto stamp = std::chrono::duration_cast<std::chrono::seconds>(
			std::chrono::system_clock::now().time_since_epoch()).count();
		auto path = dir / fmt::format("{}_{}_{}.txt", m_macro.levelID, stamp, reason);

		std::string out;
		out += fmt::format("# DynamicButter recording (M2 debug dump)\n");
		out += fmt::format("# level {} '{}', framerate {}, end tick {}, {} inputs\n",
			m_macro.levelID, m_macro.levelName, m_macro.framerate, m_macro.endFrame, m_macro.inputs.size());
		out += "# tick  action   key  x            y            rot        xVel        yVel\n";
		for (auto const& in : m_macro.inputs) {
			// 9 significant digits round-trip a float exactly; 17 do for a double.
			out += fmt::format("{:>6}  {:<7}  {}   {:<14.9g} {:<14.9g} {:<12.9g} {:<20.17g} {:.17g}\n",
				in.frame, in.down ? "PRESS" : "RELEASE", in.player2 ? "P2" : "P1",
				in.phys.x, in.phys.y, in.phys.rotation, in.phys.xVel, in.phys.yVel);
		}

		if (auto res = file::writeString(path, out); !res) {
			log::error("record: could not write {}: {}", path.string(), res.unwrapErr());
			return;
		}
		log::info("record: wrote {}", path.string());
	}

	std::optional<Macro> MacroManager::loadLatestDump(int levelID) const {
		auto entries = file::readDirectory(recordingsDir());
		if (!entries) return std::nullopt;

		// Files are "<levelID>_<unix seconds>_complete.txt"; pick the newest for this level.
		std::string prefix = fmt::format("{}_", levelID);
		std::string bestPath;
		long long bestStamp = -1;
		for (auto const& entry : entries.unwrap()) {
			auto full = entry.string();
			auto slash = full.find_last_of("/\\");
			auto name = slash == std::string::npos ? full : full.substr(slash + 1);
			if (!name.starts_with(prefix) || !name.ends_with("_complete.txt")) continue;
			auto stampStr = name.substr(prefix.size(), name.size() - prefix.size() - std::string("_complete.txt").size());
			long long stamp = -1;
			try { stamp = std::stoll(stampStr); } catch (...) { continue; }
			if (stamp > bestStamp) { bestStamp = stamp; bestPath = full; }
		}
		if (bestPath.empty()) return std::nullopt;

		auto text = file::readString(bestPath);
		if (!text) {
			log::error("playback: could not read {}: {}", bestPath, text.unwrapErr());
			return std::nullopt;
		}

		Macro macro;
		macro.levelID = levelID;
		std::istringstream lines(text.unwrap());
		std::string line;
		while (std::getline(lines, line)) {
			if (line.empty()) continue;
			if (line[0] == '#') {
				if (auto pos = line.find("end tick "); pos != std::string::npos) {
					macro.endFrame = static_cast<uint32_t>(std::stoul(line.substr(pos + 9)));
				}
				continue;
			}
			std::istringstream row(line);
			MacroInput in;
			std::string action, key;
			if (!(row >> in.frame >> action >> key >> in.phys.x >> in.phys.y >> in.phys.rotation >> in.phys.xVel >> in.phys.yVel)) {
				log::warn("playback: skipping bad line in {}: {}", bestPath, line);
				continue;
			}
			in.down = action == "PRESS";
			in.player2 = key == "P2";
			macro.inputs.push_back(in);
		}
		log::info("playback: loaded {} ({} inputs, end tick {})", bestPath, macro.inputs.size(), macro.endFrame);
		return macro;
	}
}
