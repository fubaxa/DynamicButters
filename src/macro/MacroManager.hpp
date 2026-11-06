#pragma once

#include "macro/Macro.hpp"

class PlayerObject;
class PlayLayer;
class GJGameLevel;

namespace db {
	enum class MacroMode { Off, Record };

	// Owns the current macro and the record/playback state machine (playback comes in M3).
	// Hooks call into this; it never touches GD directly except to read player state.
	class MacroManager {
	public:
		static MacroManager& get();

		MacroMode mode() const { return m_mode; }
		bool isRecording() const { return m_mode == MacroMode::Record; }
		Macro const& macro() const { return m_macro; }

		// True while the mod needs vanilla Click Between Steps off (recording, later playback),
		// or when the debug setting asks for it.
		bool shouldForceCbsOff() const;

		// --- called from hooks ---
		void onLevelStart(GJGameLevel* level);               // PlayLayer::init (before the original)
		void onAttemptStart(PlayLayer* pl, uint32_t tick, bool practice); // after resetLevel
		void onInput(uint32_t tick, bool down, int button, bool player2, PlayerObject* player);
		void onLevelComplete(PlayLayer* pl, uint32_t tick);
		void onQuit();

	private:
		MacroManager() = default;
		void readModeSetting();
		void recomputeHeld();
		void writeDump(char const* reason) const;

		MacroMode m_mode = MacroMode::Off;
		Macro m_macro;
		bool m_held[2] = {false, false}; // jump held per key (P1, P2), used to drop duplicate inputs
		uint32_t m_dropped = 0;          // duplicate inputs dropped this attempt (diagnostic)
	};
}
