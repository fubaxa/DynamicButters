#include "macro/MacroManager.hpp"

#include <Geode/Geode.hpp>
#include <Geode/utils/file.hpp>

#include <algorithm>
#include <chrono>

#include "debug/Settings.hpp"

using namespace geode::prelude;

namespace db {
	MacroManager& MacroManager::get() {
		static MacroManager instance;
		return instance;
	}

	bool MacroManager::shouldForceCbsOff() const {
		return m_mode != MacroMode::Off || settings::forceDisableCbs();
	}

	void MacroManager::readModeSetting() {
		auto value = Mod::get()->getSettingValue<std::string>("macro-mode");
		auto mode = value == "Record" ? MacroMode::Record : MacroMode::Off;
		if (mode != m_mode) {
			log::info("macro: mode {} -> {}", m_mode == MacroMode::Record ? "Record" : "Off", value);
			m_mode = mode;
		}
	}

	void MacroManager::onLevelStart(GJGameLevel* level) {
		readModeSetting();
		m_macro = Macro{};
		m_macro.levelID = level->m_levelID.value();
		m_macro.levelName = level->m_levelName;
		m_held[0] = m_held[1] = false;
		m_dropped = 0;
	}

	void MacroManager::onAttemptStart(PlayLayer* pl, uint32_t tick, bool practice) {
		// Mode changes apply here, at the start of an attempt, so an attempt is never half recorded.
		if (tick == 0) readModeSetting();
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
		// (Re-pressing a button held at the checkpoint, and exact state restore, are M4/M5.)
		auto before = m_macro.inputs.size();
		std::erase_if(m_macro.inputs, [tick](MacroInput const& in) { return in.frame > tick; });
		m_macro.endFrame = 0;
		recomputeHeld();
		log::info("record: respawn at tick {} (practice={}), removed {} inputs, {} left",
			tick, practice, before - m_macro.inputs.size(), m_macro.inputs.size());
	}

	void MacroManager::recomputeHeld() {
		m_held[0] = m_held[1] = false;
		for (auto const& in : m_macro.inputs) m_held[in.player2 ? 1 : 0] = in.down;
	}

	void MacroManager::onInput(uint32_t tick, bool down, int button, bool player2, PlayerObject* player) {
		if (!isRecording()) return;
		if (button != 1) return; // platformer left/right: out of scope for now

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
		if (player) {
			in.phys.x = player->m_position.x;
			in.phys.y = player->m_position.y;
			in.phys.rotation = player->getRotation();
			in.phys.xVel = player->getCurrentXVelocity();
			in.phys.yVel = player->m_yVelocity;
		}
		m_macro.inputs.push_back(in);
	}

	void MacroManager::onLevelComplete(PlayLayer* pl, uint32_t tick) {
		if (!isRecording()) return;
		m_macro.endFrame = tick;
		uint32_t presses = 0;
		for (auto const& in : m_macro.inputs) presses += in.down;
		log::info("record: level complete at tick {} ({:.2f} s): {} inputs ({} presses), {} duplicates dropped",
			tick, tick / (2.0 * m_macro.framerate), m_macro.inputs.size(), presses, m_dropped);
		writeDump("complete");
	}

	void MacroManager::onQuit() {
		if (isRecording() && !m_macro.inputs.empty() && m_macro.endFrame == 0) {
			log::info("record: left the level without completing it ({} inputs, not saved)", m_macro.inputs.size());
		}
	}

	// Temporary human-readable dump so M2 recordings can be checked by eye.
	// Replaced by real .gdr2 save/load in M6.
	void MacroManager::writeDump(char const* reason) const {
		auto dir = Mod::get()->getSaveDir() / "recordings";
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
			out += fmt::format("{:>6}  {:<7}  {}   {:<12.6f} {:<12.6f} {:<10.4f} {:<11.6f} {:.6f}\n",
				in.frame, in.down ? "PRESS" : "RELEASE", in.player2 ? "P2" : "P1",
				in.phys.x, in.phys.y, in.phys.rotation, in.phys.xVel, in.phys.yVel);
		}

		if (auto res = file::writeString(path, out); !res) {
			log::error("record: could not write {}: {}", path.string(), res.unwrapErr());
			return;
		}
		log::info("record: wrote {}", path.string());
	}
}
