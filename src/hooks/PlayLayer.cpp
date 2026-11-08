#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>

#include <optional>

#include "debug/Settings.hpp"
#include "debug/RespawnProbe.hpp"
#include "macro/MacroManager.hpp"
#include "debug/CheckpointTester.hpp"
#include "debug/SpeedProbe.hpp"
#include "physics/PreciseCheckpoints.hpp"
#include "physics/PlayerState.hpp"
#include "safety/SafeMode.hpp"
#include "macro/Seek.hpp"
#include "macro/SmartStartPos.hpp"
#include "dynamic/WindowScan.hpp"
#include "dynamic/DynamicAssist.hpp"

using namespace geode::prelude;

// Tick overlay, attempt/checkpoint bookkeeping, and the MacroManager lifecycle calls.

class $modify(DBPlayLayer, PlayLayer) {
	struct Fields {
		CCLabelBMFont* tickLabel = nullptr;
		CCLabelBMFont* safeLabel = nullptr;
		bool baseTestMode = false; // GD's own test mode (start position), restored when the attempt is clean
		std::optional<unsigned> endTick; // set on level complete: the overlay freezes here
	};

	bool init(GJGameLevel* level, bool dontCreateObjects) {
		// Set up the macro before the original init: init calls resetLevel, which starts attempt 1.
		db::MacroManager::get().onLevelStart(level);
		db::cptest::onLevelStart();
		if (!PlayLayer::init(level, dontCreateObjects)) return false;
		m_fields->baseTestMode = m_isTestMode;
		log::info("PlayLayer::init level {} '{}' twoPlayer={} cbsOverride={} clickBetweenSteps={} clickOnSteps={}",
			level->m_levelID.value(), level->m_levelName,
			m_levelSettings ? m_levelSettings->m_twoPlayerMode : false,
			level->m_cbsOverride, m_clickBetweenSteps, m_clickOnSteps);

		if (db::MacroManager::get().shouldForceCbsOff()) {
			// Only affects this PlayLayer instance; the saved GD option is untouched.
			m_clickBetweenSteps = false;
			log::info("force-cbs-off: clickBetweenSteps set to false for this level");
		}

		auto label = CCLabelBMFont::create("tick 0", "bigFont.fnt");
		label->setID("tick-label"_spr);
		label->setScale(0.4f);
		label->setOpacity(180);
		label->setAnchorPoint({0.f, 1.f});
		auto win = CCDirector::get()->getWinSize();
		label->setPosition({5.f, win.height - 5.f});
		label->setVisible(db::settings::showTickCounter());
		this->addChild(label, 1000);
		m_fields->tickLabel = label;

		// Shown while the attempt won't save progress (safe mode).
		auto safe = CCLabelBMFont::create("Safe mode", "bigFont.fnt");
		safe->setID("safe-mode-label"_spr);
		safe->setScale(0.35f);
		safe->setOpacity(200);
		safe->setColor({255, 120, 120});
		safe->setAnchorPoint({0.f, 1.f});
		safe->setPosition({5.f, win.height - 20.f});
		safe->setVisible(false);
		this->addChild(safe, 1000);
		m_fields->safeLabel = safe;
		return true;
	}

	void postUpdate(float dt) {
		PlayLayer::postUpdate(dt);
		db::seek::onFrameEnd(this);
		db::cptest::onFrameEnd(this);
		applySafeMode();
		if (auto safe = m_fields->safeLabel) {
			bool blocking = db::safe::blocking();
			safe->setVisible(blocking);
			if (blocking) safe->setString(fmt::format("Safe mode: {}", db::safe::reason()).c_str());
		}
		auto label = m_fields->tickLabel;
		if (!label) return;
		bool show = db::settings::showTickCounter();
		label->setVisible(show);
		if (!show) return;

		if (auto scanStatus = db::scan::status(); !scanStatus.empty()) {
			label->setString(scanStatus.c_str());
			return;
		}
		auto& mgr = db::MacroManager::get();
		std::string mode = mgr.isRecording() ? "  REC" : mgr.isPlaying() ? "  PLAY" : "";
		if (auto dyn = db::dynamic::status(); !dyn.empty()) mode = "  " + dyn;
		if (auto end = m_fields->endTick) {
			label->setString(fmt::format("tick {} (end){}", *end, mode).c_str());
		} else {
			label->setString(fmt::format("tick {}{}", m_gameState.m_currentProgress, mode).c_str());
		}
	}

	void resetLevel() {
		unsigned before = m_gameState.m_currentProgress;
		db::smartstartpos::apply(this);
		PlayLayer::resetLevel();
		m_fields->endTick.reset();
		unsigned now = m_gameState.m_currentProgress;
		// GD has finished restoring the player here; compare with / apply the precise state first.
		db::checkpoints::afterReset(this, now);
		if (!db::runtime::quiet) {
			log::info("resetLevel: tick {} -> {} (practice={}, checkpoints={})",
				before, now, m_isPracticeMode,
				m_checkpointArray ? m_checkpointArray->count() : 0);
			// Player state is only fully restored once resetLevel returns
			// (at the end of loadFromCheckpoint it still holds the death position).
			db::debug::logPlayer("respawn: state", now, m_player1);
			db::debug::respawnTick = now;
			db::debug::probeAtTick = now + db::debug::PROBE_DELAY;
		}
		db::MacroManager::get().onAttemptStart(this, now, m_isPracticeMode);
		db::dynamic::onAttemptStart(this, now);
		db::safe::onAttemptStart(now);
		db::speedprobe::onAttemptStart(this, now);
		applySafeMode();
		db::cptest::onAttemptStart(this, now);
	}

	void levelComplete() {
		if (db::scan::onLevelComplete()) return; // a scan trial reached the end: passed
		unsigned tick = m_gameState.m_currentProgress;
		m_fields->endTick = tick;
		db::MacroManager::get().onLevelComplete(this, tick);
		db::cptest::logSummary("level complete");
		db::seek::abort("the level ended before the start position");
		applySafeMode();
		if (db::safe::blocking()) log::info("safe-mode: level complete not saved ({})", db::safe::reason());
		PlayLayer::levelComplete();
	}

	// End-of-level text, effect and end screen: blocked while a Dynamic scan runs (and shortly after),
	// since every trial that reaches the end wall would otherwise queue one.
	// The end animation is a chain of timed actions that ends in levelComplete. During a scan it must
	// not start at all (see WindowScan.hpp); reaching the end wall counts as completing the level.
	void playEndAnimationToPos(CCPoint position) {
		if (db::scan::active() && db::scan::suppressEndUI()) {
			db::scan::onLevelComplete();
			return;
		}
		PlayLayer::playEndAnimationToPos(position);
	}

	void showCompleteText() {
		if (db::scan::suppressEndUI()) return;
		PlayLayer::showCompleteText();
	}

	void showCompleteEffect() {
		if (db::scan::suppressEndUI()) return;
		PlayLayer::showCompleteEffect();
	}

	void showEndLayer() {
		if (db::scan::suppressEndUI()) return;
		PlayLayer::showEndLayer();
	}

	// GD saves a new best % when the player dies.
	void destroyPlayer(PlayerObject* player, GameObject* object) {
		if (db::scan::onPlayerDied(this, object)) return; // a scan trial failed; no GD death
		applySafeMode();
		PlayLayer::destroyPlayer(player, object);
		if (player && player->m_isDead) db::seek::onPlayerDied();
	}

	// Safe mode uses test mode, GD's own "don't save progress" state (start positions use it). It
	// stays on for the whole unsafe attempt, because GD can also save after levelComplete returns
	// (end animation, end screen); it's set back to GD's own value when a clean attempt starts.
	void applySafeMode() {
		db::safe::check();
		bool want = m_fields->baseTestMode || db::safe::blocking();
		if (m_isTestMode != want) {
			m_isTestMode = want;
			log::info("safe-mode: test mode {}", want ? "on" : "off");
		}
	}

	void onQuit() {
		db::MacroManager::get().onQuit();
		db::cptest::logSummary("left the level");
		db::seek::abort("left the level");
		db::scan::abort("left the level");
		PlayLayer::onQuit();
	}

	void pauseGame(bool unfocused) {
		db::runtime::pauseContext = "pause";
		PlayLayer::pauseGame(unfocused);
		db::runtime::pauseContext = nullptr;
	}

	void resume() {
		db::runtime::pauseContext = "resume";
		PlayLayer::resume();
		db::runtime::pauseContext = nullptr;
	}

	void loadFromCheckpoint(CheckpointObject* object) {
		unsigned before = m_gameState.m_currentProgress;
		PlayLayer::loadFromCheckpoint(object);
		db::checkpoints::onLoad(object);
		if (!db::runtime::quiet) log::info("loadFromCheckpoint: tick {} -> {}", before, m_gameState.m_currentProgress);
	}

	CheckpointObject* createCheckpoint() {
		auto cp = PlayLayer::createCheckpoint();
		db::checkpoints::onCreated(this, cp);
		return cp;
	}
};
