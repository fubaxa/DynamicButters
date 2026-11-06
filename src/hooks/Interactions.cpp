#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>

#include "dynamic/WindowScan.hpp"

using namespace geode::prelude;

// Level interactions that depend on where the player is (orbs, pads, touch triggers, pickups).
// The Dynamic scan compares them between the reference run and each trial: two runs with the same
// player state but a different set of used orbs or triggered groups are not the same run.

class $modify(DBInteractions, GJBaseGameLayer) {
	bool isPlayLayer() {
		return PlayLayer::get() == static_cast<GJBaseGameLayer*>(this);
	}

	void playerTouchedRing(PlayerObject* player, RingObject* object) {
		if (isPlayLayer()) db::scan::onInteraction(db::scan::Interaction::Ring, object);
		GJBaseGameLayer::playerTouchedRing(player, object);
	}

	void playerTouchedTrigger(PlayerObject* player, EffectGameObject* object) {
		if (isPlayLayer()) db::scan::onInteraction(db::scan::Interaction::Trigger, object);
		GJBaseGameLayer::playerTouchedTrigger(player, object);
	}

	void bumpPlayer(PlayerObject* player, EffectGameObject* object) {
		if (isPlayLayer()) db::scan::onInteraction(db::scan::Interaction::Pad, object);
		GJBaseGameLayer::bumpPlayer(player, object);
	}

	void pickupItem(EffectGameObject* object) {
		if (isPlayLayer()) db::scan::onInteraction(db::scan::Interaction::Pickup, object);
		GJBaseGameLayer::pickupItem(object);
	}
};
