#include <Geode/Geode.hpp>
#include <Geode/modify/MenuLayer.hpp>

#include <filesystem>
#include <fstream>

using namespace geode::prelude;

// Object dump (setting `object-dump`, off by default): once per game start, when the main menu
// first opens, creates every object ID GD knows and writes its type and hitbox to
// `<save dir>/objects.csv`. Logging only; the objects are never added to a scene. Used by the
// separate physics simulator's level converter (which objects are solid, hazards, decoration...).
//
// `<save dir>/objects.progress` holds the ID being created: if an ID crashes GD, it's the one in
// that file, and it is skipped on the next start (listed in `objects.skip`).
namespace {
	constexpr float PROBE_X = 300.f, PROBE_Y = 300.f;

	std::filesystem::path saveFile(char const* name) {
		return std::filesystem::u8path((Mod::get()->getSaveDir() / name).string());
	}

	std::set<int> readSkip() {
		std::set<int> skip;
		std::ifstream in(saveFile("objects.skip"));
		for (int id; in >> id;) skip.insert(id);
		// A leftover progress file means the last dump crashed on that ID.
		std::ifstream prog(saveFile("objects.progress"));
		if (int id; prog >> id) {
			skip.insert(id);
			std::ofstream out(saveFile("objects.skip"), std::ios::app);
			out << id << "\n";
			log::warn("objdump: the last dump stopped at object {}; skipping it", id);
		}
		return skip;
	}

	// Some objects' setup reads the current level (seen: object 1329 crashed in
	// EffectGameObject::customSetup reading a null pointer at the main menu). Those are caught here
	// and written as failed instead of crashing the game. No C++ objects may live in this function.
	bool safeSetup(GameObject* obj) {
		__try {
			obj->customSetup();
			return true;
		} __except (1 /* EXCEPTION_EXECUTE_HANDLER */) {
			return false;
		}
	}

	void dump() {
		auto toolbox = ObjectToolbox::sharedState();
		if (!toolbox) return;
		auto skip = readSkip();
		std::ofstream out(saveFile("objects.csv"), std::ios::out | std::ios::trunc);
		if (!out.is_open()) {
			log::warn("objdump: could not open objects.csv");
			return;
		}
		out << "# DynamicButter object dump v2, gd=" << GEODE_GD_VERSION_STRING << "\n";
		out << "# rect = hitbox at scale 1, rotation 0; rectDX/DY = hitbox centre minus object position\n";
		out << "id,frame,type,rectW,rectH,rectDX,rectDY,radius,width,height,trigger,decoration,decoration2,"
			"passable,noTouch,invisible,slope,extendedCollision,skipped,spriteW,spriteH\n";
		int count = 0, failed = 0;
		for (auto const& [id, frame] : toolbox->m_allKeys) {
			if (skip.contains(id)) {
				out << id << "," << frame << ",,,,,,,,,,,,,,,,,1\n";
				continue;
			}
			{
				std::ofstream prog(saveFile("objects.progress"), std::ios::out | std::ios::trunc);
				prog << id << "\n";
			}
			auto obj = GameObject::createWithKey(id);
			if (!obj) {
				out << id << "," << frame << ",,,,,,,,,,,,,,,,,0\n";
				failed++;
				continue;
			}
			obj->setStartPos({PROBE_X, PROBE_Y});
			obj->setPosition({PROBE_X, PROBE_Y});
			// createWithKey alone leaves every object as type 0 with its sprite size as hitbox (seen
			// in dump v1); customSetup is where GD sets the type and the real hitbox per object ID.
			auto spriteSize = obj->getObjectRect().size;
			if (!safeSetup(obj)) {
				out << id << "," << frame << ",,,,,,,,,,,,,,,,,2\n";
				failed++;
				continue;
			}
			obj->setObjectRectDirty(true);
			auto const& r = obj->getObjectRect();
			out << fmt::format("{},{},{},{:.9g},{:.9g},{:.9g},{:.9g},{:.9g},{:.9g},{:.9g},{},{},{},{},{},{},{},{},0,{:.9g},{:.9g}\n",
				id, frame, static_cast<int>(obj->m_objectType), r.size.width, r.size.height,
				r.getMidX() - PROBE_X, r.getMidY() - PROBE_Y, obj->m_objectRadius, obj->m_width,
				obj->m_height, obj->m_isTrigger ? 1 : 0, obj->m_isDecoration ? 1 : 0,
				obj->m_isDecoration2 ? 1 : 0, obj->m_isPassable ? 1 : 0, obj->m_isNoTouch ? 1 : 0,
				obj->m_isInvisible ? 1 : 0, obj->m_slopeDirection, obj->m_hasExtendedCollision ? 1 : 0,
				spriteSize.width, spriteSize.height);
			count++;
		}
		out.close();
		std::error_code ec;
		std::filesystem::remove(saveFile("objects.progress"), ec);
		log::info("objdump: wrote {} objects to objects.csv ({} could not be created, {} skipped)",
			count, failed, skip.size());
	}
}

class $modify(DBObjectDumpMenu, MenuLayer) {
	bool init() {
		if (!MenuLayer::init()) return false;
		static bool done = false;
		if (!done && Mod::get()->getSettingValue<bool>("object-dump")) {
			done = true;
			dump();
		}
		return true;
	}
};
