#pragma once

#include "macro/Macro.hpp"

#include <optional>
#include <string>
#include <vector>

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
		// A seek (M8) plays the macro whatever the mode is, and nothing is recorded while it runs.
		bool isRecording() const { return m_mode == MacroMode::Record && !m_seeking; }
		bool isPlaying() const { return m_play.has_value() && (m_mode == MacroMode::Playback || m_seeking); }
		bool isSeeking() const { return m_seeking; }
		Macro const& macro() const { return m_macro; }

		// --- pause-menu popup ---
		// Mode from the setting; it becomes the active mode() at the start of the next attempt.
		MacroMode pendingMode() const;
		void setPendingMode(MacroMode mode);
		// Play this file instead of the newest recording of the level (applies at the next attempt).
		void selectFile(std::string const& path);
		void clearSelection();
		std::string const& selectedPath() const { return m_selectedPath; }
		std::optional<Macro> const& playMacro() const { return m_play; }
		std::string const& playSource() const { return m_playSource; }
		std::string const& lastSaved() const { return m_lastSaved; }
		bool hasPendingChange() const { return m_reloadPending || pendingMode() != m_mode; }

		// --- macro start positions (M8) ---
		// Loads the macro to play (selected file, this session's recording, or the newest file for
		// the level) if it isn't loaded yet. False if there is none.
		bool ensurePlayMacro();
		// Start positions of the loaded macro; edits are saved to its file right away.
		std::vector<uint32_t> const* startTicks() const { return m_play ? &m_play->startTicks : nullptr; }
		void addStartTick(uint32_t tick);
		void removeStartTick(uint32_t tick);
		void clearStartTicks();
		// Seek: macro playback runs regardless of mode; recording pauses.
		void beginSeek();
		// Seek reached `tick`. In Record mode the recording continues from there, starting with the
		// macro's inputs up to `tick`.
		void endSeek(bool reached, uint32_t tick);

		// --- Dynamic window scan ---
		// Plays these inputs instead of the macro's (one click moved) until restoreInputs().
		void setTrialInputs(std::vector<MacroInput> inputs);
		void restoreInputs();
		// Scan mode: no physics correction and no per-input / per-attempt logs.
		void setQuiet(bool quiet) { m_quiet = quiet; }
		bool quiet() const { return m_quiet; }
		// Stores the scanned inputs (with their windows) in the loaded macro and saves its file.
		void saveWindows(std::vector<MacroInput> scanned);
		std::string const& playPath() const { return m_playPath; }

		// True while the mod needs vanilla Click Between Steps off (recording, playback),
		// or when the debug setting asks for it.
		bool shouldForceCbsOff() const;

		// --- called from hooks ---
		void onLevelStart(GJGameLevel* level);                            // PlayLayer::init (before the original)
		void onAttemptStart(PlayLayer* pl, uint32_t tick, bool practice); // after resetLevel
		void onInput(uint32_t tick, bool down, int button, bool player2, PlayerObject* player);
		void onLevelComplete(PlayLayer* pl, uint32_t tick);
		void onQuit();
		void onStepEnd(uint32_t tick, PlayerObject* player);              // end of every processCommands

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
		void applyPending();
		void loadPlayback();
		void recomputeHeld();
		void saveRecording();
		void saveStartTicks();
		bool m_seeking = false;
		bool m_quiet = false;
		std::optional<std::vector<MacroInput>> m_savedInputs; // macro's own inputs during a scan trial
		std::string m_playPath; // file m_play was loaded from / saved to (empty: not saved)
		void logPlaybackSummary(char const* reason);

		MacroMode m_mode = MacroMode::Off;

		// recording
		Macro m_macro;
		bool m_held[2] = {false, false}; // jump held per key (P1, P2), used to drop duplicate inputs
		uint32_t m_dropped = 0;          // duplicate inputs dropped this attempt (diagnostic)

		// Practice respawn while a key is held: GD releases the player at respawn tick + 2 (its own
		// release, always on the P1 key). The recording must release the held key(s) at that tick too.
		struct RespawnRelease {
			bool active = false;
			uint32_t tick = 0;
			PhysSnapshot fallbackPhys; // respawn state, used only if GD's release never arrives
		} m_respawnRelease;
		void emitRespawnRelease(PhysSnapshot const& phys, char const* how);
		static PhysSnapshot snapshotOf(PlayerObject* player);

		// last completed recording this session, so Playback works right after recording even if saving failed
		std::optional<Macro> m_lastRecording;
		std::string m_lastSaved; // file name of the last saved recording
		std::string m_lastSavedPath;

		std::string m_selectedPath; // picked in the popup; empty = newest recording of the level
		bool m_reloadPending = false;
		std::string m_playSource;   // where m_play came from, for the popup

		// playback
		std::optional<Macro> m_play;
		size_t m_cursor = 0;
		bool m_playHeld[2] = {false, false};
		bool m_dispatching = false;

		// Level start positions (StartPosObject): the attempt starts mid-level, so the macro is
		// shifted to the tick where the recorded run passed the spawn x. macro tick = game tick + offset.
		int64_t m_tickOffset = 0;
		bool m_startPosAttempt = false;        // this attempt began at a level start position
		bool m_startPress[2] = {false, false}; // macro holds these keys at the start position: press at once
		std::optional<uint32_t> macroTickAtX(float x) const; // fallback for macros without a path
		void setupStartPos(PlayLayer* pl, uint32_t tick);

		// The run's x path (PathTrack.hpp): built while recording, and while playing from the start
		// (a macro without a path learns it when such a playback completes).
		PathBuilder m_pathBuilder;
		bool tracksPath() const;

		struct PlayStats {
			uint32_t dispatched = 0;
			uint32_t late = 0;
			uint32_t corrected = 0;
			uint32_t unmeasured = 0; // inputs on a later step of a multi-step frame (see dispatchDue)
			double maxDx = 0, maxDy = 0, maxDyVel = 0, maxDrot = 0;
			uint32_t maxAtTick = 0;
		} m_stats;
	};
}
