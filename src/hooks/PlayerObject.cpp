#include <Geode/Geode.hpp>
#include <Geode/modify/PlayerObject.hpp>

#include "macro/MacroManager.hpp"
#include "physics/PlayerState.hpp"

using namespace geode::prelude;

// Known issue (PLAN.md "Open issues"): pausing during a macro hold breaks the hold.
// Pausing makes GD call releaseAllButtons() directly (not through handleButton). Blocking that
// call was tried and did NOT keep the hold, so GD clears the hold somewhere else too.
// This hook only logs the calls, for when the issue is picked up again.

class $modify(DBPlayerObject, PlayerObject) {
	void releaseAllButtons() {
		auto context = db::runtime::pauseContext;
		if (context) {
			log::debug("releaseAllButtons: context={} holding={}", context, db::MacroManager::get().isHoldingAny());
		}
		PlayerObject::releaseAllButtons();
	}
};
