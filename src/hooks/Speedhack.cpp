#include <Geode/Geode.hpp>
#include <Geode/modify/CCScheduler.hpp>

#include "safety/SafeMode.hpp"
#include "macro/Seek.hpp"

#include <chrono>

using namespace geode::prelude;

// Speedhack: scales the frame time the whole game receives while a level is open, and the music
// pitch (song and sound effects) with it. Physics still runs in fixed 240/s steps of game time,
// and macros are stamped by tick, so a run recorded at 0.5x plays back identically at 1x.
// An attempt played at a speed other than 1x doesn't save progress while safe mode is on.

namespace {
	float appliedPitch = 1.f;

	void setMusicPitch(float pitch) {
		if (pitch == appliedPitch) return;
		// The master channel group: the song (m_backgroundMusicChannel) and the sound effects
		// (m_globalChannel) are separate groups that both feed into it.
		auto engine = FMODAudioEngine::sharedEngine();
		if (!engine || !engine->m_system) return;
		FMOD::ChannelGroup* master = nullptr;
		if (engine->m_system->getMasterChannelGroup(&master) == FMOD_OK && master) {
			master->setPitch(pitch);
			appliedPitch = pitch;
		}
	}
}

class $modify(DBScheduler, CCScheduler) {
	void update(float dt) {
		if (db::seek::wantsFastForward()) {
			fastForward();
			return;
		}
		float speed = db::safe::speedhackFactor();
		bool inLevel = PlayLayer::get() != nullptr;
		float effective = inLevel ? speed : 1.f;
		setMusicPitch(effective);
		CCScheduler::update(dt * effective);
	}

	// Seek (M8): many virtual 240-FPS frames per rendered frame, each a full game update.
	void fastForward() {
		setMusicPitch(1.f);
		auto start = std::chrono::steady_clock::now();
		auto budget = std::chrono::milliseconds(db::seek::BUDGET_MS);
		unsigned frames = 0;
		while (db::seek::wantsFastForward()) {
			CCScheduler::update(db::seek::VIRTUAL_FRAME_DT);
			db::seek::afterVirtualFrame();
			if (++frames % 8 == 0 && std::chrono::steady_clock::now() - start >= budget) break;
		}
	}
};
