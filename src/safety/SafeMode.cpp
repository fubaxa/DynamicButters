#include "safety/SafeMode.hpp"

#include <Geode/Geode.hpp>

#include <cmath>

#include "macro/MacroManager.hpp"

using namespace geode::prelude;

namespace db::safe {
	namespace {
		bool s_unsafe = false;
		std::string s_reason;
	}

	bool enabled() {
		return Mod::get()->getSettingValue<bool>("safe-mode");
	}

	float speedhackFactor() {
		auto mod = Mod::get();
		if (!mod->getSettingValue<bool>("speedhack-enabled")) return 1.f;
		float speed = static_cast<float>(mod->getSettingValue<double>("speedhack"));
		if (!(speed > 0.f) || !std::isfinite(speed)) return 1.f;
		return speed;
	}

	void markUnsafe(std::string reason) {
		if (s_unsafe) return;
		s_unsafe = true;
		s_reason = std::move(reason);
		log::info("safe-mode: attempt is unsafe ({}){}", s_reason,
			enabled() ? ", progress will not be saved" : ", but safe mode is off");
	}

	void check() {
		if (s_unsafe) return;
		if (MacroManager::get().isPlaying()) {
			markUnsafe("playback");
			return;
		}
		if (MacroManager::get().isDynamic()) {
			markUnsafe("Dynamic");
			return;
		}
		float speed = speedhackFactor();
		if (std::abs(speed - 1.f) > 1e-4f) markUnsafe(fmt::format("speedhack {:.2f}x", speed));
	}

	void onAttemptStart(uint32_t tick) {
		// Practice respawns continue the same attempt; only a restart from the start is clean.
		if (tick == 0) {
			s_unsafe = false;
			s_reason.clear();
		}
		check();
	}

	bool unsafe() { return s_unsafe; }
	std::string const& reason() { return s_reason; }
	bool blocking() { return enabled() && s_unsafe; }
}
