#pragma once

#include <Geode/Geode.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>

namespace db {
	// Snapshot of the player state that matters for physics. Captured at the end of a step.
	// restore() only writes the values seen to drift so far (position, rotation, y-velocity);
	// the rest is compared, so the checkpoint test reports if GD loses anything else.
	struct PlayerState {
		float x = 0.f, y = 0.f;
		float rotation = 0.f;
		double yVel = 0.0;
		double xVel = 0.0;
		float vehicleSize = 1.f;
		float playerSpeed = 0.f;
		bool onGround = false;
		bool upsideDown = false;
		uint16_t gamemode = 0; // bit flags: ship, ufo, ball, wave, robot, spider, swing

		static PlayerState capture(PlayerObject* p) {
			PlayerState s;
			if (!p) return s;
			s.x = p->m_position.x;
			s.y = p->m_position.y;
			s.rotation = p->getRotation();
			s.yVel = p->m_yVelocity;
			s.xVel = p->getCurrentXVelocity();
			s.vehicleSize = p->m_vehicleSize;
			s.playerSpeed = p->m_playerSpeed;
			s.onGround = p->m_isOnGround;
			s.upsideDown = p->m_isUpsideDown;
			s.gamemode = (p->m_isShip << 0) | (p->m_isBird << 1) | (p->m_isBall << 2) | (p->m_isDart << 3)
				| (p->m_isRobot << 4) | (p->m_isSpider << 5) | (p->m_isSwing << 6);
			return s;
		}

		void restore(PlayerObject* p) const {
			if (!p) return;
			cocos2d::CCPoint pos{x, y};
			p->m_position = pos;
			p->setPosition(pos);
			p->setRotation(rotation);
			p->m_yVelocity = yVel;
		}

		struct Diff {
			double dx = 0, dy = 0, dyVel = 0, dxVel = 0, drot = 0;
			bool otherDiffers = false; // flags, gamemode, size, speed

			double worst() const { return std::max({dx, dy, dyVel, dxVel, drot}); }
			bool exact() const { return worst() == 0.0 && !otherDiffers; }
			std::string str() const {
				return fmt::format("dx={:.6f} dy={:.6f} dyVel={:.6f} dxVel={:.6f} drot={:.4f}{}",
					dx, dy, dyVel, dxVel, drot, otherDiffers ? " +flags" : "");
			}
			void maxWith(Diff const& o) {
				dx = std::max(dx, o.dx); dy = std::max(dy, o.dy); dyVel = std::max(dyVel, o.dyVel);
				dxVel = std::max(dxVel, o.dxVel); drot = std::max(drot, o.drot);
				otherDiffers = otherDiffers || o.otherDiffers;
			}
		};

		static Diff diff(PlayerState const& a, PlayerState const& b) {
			Diff d;
			d.dx = std::abs(double(a.x) - b.x);
			d.dy = std::abs(double(a.y) - b.y);
			d.dyVel = std::abs(a.yVel - b.yVel);
			d.dxVel = std::abs(a.xVel - b.xVel);
			double r = std::fmod(std::abs(double(a.rotation) - b.rotation), 360.0);
			d.drot = std::min(r, 360.0 - r);
			d.otherDiffers = a.onGround != b.onGround || a.upsideDown != b.upsideDown
				|| a.gamemode != b.gamemode || a.vehicleSize != b.vehicleSize || a.playerSpeed != b.playerSpeed;
			return d;
		}

		std::string str() const {
			return fmt::format("x={:.6f} y={:.6f} rot={:.4f} yVel={:.6f} xVel={:.6f} ground={} upside={} mode={:#x}",
				x, y, rotation, yVel, xVel, onGround, upsideDown, gamemode);
		}
	};

	// Shared per-frame runtime flags (set by the GJBaseGameLayer hooks).
	namespace runtime {
		inline bool inStep = false;        // inside GJBaseGameLayer::processCommands
		inline unsigned stepInFrame = 0;   // 1-based index of the current step within its rendered frame
		inline char const* pauseContext = nullptr; // "pause" / "resume" while those PlayLayer calls run
	}
}
