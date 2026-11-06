#include "physics/PreciseCheckpoints.hpp"

#include <Geode/Geode.hpp>
#include <Geode/modify/CheckpointObject.hpp>

#include "debug/Settings.hpp"
#include "physics/PlayerState.hpp"

using namespace geode::prelude;

class $modify(DBCheckpointObject, CheckpointObject) {
	struct Fields {
		bool valid = false;
		uint32_t tick = 0;
		db::PlayerState p1, p2;
	};
};

namespace db::checkpoints {
	namespace {
		// createCheckpoint can run in the middle of a step; the snapshot must be the end-of-step
		// state (that's what the run continues from), so it's taken when the step finishes.
		Ref<CheckpointObject> pendingCapture;
		Ref<CheckpointObject> pendingRestore;

		void capture(PlayLayer* pl, CheckpointObject* cp) {
			auto f = static_cast<DBCheckpointObject*>(cp)->m_fields.self();
			f->tick = pl->m_gameState.m_currentProgress;
			f->p1 = PlayerState::capture(pl->m_player1);
			f->p2 = PlayerState::capture(pl->m_player2);
			f->valid = true;
			if (!runtime::quiet) log::info("checkpoint: saved at tick {} {}", f->tick, f->p1.str());
		}
	}

	void onCreated(PlayLayer* pl, CheckpointObject* cp) {
		if (!cp) return;
		if (runtime::inStep) {
			pendingCapture = cp;
		} else {
			capture(pl, cp);
		}
	}

	void onStepEnd(PlayLayer* pl) {
		if (pendingCapture) {
			capture(pl, pendingCapture);
			pendingCapture = nullptr;
		}
	}

	void onLoad(CheckpointObject* cp) {
		pendingRestore = cp;
	}

	void afterReset(PlayLayer* pl, uint32_t tick) {
		if (!pendingRestore) return;
		auto cp = pendingRestore;
		pendingRestore = nullptr;

		auto f = static_cast<DBCheckpointObject*>(cp.data())->m_fields.self();
		if (!f->valid) {
			log::warn("checkpoint: respawn at tick {} from a checkpoint without saved state", tick);
			return;
		}
		if (f->tick != tick) {
			log::warn("checkpoint: respawn tick {} != saved tick {}", tick, f->tick);
		}

		if (runtime::quiet) return;
		auto gd = PlayerState::capture(pl->m_player1);
		auto d = PlayerState::diff(gd, f->p1);
		log::info("checkpoint: GD respawn vs saved at tick {}: {}{}", tick, d.str(), d.exact() ? " (exact)" : "");
		if (!d.exact()) {
			log::info("checkpoint:   GD    {}", gd.str());
			log::info("checkpoint:   saved {}", f->p1.str());
		}

		// No restore: GD's own respawn state is the one the continuous run matches. Overwriting it
		// with this end-of-step snapshot broke ship recordings (see PLAN.md, M6 testing): in a ship
		// section GD's checkpoint was one step (1.95 units) ahead of the snapshot, and the run after
		// the respawn only lined up with an unbroken run when GD's value was kept.
	}
}
