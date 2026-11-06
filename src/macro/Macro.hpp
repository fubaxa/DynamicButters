#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "macro/PathTrack.hpp"

namespace db {
	// Player state at the moment an input is applied. Matches GDR2's standard "Phys" extension
	// (xPos, yPos, rotation as float; xVel, yVel as double), so other bots can read it later.
	struct PhysSnapshot {
		float x = 0.f;
		float y = 0.f;
		float rotation = 0.f;
		double xVel = 0.0;
		double yVel = 0.0;
	};

	// Dynamic (window scan): how many physics steps earlier / later this input can happen with
	// the rest of the macro unchanged and the player still surviving past the next input.
	struct InputWindow {
		enum Flags : uint8_t {
			EarlyBound = 1,      // the scan stopped at the previous input (window may be larger)
			LateBound = 2,       // the scan stopped at the next input
			EarlyMax = 4,        // the scan stopped at the scan range
			LateMax = 8,
			BaselineFailed = 16, // the unmodified macro didn't pass from the checkpoint: no window
		};
		bool scanned = false;
		uint8_t early = 0; // steps (1 step = 2 ticks)
		uint8_t late = 0;
		uint8_t flags = 0;
	};

	struct MacroInput {
		uint32_t frame = 0;   // m_gameState.m_currentProgress when GD applied the input (half-step units)
		uint8_t button = 1;   // 1 = jump (platformer left/right not supported yet)
		bool down = false;    // press / release
		bool player2 = false; // the P2 *key* was used (in 1-player levels it still moves player 1)
		PhysSnapshot phys;    // state right before the input was applied
		InputWindow window;   // Dynamic window scan result
	};

	struct Macro {
		int levelID = 0;
		std::string levelName;
		double framerate = 240.0;     // physics steps per second (GDR2 metadata)
		uint32_t endFrame = 0;        // tick at level completion (0 = not completed)
		int seed = 0;                 // GDR2 metadata; not captured yet (see PLAN.md open issues)
		std::string author;
		std::string botName = "DynamicButter"; // bot that recorded it (from the file when loaded)
		int64_t recordedAt = 0;       // unix seconds; 0 = unknown (files from other bots)
		std::vector<MacroInput> inputs; // sorted by frame, P1 and P2 keys interleaved
		std::vector<uint32_t> startTicks; // macro start positions (M8), sorted
		std::vector<PathPoint> path;      // the run's x over time (corner points), for level start positions

		// Whether the macro holds a key at the end of `tick` (after inputs at exactly `tick`).
		bool heldAt(uint32_t tick) const {
			bool held[2] = {false, false};
			for (auto const& in : inputs) {
				if (in.frame > tick) break;
				held[in.player2 ? 1 : 0] = in.down;
			}
			return held[0] || held[1];
		}
		// Latest tick <= `tick` at which no key is held: a respawn mid-hold isn't reproducible
		// (GD releases the player 2 ticks after every respawn), so start positions avoid holds.
		uint32_t latestFreeTick(uint32_t tick) const {
			while (tick > 0 && heldAt(tick)) {
				// Step back to just before the press that started this hold.
				uint32_t press = 0;
				for (auto const& in : inputs) {
					if (in.frame > tick) break;
					if (in.down) press = in.frame;
				}
				tick = press >= 2 ? press - 2 : 0;
			}
			return tick;
		}
		uint32_t lastInputFrame() const { return inputs.empty() ? 0 : inputs.back().frame; }

		// Local/editor levels all have ID 0, so those are told apart by name.
		bool isFor(int id, std::string const& name) const {
			return levelID == id && (id != 0 || levelName == name);
		}

		void clear() {
			inputs.clear();
			path.clear();
			endFrame = 0;
		}
	};
}
