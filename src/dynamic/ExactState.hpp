#pragma once

#include <Geode/Geode.hpp>

#include <cstdint>
#include <functional>
#include <string>

#include "physics/PlayerState.hpp"

// Everything about a player that decides the rest of the run, compared exactly (no tolerance).
// If two runs have equal ExactStates (and the same level interactions) for several steps in a row,
// they are the same run from then on.
//
// Rotation is captured but not compared: GD's player hitbox doesn't rotate, so rotation is only
// visual (a rolling ball or a cube mid-jump would otherwise never match a run that's otherwise identical).
namespace db {
	struct ExactPlayer {
		PlayerState basic;   // x, y, rotation, y/x velocity, size, speed, ground, gravity, gamemode
		bool dashing = false;
		bool jumpBuffered = false;
		bool ringJump = false;
		bool onSlope = false;
		bool holding = false; // jump button held
		size_t touchedRings = 0; // hash of the orbs currently being touched

		static ExactPlayer capture(PlayerObject* p) {
			ExactPlayer s;
			if (!p) return s;
			s.basic = PlayerState::capture(p);
			s.dashing = p->m_isDashing;
			s.jumpBuffered = p->m_jumpBuffered;
			s.ringJump = p->m_stateRingJump;
			s.onSlope = p->m_isOnSlope;
			auto held = p->m_holdingButtons.find(1);
			s.holding = held != p->m_holdingButtons.end() && held->second;
			size_t h = p->m_touchedRings.size();
			for (int id : p->m_touchedRings) h ^= std::hash<int>{}(id) + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
			s.touchedRings = h;
			return s;
		}

		bool operator==(ExactPlayer const& o) const { return diff(o).empty(); }

		// Names of the compared fields that differ, with both values ("" = equal).
		std::string diff(ExactPlayer const& o) const {
			std::string out;
			auto const& a = basic;
			auto const& b = o.basic;
			auto add = [&](char const* name, auto x, auto y) {
				if (x != y) out += fmt::format(" {}={}/{}", name, x, y);
			};
			add("x", a.x, b.x);
			add("y", a.y, b.y);
			add("yVel", a.yVel, b.yVel);
			add("xVel", a.xVel, b.xVel);
			add("size", a.vehicleSize, b.vehicleSize);
			add("speed", a.playerSpeed, b.playerSpeed);
			add("ground", a.onGround, b.onGround);
			add("upside", a.upsideDown, b.upsideDown);
			add("mode", a.gamemode, b.gamemode);
			add("dash", dashing, o.dashing);
			add("jumpBuffer", jumpBuffered, o.jumpBuffered);
			add("ringJump", ringJump, o.ringJump);
			add("slope", onSlope, o.onSlope);
			add("holding", holding, o.holding);
			add("touchedRings", touchedRings, o.touchedRings);
			return out;
		}

		// Candidate for resting on a block (or the ceiling with flipped gravity): grounded, not dashing,
		// no button held. GD keeps a small non-zero y-velocity on most grounded steps, so resting is
		// decided by the state staying the same over several steps (see settledWith), not yVel == 0.
		bool resting() const { return basic.onGround && !dashing && !holding; }

		// Same settled state as `o` apart from x (which only depends on time).
		bool settledWith(ExactPlayer const& o) const {
			return resting() && o.resting() && basic.y == o.basic.y && basic.yVel == o.basic.yVel
				&& basic.upsideDown == o.basic.upsideDown && basic.gamemode == o.basic.gamemode
				&& basic.vehicleSize == o.basic.vehicleSize && basic.playerSpeed == o.basic.playerSpeed
				&& onSlope == o.onSlope && jumpBuffered == o.jumpBuffered && ringJump == o.ringJump;
		}
	};

	// Player 2 only counts in dual mode: outside it GD keeps updating player 2 in the background
	// during an unbroken run but leaves it untouched on a respawn, and it has no effect on the run.
	struct ExactState {
		ExactPlayer p1, p2;
		bool dual = false;

		static ExactState capture(GJBaseGameLayer* gl) {
			ExactState s;
			if (!gl) return s;
			s.dual = gl->m_gameState.m_isDualMode;
			s.p1 = ExactPlayer::capture(gl->m_player1);
			if (s.dual) s.p2 = ExactPlayer::capture(gl->m_player2);
			return s;
		}

		bool operator==(ExactState const& o) const {
			return dual == o.dual && p1 == o.p1 && (!dual || p2 == o.p2);
		}

		std::string diff(ExactState const& o) const {
			auto d1 = p1.diff(o.p1);
			auto d2 = (dual || o.dual) ? p2.diff(o.p2) : std::string();
			if (dual != o.dual) d2 += fmt::format(" dual={}/{}", dual, o.dual);
			std::string out;
			if (!d1.empty()) out += "P1:" + d1;
			if (!d2.empty()) out += (out.empty() ? "P2:" : " P2:") + d2;
			return out;
		}
	};
}
