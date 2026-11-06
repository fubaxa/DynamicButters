#pragma once

#include "macro/Macro.hpp"

#include <map>
#include <optional>

class PlayerObject;
class PlayLayer;
class GJGameLevel;
class GJBaseGameLayer;

namespace db {
	enum class MacroMode { Off, Record, Playback };

	// Owns the current macro and the record/playback state machine.
	// Hooks call into this; it only touches GD to read player state and, in playback,
	// to apply inputs (handleButton) and physics corrections.
	class MacroManager {
	public:
		static MacroManager& get();

		MacroMode mode() const { return m_mode; }
		bool isRecording() const { return m_mode == MacroMode::Record; }
		bool isPlaying() const { return m_mode == MacroMode::Playback && m_play.has_value(); }
		Macro const& macro() const { return m_macro; }

		// True while the mod needs vanilla Click Between Steps off (recording, playback),
		// or when the debug setting asks for it.
		bool shouldForceCbsOff() const;

		// --- called from hooks ---
		void onLevelStart(GJGameLevel* level);                            // PlayLayer::init (before the original)
		void onAttemptStart(PlayLayer* pl, uint32_t tick, bool practice); // after resetLevel
		void onInput(uint32_t tick, bool down, int button, bool player2, PlayerObject* player);
		void onLevelComplete(PlayLayer* pl, uint32_t tick);
		void onQuit();

		// --- playback ---
		// Should an input that did not come from the macro be dropped? (Player's own clicks.)
		bool shouldBlockExternalInput(bool down, bool player2) const;
		// Apply every macro input due at `tick`. `late` = called after the step's physics
		// (fallback when processQueuedButtons did not run this step).
		void dispatchDue(GJBaseGameLayer* gl, uint32_t tick, bool late);
		bool isDispatching() const { return m_dispatching; }
		bool isMacroHolding() const { return isPlaying() && (m_playHeld[0] || m_playHeld[1]); }
		// A button is held according to the macro (playback) or the recording (record mode).
		bool isHoldingAny() const {
			if (isPlaying()) return m_playHeld[0] || m_playHeld[1];
			if (isRecording()) return m_held[0] || m_held[1];
			return false;
		}

	private:
		MacroManager() = default;
		void readModeSetting();
		void recomputeHeld();
		void writeDump(char const* reason) const;
		std::optional<Macro> loadLatestDump(int levelID) const;
		void logPlaybackSummary(char const* reason);

		MacroMode m_mode = MacroMode::Off;

		// recording
		Macro m_macro;
		bool m_held[2] = {false, false}; // jump held per key (P1, P2), used to drop duplicate inputs
		uint32_t m_dropped = 0;          // duplicate inputs dropped this attempt (diagnostic)

		// last completed recording per level, so Playback works right after recording
		std::map<int, Macro> m_completed;

		// playback
		std::optional<Macro> m_play;
		size_t m_cursor = 0;
		bool m_playHeld[2] = {false, false};
		bool m_dispatching = false;
		struct PlayStats {
			uint32_t dispatched = 0;
			uint32_t late = 0;
			uint32_t corrected = 0;
			double maxDx = 0, maxDy = 0, maxDyVel = 0, maxDrot = 0;
			uint32_t maxAtTick = 0;
		} m_stats;
	};
}
