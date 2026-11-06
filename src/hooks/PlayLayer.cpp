#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>

#include <optional>

#include "debug/Settings.hpp"
#include "debug/RespawnProbe.hpp"
#include "macro/MacroManager.hpp"
#include "debug/CheckpointTester.hpp"
#include "physics/PreciseCheckpoints.hpp"
#include "physics/PlayerState.hpp"

using namespace geode::prelude;

// Tick overlay, attempt/checkpoint bookkeeping, and the MacroManager lifecycle calls.

class $modify(DBPlayLayer, PlayLayer) {
	struct Fields {
		CCLabelBMFont* tickLabel = nullptr;
		std::optional<unsigned> endTick; // set on level complete: the overlay freezes here
	};

	bool init(GJGameLevel* level, bool dontCreateObjects) {
		// Set up the macro before the original init: init calls resetLevel, which starts attempt 1.
		db::MacroManager::get().onLevelStart(level);
		db::cptest::onLevelStart();
		if (!PlayLayer::init(level, dontCreateObjects)) return false;
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
		return true;
	}

	void postUpdate(float dt) {
		PlayLayer::postUpdate(dt);
		db::cptest::onFrameEnd(this);
		auto label = m_fields->tickLabel;
		if (!label) return;
		bool show = db::settings::showTickCounter();
		label->setVisible(show);
		if (!show) return;

		auto& mgr = db::MacroManager::get();
		char const* mode = mgr.isRecording() ? "  REC" : mgr.isPlaying() ? "  PLAY" : "";
		if (auto end = m_fields->endTick) {
			label->setString(fmt::format("tick {} (end){}", *end, mode).c_str());
		} else {
			label->setString(fmt::format("tick {}{}", m_gameState.m_currentProgress, mode).c_str());
		}
	}

	void resetLevel() {
		unsigned before = m_gameState.m_currentProgress;
		PlayLayer::resetLevel();
		m_fields->endTick.reset();
		unsigned now = m_gameState.m_currentProgress;
		// GD has finished restoring the player here; compare with / apply the precise state first.
		db::checkpoints::afterReset(this, now);
		log::info("resetLevel: tick {} -> {} (practice={}, checkpoints={})",
			before, now, m_isPracticeMode,
			m_checkpointArray ? m_checkpointArray->count() : 0);
		// Player state is only fully restored once resetLevel returns
		// (at the end of loadFromCheckpoint it still holds the death position).
		db::debug::logPlayer("respawn: state", now, m_player1);
		db::debug::respawnTick = now;
		db::debug::probeAtTick = now + db::debug::PROBE_DELAY;
		db::MacroManager::get().onAttemptStart(this, now, m_isPracticeMode);
		db::cptest::onAttemptStart(this, now);
	}

	void levelComplete() {
		unsigned tick = m_gameState.m_currentProgress;
		m_fields->endTick = tick;
		db::MacroManager::get().onLevelComplete(this, tick);
		db::cptest::logSummary("level complete");
		PlayLayer::levelComplete();
	}

	void onQuit() {
		db::MacroManager::get().onQuit();
		db::cptest::logSummary("left the level");
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
		log::info("loadFromCheckpoint: tick {} -> {}", before, m_gameState.m_currentProgress);
	}

	CheckpointObject* createCheckpoint() {
		auto cp = PlayLayer::createCheckpoint();
		db::checkpoints::onCreated(this, cp);
		return cp;
	}
};
