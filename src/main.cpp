#include <Geode/Geode.hpp>

#include "macro/MacroStore.hpp"

using namespace geode::prelude;

// Hooks live in src/hooks/. M1 adds the tick probe, the tick overlay and the input log.

$on_mod(Loaded) {
	log::info("DynamicButter loaded (GD {}, Geode {})",
		GEODE_GD_VERSION_STRING, Loader::get()->getVersion().toVString());
	db::store::importLegacyDumps();

	// Speedhack hotkey (rebindable in the mod settings; works while playing).
	listenForKeybindSettingPresses("speedhack-key", [](Keybind const&, bool down, bool repeat, double) {
		if (!down || repeat) return false;
		auto mod = Mod::get();
		bool on = !mod->getSettingValue<bool>("speedhack-enabled");
		mod->setSettingValue<bool>("speedhack-enabled", on);
		auto text = on ? fmt::format("Speedhack on ({:.2f}x)", mod->getSettingValue<double>("speedhack"))
			: std::string("Speedhack off");
		Notification::create(text, NotificationIcon::Info, 1.f)->show();
		log::info("speedhack: {} via hotkey", on ? "on" : "off");
		return false;
	});
}
