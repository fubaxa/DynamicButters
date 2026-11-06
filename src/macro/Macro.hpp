#pragma once

#include <cstdint>
#include <string>
#include <vector>

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

	struct MacroInput {
		uint32_t frame = 0;   // m_gameState.m_currentProgress when GD applied the input (half-step units)
		uint8_t button = 1;   // 1 = jump (platformer left/right not supported yet)
		bool down = false;    // press / release
		bool player2 = false; // the P2 *key* was used (in 1-player levels it still moves player 1)
		PhysSnapshot phys;    // state right before the input was applied
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

		// Local/editor levels all have ID 0, so those are told apart by name.
		bool isFor(int id, std::string const& name) const {
			return levelID == id && (id != 0 || levelName == name);
		}

		void clear() {
			inputs.clear();
			endFrame = 0;
		}
	};
}
