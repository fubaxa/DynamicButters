#include <Geode/Geode.hpp>
#include <Geode/modify/CCScheduler.hpp>

#include <cmath>

using namespace geode::prelude;

// Speedhack: scales the frame time the whole game receives while a level is open, and the music
// pitch with it. Physics still runs in fixed 240/s steps of game time, and macros are stamped by
// tick, so a run recorded at 0.5x plays back identically at 1x.
// Any attempt played at a speed other than 1x must not save progress (safe mode, M7).

namespace {
	float appliedPitch = 1.f;

	void setMusicPitch(float pitch) {
		if (pitch == appliedPitch) return;
		auto engine = FMODAudioEngine::sharedEngine();
		if (engine && engine->m_globalChannel) {
			engine->m_globalChannel->setPitch(pitch);
			appliedPitch = pitch;
		}
	}
}

class $modify(DBScheduler, CCScheduler) {
	void update(float dt) {
		float speed = static_cast<float>(Mod::get()->getSettingValue<double>("speedhack"));
		if (!(speed > 0.f) || !std::isfinite(speed)) speed = 1.f;
		bool inLevel = PlayLayer::get() != nullptr;
		float effective = inLevel ? speed : 1.f;
		setMusicPitch(effective);
		CCScheduler::update(dt * effective);
	}
};
