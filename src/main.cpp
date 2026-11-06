#include <Geode/Geode.hpp>

using namespace geode::prelude;

// Hooks live in src/hooks/. M1 adds the tick probe, the tick overlay and the input log.

$on_mod(Loaded) {
	log::info("DynamicButter loaded (GD {}, Geode {})",
		GEODE_GD_VERSION_STRING, Loader::get()->getVersion().toVString());
}
