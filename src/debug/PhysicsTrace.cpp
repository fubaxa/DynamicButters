#include "debug/PhysicsTrace.hpp"

#include <Geode/Geode.hpp>

#include <cctype>
#include <chrono>
#include <filesystem>
#include <fstream>

#include "dynamic/ExactState.hpp"
#include "dynamic/WindowScan.hpp"
#include "macro/Seek.hpp"
#include "physics/PlayerState.hpp"

using namespace geode::prelude;

namespace db::trace {
	bool enabled() {
		return Mod::get()->getSettingValue<bool>("physics-trace");
	}

	namespace {
		struct State {
			std::ofstream out;
			std::string path;
			unsigned attempt = 0;
			uint64_t steps = 0;
			bool completed = false; // level complete: the end animation isn't traced
			GameObject* lastPortal[2] = {nullptr, nullptr}; // per player, to see when a portal is used
		} s;

		bool paused();

		// A trace file is open and the current attempt is a real, unfinished run.
		bool writing() {
			return s.out.is_open() && !s.completed && enabled() && !paused();
		}

		// Shortest text that reads back as exactly the same value.
		std::string f(float v) { return fmt::format("{:.9g}", v); }
		std::string d(double v) { return fmt::format("{:.17g}", v); }

		bool paused() {
			// Scans and seeks replay the level many times in fast-forward: not a real run.
			return runtime::quiet || scan::active() || seek::active();
		}

		std::string safeName(std::string name) {
			for (auto& c : name) {
				if (!std::isalnum(static_cast<unsigned char>(c)) && c != ' ' && c != '-' && c != '_') c = '_';
			}
			return name.empty() ? std::string("level") : name;
		}

		std::string rect(GameObject* obj) {
			if (!obj) return ",,,,,";
			auto const& r = obj->getObjectRect();
			return fmt::format("{},{},{},{},{},{}", f(obj->getPositionX()), f(obj->getPositionY()),
				f(r.origin.x), f(r.origin.y), f(r.size.width), f(r.size.height));
		}

		// Rotation plus the 4 corners of the object's oriented box (the hitbox rotated objects use),
		// as GD last computed it. Read only: an empty or out-of-date box is written as empty fields.
		std::string oriented(GameObject* obj) {
			if (!obj) return ",,,,,,,,";
			auto box = obj->m_orientedBox;
			if (!box || obj->m_isOrientedBoxDirty) return fmt::format("{},,,,,,,,", f(obj->getRotation()));
			auto const& c = box->m_corners;
			return fmt::format("{},{},{},{},{},{},{},{},{}", f(obj->getRotation()),
				f(c[0].x), f(c[0].y), f(c[1].x), f(c[1].y), f(c[2].x), f(c[2].y), f(c[3].x), f(c[3].y));
		}

		void writeHeader(PlayLayer* pl) {
			auto level = pl->m_level;
			s.out << "# DynamicButter physics trace v3\n";
			s.out << fmt::format("# level id={} name={} gd={}\n",
				level ? level->m_levelID.value() : 0, level ? std::string(level->m_levelName) : "?",
				GEODE_GD_VERSION_STRING);
			s.out << "# ticks: m_currentProgress, +2 per physics step (240 steps/s)\n";
			if (auto ls = pl->m_levelSettings) {
				s.out << fmt::format(
					"# start mode={} speed={} mini={} dual={} mirror={} flipped={} platformer={} twoPlayer={} "
					"rotateGameplay={} reverseGameplay={}\n",
					ls->m_startMode, static_cast<int>(ls->m_startSpeed), ls->m_startMini, ls->m_startDual,
					ls->m_mirrorMode, ls->m_isFlipped, ls->m_platformerMode, ls->m_twoPlayerMode,
					ls->m_rotateGameplay, ls->m_reverseGameplay);
				// Level-string keys in brackets (level start object).
				s.out << fmt::format(
					"# legacy fixGravityBug[kA32]={} fixNegativeScale[kA33]={} fixRobotJump[kA34]={} "
					"fixRadiusCollision[kA39]={} enable22Changes[kA40]={} allowStaticRotate[kA41]={} "
					"reverseSync[kA42]={} decreaseBoostSlide[kA45]={} enableImpulseFix[kA48]={} "
					"enablePlayerSqueeze[kA31]={} allowMultiRotation[kA27]={} dynamicLevelHeight[kA37]={} "
					"sortGroups[kA38]={}\n",
					ls->m_fixGravityBug, ls->m_fixNegativeScale, ls->m_fixRobotJump,
					ls->m_fixRadiusCollision, ls->m_enable22Changes, ls->m_allowStaticRotate,
					ls->m_reverseSync, ls->m_decreaseBoostSlide, ls->m_enableImpulseFix,
					ls->m_enablePlayerSqueeze, ls->m_allowMultiRotation, ls->m_dynamicLevelHeight,
					ls->m_sortGroups);
				// GD's own serialized settings: every key it knows, including ones not named above.
				s.out << "# settings " << ls->getSaveString() << "\n";
			} else {
				s.out << "# start/legacy: no level settings available\n";
			}
			if (auto p = pl->m_player1) {
				// The copies GD keeps on the player (read during physics).
				s.out << fmt::format("# player1 fixRobotJump={} enable22Changes={} enableImpulseFix={}\n",
					p->m_fixRobotJump, p->m_enable22Changes, p->m_enableImpulseFix);
			}
			s.out << "# A,attempt,tick,practice\n";
			s.out << "# S,tick,stepInFrame,x,y,xVel,yVel,rotation,size,speed,onGround,upsideDown,mode,"
				"dashing,jumpBuffered,ringJump,onSlope,holding,dual,rectX,rectY,rectW,rectH\n";
			s.out << "#   mode bits: 1 ship, 2 ufo, 4 ball, 8 wave, 16 robot, 32 spider, 64 swing (0 = cube)\n";
			s.out << "# I,tick,down,button,p2key,fromMacro,x,y,yVel\n";
			s.out << "# O,tick,kind,player,objectID,objX,objY,rectX,rectY,rectW,rectH,objRot,objOBB(8),playerRot,playerOBB(8)\n";
			s.out << "#   ring = touching the orb (every step); speed portals are kind=trigger (IDs 200 201 202 203 1334)\n";
			s.out << "#   OBB = 4 corners x,y of the oriented (rotated) hitbox, empty if GD hasn't computed it\n";
			s.out << "# P,tick,player,objectID,objX,objY,rectX,rectY,rectW,rectH,objRot,objOBB(8),playerRot,playerOBB(8)\n";
			s.out << "#   portal used (the player's last activated portal changed): mode, gravity, size, mirror, dual...\n";
			s.out << "# U,tick,player,objectID,objX,objY,rectX,rectY,rectW,rectH   (orb actually used: it changed the player)\n";
			s.out << "# D,tick,objectID,objX,objY,rectX,rectY,rectW,rectH,dead   (GD's anticheat spike is left out)\n";
			s.out << "# C,tick   (nothing more is written until the next attempt)\n";
		}

		bool open(PlayLayer* pl) {
			if (s.out.is_open()) return true;
			auto dir = std::filesystem::u8path((Mod::get()->getSaveDir() / "traces").string());
			std::error_code ec;
			std::filesystem::create_directories(dir, ec);
			auto level = pl->m_level;
			auto stamp = std::chrono::duration_cast<std::chrono::seconds>(
				std::chrono::system_clock::now().time_since_epoch()).count();
			auto name = fmt::format("{}_{}_{}.csv", level ? level->m_levelID.value() : 0,
				safeName(level ? std::string(level->m_levelName) : ""), stamp);
			auto file = dir / std::filesystem::u8path(name);
			s.out.open(file, std::ios::out | std::ios::trunc);
			if (!s.out.is_open()) {
				log::warn("trace: could not open {}", file.string());
				return false;
			}
			s.path = file.string();
			s.attempt = 0;
			s.steps = 0;
			writeHeader(pl);
			log::info("trace: writing {}", s.path);
			return true;
		}

		PlayLayer* currentPlayLayer() { return PlayLayer::get(); }
	}

	void onAttemptStart(PlayLayer* pl, uint32_t tick, bool practice) {
		if (!pl || !enabled() || paused() || !open(pl)) return;
		s.completed = false;
		// Whatever the players hold at the start doesn't count as a portal used in this attempt.
		s.lastPortal[0] = pl->m_player1 ? pl->m_player1->m_lastActivatedPortal : nullptr;
		s.lastPortal[1] = pl->m_player2 ? pl->m_player2->m_lastActivatedPortal : nullptr;
		s.out << fmt::format("A,{},{},{}\n", ++s.attempt, tick, practice ? 1 : 0);
		s.out.flush();
	}

	void onStepEnd(PlayLayer* pl, uint32_t tick) {
		if (!pl || !writing()) return;
		auto p = pl->m_player1;
		if (!p) return;
		auto e = ExactPlayer::capture(p);
		auto const& b = e.basic;
		auto const& r = p->getObjectRect();
		s.out << fmt::format("S,{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{}\n",
			tick, runtime::stepInFrame, f(b.x), f(b.y), d(b.xVel), d(b.yVel), f(b.rotation),
			f(b.vehicleSize), f(b.playerSpeed), b.onGround ? 1 : 0, b.upsideDown ? 1 : 0, b.gamemode,
			e.dashing ? 1 : 0, e.jumpBuffered ? 1 : 0, e.ringJump ? 1 : 0, e.onSlope ? 1 : 0,
			e.holding ? 1 : 0, pl->m_gameState.m_isDualMode ? 1 : 0,
			f(r.origin.x), f(r.origin.y), f(r.size.width), f(r.size.height));
		PlayerObject* players[2] = {pl->m_player1, pl->m_player2};
		for (int i = 0; i < 2; ++i) {
			auto pp = players[i];
			if (!pp || pp->m_lastActivatedPortal == s.lastPortal[i]) continue;
			s.lastPortal[i] = pp->m_lastActivatedPortal;
			if (!s.lastPortal[i]) continue;
			s.out << fmt::format("P,{},{},{},{},{},{}\n", tick, i + 1, s.lastPortal[i]->m_objectID,
				rect(s.lastPortal[i]), oriented(s.lastPortal[i]), oriented(pp));
		}
		if (++s.steps % 2400 == 0) s.out.flush();
	}

	void onInput(PlayLayer* pl, uint32_t tick, bool down, int button, bool p2key, bool fromMacro, PlayerObject* p) {
		if (!pl || !writing()) return;
		s.out << fmt::format("I,{},{},{},{},{},{},{},{}\n", tick, down ? 1 : 0, button, p2key ? 1 : 0,
			fromMacro ? 1 : 0, p ? f(p->m_position.x) : "", p ? f(p->m_position.y) : "",
			p ? d(p->m_yVelocity) : "");
	}

	void onInteraction(char const* kind, PlayerObject* player, GameObject* obj) {
		auto pl = currentPlayLayer();
		if (!pl || !writing()) return;
		if (!player) player = pl->m_player1;
		s.out << fmt::format("O,{},{},{},{},{},{},{}\n", pl->m_gameState.m_currentProgress, kind,
			player == pl->m_player2 ? 2 : 1, obj ? obj->m_objectID : 0, rect(obj), oriented(obj), oriented(player));
	}

	void onRingUsed(PlayerObject* player, GameObject* obj) {
		auto pl = currentPlayLayer();
		if (!pl || !writing()) return;
		s.out << fmt::format("U,{},{},{},{}\n", pl->m_gameState.m_currentProgress,
			player == pl->m_player2 ? 2 : 1, obj ? obj->m_objectID : 0, rect(obj));
	}

	void onDestroyPlayer(PlayLayer* pl, PlayerObject* player, GameObject* obj) {
		if (!pl || !writing()) return;
		// GD calls destroyPlayer with its own anticheat spike on purpose (never a real death).
		if (obj && obj == pl->m_anticheatSpike) return;
		s.out << fmt::format("D,{},{},{},{}\n", pl->m_gameState.m_currentProgress,
			obj ? obj->m_objectID : 0, rect(obj), player && player->m_isDead ? 1 : 0);
		s.out.flush();
	}

	void onLevelComplete(uint32_t tick) {
		if (!writing()) return;
		s.out << fmt::format("C,{}\n", tick);
		s.out.flush();
		s.completed = true;
	}

	void onQuit() {
		if (!s.out.is_open()) return;
		s.out.flush();
		s.out.close();
		log::info("trace: closed {} ({} attempts, {} steps)", s.path, s.attempt, s.steps);
		s.path.clear();
	}
}
