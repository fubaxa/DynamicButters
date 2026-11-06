#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>

using namespace geode::prelude;

// M0: smoke test. Proves the mod loads on GD 2.2091 / Geode v6 alpha and that
// the hook targets from PLAN.md §2 exist with the signatures we expect.
// The compile itself is the signature check: a mismatched signature fails to build.

$on_mod(Loaded) {
	log::info("DynamicButter loaded (GD {}, Geode {})",
		GEODE_GD_VERSION_STRING, Loader::get()->getVersion().toVString());
}

class $modify(DBPlayLayer, PlayLayer) {
	// 2.2091 signature: (level, dontCreateObjects); 2.2081 had an extra useReplay arg.
	bool init(GJGameLevel* level, bool dontCreateObjects) {
		if (!PlayLayer::init(level, dontCreateObjects)) return false;
		log::info("PlayLayer::init level {} '{}'", level->m_levelID.value(), level->m_levelName);
		return true;
	}
};

class $modify(DBBaseGameLayer, GJBaseGameLayer) {
	void handleButton(bool down, int button, bool isPlayer1) {
		log::debug("handleButton down={} button={} p1={} tick={}",
			down, button, isPlayer1, m_gameState.m_currentProgress);
		GJBaseGameLayer::handleButton(down, button, isPlayer1);
	}
};
