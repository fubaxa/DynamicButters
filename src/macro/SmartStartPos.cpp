#include "macro/SmartStartPos.hpp"

#include <Geode/Geode.hpp>

#include <optional>

using namespace geode::prelude;

namespace db::smartstartpos {
	namespace {
		std::optional<Speed> speedOf(int id) {
			switch (id) {
				case 200: return Speed::Slow;     // 0.5x
				case 201: return Speed::Normal;   // 1x
				case 202: return Speed::Fast;     // 2x
				case 203: return Speed::Faster;   // 3x
				case 1334: return Speed::Fastest; // 4x
				default: return std::nullopt;
			}
		}

		std::optional<int> modeOf(int id) {
			switch (id) {
				case 12: return 0;   // cube
				case 13: return 1;   // ship
				case 47: return 2;   // ball
				case 111: return 3;  // ufo
				case 660: return 4;  // wave
				case 745: return 5;  // robot
				case 1331: return 6; // spider
				case 1933: return 7; // swing
				default: return std::nullopt;
			}
		}

		// Last portal of each kind before the start position: (x, value).
		template <class T>
		struct Last {
			float x = -1e9f;
			std::optional<T> value;
			void offer(float ox, T v) {
				if (ox >= x) { x = ox; value = v; }
			}
		};
	}

	void apply(PlayLayer* pl) {
		if (!pl || !pl->m_startPosObject || !pl->m_objects) return;
		if (!Mod::get()->getSettingValue<bool>("smart-startpos")) return;
		auto sp = pl->m_startPosObject;
		auto st = sp->m_startSettings;
		if (!st) return;

		CCPoint spos = sp->getPosition();
		Last<Speed> speed;
		Last<int> mode;
		Last<bool> mini, dual, mirror;
		int inside = 0, skippedNoPreview = 0;

		// Before any portal the player has the level's own start settings: they count as passed
		// first, so a start position with no portal of some kind before it gets the level's value.
		if (auto ls = pl->m_levelSettings) {
			speed.offer(-1e9f, ls->m_startSpeed);
			mode.offer(-1e9f, ls->m_startMode);
			mini.offer(-1e9f, ls->m_startMini);
			dual.offer(-1e9f, ls->m_startDual);
			mirror.offer(-1e9f, ls->m_mirrorMode);
		}

		for (auto obj : CCArrayExt<GameObject*>(pl->m_objects)) {
			if (!obj) continue;
			int id = obj->m_objectID;
			auto spd = speedOf(id);
			auto md = modeOf(id);
			// Gravity portals (10, 11) are left alone: the start position's own gravity setting wins.
			bool other = id == 99 || id == 101 || id == 286 || id == 287 || id == 45 || id == 46;
			if (!spd && !md && !other) continue;

			float ox = obj->getPositionX();
			// A portal the start position is inside counts as passed, even when its centre is
			// just ahead. Leaving it to the game isn't safe: on spawn the game checks it with the
			// start position's old gamemode hitbox, which can be too small to touch it. Forcing
			// it is harmless when the game applies it too (it sets the same thing again).
			bool isInside = obj->getObjectRect().containsPoint(spos);
			if (ox > spos.x && !isInside) continue;
			if (isInside) inside++;

			if (spd) {
				// Unused speed portals have preview turned off.
				if (auto eff = typeinfo_cast<EffectGameObject*>(obj); eff && !eff->m_shouldPreview) {
					skippedNoPreview++;
					continue;
				}
				speed.offer(ox, *spd);
			} else if (md) {
				mode.offer(ox, *md);
			} else if (id == 99 || id == 101) {
				mini.offer(ox, id == 101);
			} else if (id == 286 || id == 287) {
				dual.offer(ox, id == 286);
			} else {
				mirror.offer(ox, id == 45);
			}
		}

		std::string changes;
		auto note = [&](char const* what, auto before, auto after) {
			if (before != after) changes += fmt::format(" {} {}->{}", what, before, after);
		};
		if (speed.value) { note("speed", int(st->m_startSpeed), int(*speed.value)); st->m_startSpeed = *speed.value; }
		if (mode.value) { note("mode", st->m_startMode, *mode.value); st->m_startMode = *mode.value; }
		if (mini.value) { note("mini", st->m_startMini, *mini.value); st->m_startMini = *mini.value; }
		if (dual.value) { note("dual", st->m_startDual, *dual.value); st->m_startDual = *dual.value; }
		if (mirror.value) { note("mirror", st->m_mirrorMode, *mirror.value); st->m_mirrorMode = *mirror.value; }

		log::info("smartstartpos: start position at x={:.2f}:{} ({} portals containing it, forced; {} speed portals without preview skipped)",
			spos.x, changes.empty() ? " settings already match" : changes, inside, skippedNoPreview);
	}
}
