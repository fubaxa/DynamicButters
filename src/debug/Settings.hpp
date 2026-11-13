#pragma once

#include <Geode/loader/Mod.hpp>

namespace db::settings {
	inline bool showTickCounter() {
		return geode::Mod::get()->getSettingValue<bool>("show-tick-counter");
	}
	inline bool logInputs() {
		return geode::Mod::get()->getSettingValue<bool>("log-inputs");
	}
	inline bool tickProbe() {
		return geode::Mod::get()->getSettingValue<bool>("tick-probe");
	}
	inline bool checkpointTest() {
		return geode::Mod::get()->getSettingValue<bool>("checkpoint-test");
	}
	inline bool physicsCorrection() {
		return geode::Mod::get()->getSettingValue<bool>("physics-correction");
	}
	inline bool startPosCorrection() {
		return geode::Mod::get()->getSettingValue<bool>("startpos-correction");
	}
	inline bool forceDisableCbs() {
		return geode::Mod::get()->getSettingValue<bool>("force-disable-cbs");
	}
}
