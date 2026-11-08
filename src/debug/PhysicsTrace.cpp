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
	namespace {
		struct State {
			std::ofstream out;
			std::string path;
			unsigned attempt = 0;
			uint64_t steps = 0;
		} s;

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

		void writeHeader(PlayLayer* pl) {
			auto level = pl->m_level;
			s.out << "# DynamicButter physics trace v1\n";
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
			s.out << "# O,tick,kind,objectID,objX,objY,rectX,rectY,rectW,rectH\n";
			s.out << "# D,tick,objectID,objX,objY,rectX,rectY,rectW,rectH,dead\n";
			s.out << "# C,tick\n";
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

	bool enabled() {
		return Mod::get()->getSettingValue<bool>("physics-trace");
	}

	void onAttemptStart(PlayLayer* pl, uint32_t tick, bool practice) {
		if (!pl || !enabled() || paused() || !open(pl)) return;
		s.out << fmt::format("A,{},{},{}\n", ++s.attempt, tick, practice ? 1 : 0);
		s.out.flush();
	}

	void onStepEnd(PlayLayer* pl, uint32_t tick) {
		if (!pl || !s.out.is_open() || !enabled() || paused()) return;
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
		if (++s.steps % 2400 == 0) s.out.flush();
	}

	void onInput(PlayLayer* pl, uint32_t tick, bool down, int button, bool p2key, bool fromMacro, PlayerObject* p) {
		if (!pl || !s.out.is_open() || !enabled() || paused()) return;
		s.out << fmt::format("I,{},{},{},{},{},{},{},{}\n", tick, down ? 1 : 0, button, p2key ? 1 : 0,
			fromMacro ? 1 : 0, p ? f(p->m_position.x) : "", p ? f(p->m_position.y) : "",
			p ? d(p->m_yVelocity) : "");
	}

	void onInteraction(char const* kind, GameObject* obj) {
		auto pl = currentPlayLayer();
		if (!pl || !s.out.is_open() || !enabled() || paused()) return;
		s.out << fmt::format("O,{},{},{},{}\n", pl->m_gameState.m_currentProgress, kind,
			obj ? obj->m_objectID : 0, rect(obj));
	}

	void onDestroyPlayer(PlayLayer* pl, PlayerObject* player, GameObject* obj) {
		if (!pl || !s.out.is_open() || !enabled() || paused()) return;
		s.out << fmt::format("D,{},{},{},{}\n", pl->m_gameState.m_currentProgress,
			obj ? obj->m_objectID : 0, rect(obj), player && player->m_isDead ? 1 : 0);
		s.out.flush();
	}

	void onLevelComplete(uint32_t tick) {
		if (!s.out.is_open() || !enabled() || paused()) return;
		s.out << fmt::format("C,{}\n", tick);
		s.out.flush();
	}

	void onQuit() {
		if (!s.out.is_open()) return;
		s.out.flush();
		s.out.close();
		log::info("trace: closed {} ({} attempts, {} steps)", s.path, s.attempt, s.steps);
		s.path.clear();
	}
}
