#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayerObject.hpp>

#include "dynamic/ExactState.hpp"
#include "dynamic/WindowScan.hpp"
#include "debug/PhysicsTrace.hpp"

using namespace geode::prelude;

// Level interactions that depend on where the player is (orbs, pads, touch triggers, pickups).
// The Dynamic scan compares them between the reference run and each trial: two runs with the same
// player state but a different set of used orbs or triggered groups are not the same run.

class $modify(DBInteractions, GJBaseGameLayer) {
	bool isPlayLayer() {
		return PlayLayer::get() == static_cast<GJBaseGameLayer*>(this);
	}

	void playerTouchedRing(PlayerObject* player, RingObject* object) {
		if (isPlayLayer()) {
			db::scan::onInteraction(db::scan::Interaction::Ring, object);
			db::trace::onInteraction("ring", player, object);
		}
		GJBaseGameLayer::playerTouchedRing(player, object);
	}

	void playerTouchedTrigger(PlayerObject* player, EffectGameObject* object) {
		if (isPlayLayer()) {
			db::scan::onInteraction(db::scan::Interaction::Trigger, object);
			db::trace::onInteraction("trigger", player, object);
		}
		GJBaseGameLayer::playerTouchedTrigger(player, object);
	}

	void bumpPlayer(PlayerObject* player, EffectGameObject* object) {
		if (isPlayLayer()) {
			db::scan::onInteraction(db::scan::Interaction::Pad, object);
			db::trace::onInteraction("pad", player, object);
		}
		GJBaseGameLayer::bumpPlayer(player, object);
	}

	void pickupItem(EffectGameObject* object) {
		if (isPlayLayer()) {
			db::scan::onInteraction(db::scan::Interaction::Pickup, object);
			db::trace::onInteraction("pickup", nullptr, object);
		}
		GJBaseGameLayer::pickupItem(object);
	}
};

// playerTouchedRing fires on every step the player overlaps an orb. ringJump is where the orb can
// actually be used; it counts as used only if it changed the player's state (GD may still reject
// it, e.g. an orb already used this press). Trace only.
class $modify(DBRingUse, PlayerObject) {
	void ringJump(RingObject* object, bool skipCheck) {
		auto pl = PlayLayer::get();
		bool mine = pl && (this == pl->m_player1 || this == pl->m_player2) && db::trace::enabled();
		db::ExactPlayer before;
		if (mine) before = db::ExactPlayer::capture(this);
		PlayerObject::ringJump(object, skipCheck);
		if (mine && !(db::ExactPlayer::capture(this) == before)) db::trace::onRingUsed(this, object);
	}
};
