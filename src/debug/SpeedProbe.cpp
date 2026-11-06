#include "debug/SpeedProbe.hpp"

#include <Geode/Geode.hpp>

#include <cmath>

#include "debug/Settings.hpp"

using namespace geode::prelude;

namespace db::speedprobe {
	namespace {
		float s_lastSpeed = -1.f;
		double s_lastMult = -1.0;

		bool isSpeedPortal(int id) {
			// 0.5x, 1x, 2x, 3x, 4x
			return id == 200 || id == 201 || id == 202 || id == 203 || id == 1334;
		}

		char const* portalName(int id) {
			switch (id) {
				case 200: return "0.5x";
				case 201: return "1x";
				case 202: return "2x";
				case 203: return "3x";
				case 1334: return "4x";
				default: return "?";
			}
		}

		std::string rectStr(CCRect const& r) {
			return fmt::format("[{:.2f}..{:.2f} x {:.2f}..{:.2f}]", r.getMinX(), r.getMaxX(), r.getMinY(), r.getMaxY());
		}

		// Speed portals within 120 units of x (both sides), closest first in the log.
		void logNearbyPortals(PlayLayer* pl, float x) {
			if (!pl->m_objects) return;
			int found = 0;
			for (auto obj : CCArrayExt<GameObject*>(pl->m_objects)) {
				if (!obj || !isSpeedPortal(obj->m_objectID)) continue;
				float ox = obj->getPositionX();
				if (std::abs(ox - x) > 120.f) continue;
				auto r = obj->getObjectRect();
				log::info("speedprobe:   portal {} at x={:.2f} y={:.2f} hitbox {} (player x - portal x = {:.2f})",
					portalName(obj->m_objectID), ox, obj->getPositionY(), rectStr(r), x - ox);
				if (++found >= 4) break;
			}
			if (!found) log::info("speedprobe:   no speed portal within 120 units");
		}
	}

	void onAttemptStart(PlayLayer* pl, uint32_t tick) {
		if (!settings::tickProbe() || !pl || !pl->m_player1) return;
		auto p = pl->m_player1;
		s_lastSpeed = p->m_playerSpeed;
		s_lastMult = p->m_speedMultiplier;
		if (auto sp = pl->m_startPosObject) {
			auto st = sp->m_startSettings;
			log::info("speedprobe: start position at x={:.2f} y={:.2f}: settings speed={} mode={} mini={}; player speed {:.4f} mult {:.4f} at x={:.3f} y={:.3f} hitbox {}",
				sp->getPositionX(), sp->getPositionY(),
				st ? static_cast<int>(st->m_startSpeed) : -1, st ? st->m_startMode : -1, st ? st->m_startMini : false,
				p->m_playerSpeed, p->m_speedMultiplier, p->m_position.x, p->m_position.y, rectStr(p->getObjectRect()));
			logNearbyPortals(pl, p->m_position.x);
		}
	}

	void onStepEnd(PlayLayer* pl, uint32_t tick) {
		if (!settings::tickProbe() || !pl || !pl->m_player1) return;
		auto p = pl->m_player1;
		if (p->m_playerSpeed == s_lastSpeed && p->m_speedMultiplier == s_lastMult) return;
		log::info("speedprobe: tick {} speed {:.4f} -> {:.4f} (mult {:.4f} -> {:.4f}) at player x={:.3f} y={:.3f} hitbox {}{}",
			tick, s_lastSpeed, p->m_playerSpeed, s_lastMult, p->m_speedMultiplier,
			p->m_position.x, p->m_position.y, rectStr(p->getObjectRect()),
			pl->m_startPosObject ? " (start position run)" : "");
		s_lastSpeed = p->m_playerSpeed;
		s_lastMult = p->m_speedMultiplier;
		logNearbyPortals(pl, p->m_position.x);
	}
}
