#include "macro/MacroStore.hpp"

#include <Geode/Geode.hpp>
#include <Geode/utils/file.hpp>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <sstream>

#include "macro/Gdr2.hpp"

using namespace geode::prelude;

namespace db::store {
	namespace {
		std::string legacyDir() {
			return (Mod::get()->getSaveDir() / "recordings").string();
		}

		std::string join(std::string const& dir, std::string const& name) {
			return dir + "/" + name;
		}

		bool exists(std::string const& path) {
			std::error_code ec;
			return std::filesystem::exists(std::filesystem::u8path(path), ec);
		}

		// File-name-safe version of a level name (ASCII letters, digits, space, - _ . ( )).
		std::string sanitize(std::string const& name) {
			std::string out;
			for (unsigned char c : name) {
				out += (std::isalnum(c) || c == ' ' || c == '-' || c == '_' || c == '(' || c == ')') ? char(c) : '_';
			}
			while (!out.empty() && (out.back() == ' ' || out.back() == '.')) out.pop_back();
			if (out.empty()) out = "macro";
			return out;
		}

		std::string uniquePath(std::string const& dir, std::string const& base) {
			auto path = join(dir, base + gdr2::FILE_EXTENSION);
			for (int n = 2; exists(path); ++n) {
				path = join(dir, fmt::format("{} ({}){}", base, n, gdr2::FILE_EXTENSION));
			}
			return path;
		}

		Result<> writeMacro(std::string const& path, Macro const& macro) {
			auto dir = macrosDir();
			if (auto res = file::createDirectoryAll(dir); !res) {
				return Err(fmt::format("could not create {}: {}", dir, res.unwrapErr()));
			}
			auto bytes = gdr2::write(macro);
			if (auto res = file::writeBinary(path, bytes); !res) {
				return Err(fmt::format("could not write {}: {}", path, res.unwrapErr()));
			}
			return Ok();
		}

		// Parses one text dump written by M2-M5's writeDump.
		std::optional<Macro> parseLegacyDump(std::string const& path, std::string const& text) {
			Macro macro;
			macro.author.clear();
			std::istringstream lines(text);
			std::string line;
			bool header = false;
			while (std::getline(lines, line)) {
				if (line.empty()) continue;
				if (line[0] == '#') {
					// "# level <id> '<name>', framerate <f>, end tick <t>, <n> inputs"
					if (line.starts_with("# level ")) {
						header = true;
						std::istringstream h(line.substr(8));
						h >> macro.levelID;
						auto q1 = line.find('\'');
						auto q2 = line.rfind("', framerate");
						if (q1 != std::string::npos && q2 != std::string::npos && q2 > q1) {
							macro.levelName = line.substr(q1 + 1, q2 - q1 - 1);
						}
						if (auto pos = line.find("end tick "); pos != std::string::npos) {
							macro.endFrame = static_cast<uint32_t>(std::stoul(line.substr(pos + 9)));
						}
					}
					continue;
				}
				std::istringstream row(line);
				MacroInput in;
				std::string action, key;
				if (!(row >> in.frame >> action >> key >> in.phys.x >> in.phys.y >> in.phys.rotation
					>> in.phys.xVel >> in.phys.yVel)) {
					log::warn("import: skipping bad line in {}: {}", path, line);
					continue;
				}
				in.down = action == "PRESS";
				in.player2 = key == "P2";
				macro.inputs.push_back(in);
			}
			if (!header) return std::nullopt;
			return macro;
		}
	}

	std::string macrosDir() {
		return (Mod::get()->getSaveDir() / "macros").string();
	}

	std::string fileNameOf(std::string const& path) {
		auto slash = path.find_last_of("/\\");
		return slash == std::string::npos ? path : path.substr(slash + 1);
	}

	Result<Macro> load(std::string const& path) {
		auto bytes = file::readBinary(path);
		if (!bytes) return Err(fmt::format("could not read: {}", bytes.unwrapErr()));
		auto data = bytes.unwrap();
		return gdr2::read(std::span<uint8_t const>(data.data(), data.size()));
	}

	std::vector<Entry> list() {
		std::vector<Entry> out;
		auto entries = file::readDirectory(macrosDir());
		if (!entries) return out;
		for (auto const& p : entries.unwrap()) {
			auto path = p.string();
			auto name = fileNameOf(path);
			if (!name.ends_with(gdr2::FILE_EXTENSION)) continue;
			auto macro = load(path);
			if (!macro) {
				log::warn("macros: skipping {}: {}", name, macro.unwrapErr());
				continue;
			}
			out.push_back({path, name, std::move(macro).unwrap()});
		}
		std::sort(out.begin(), out.end(), [](Entry const& a, Entry const& b) {
			if (a.macro.recordedAt != b.macro.recordedAt) return a.macro.recordedAt > b.macro.recordedAt;
			return a.fileName < b.fileName;
		});
		return out;
	}

	Result<std::string> save(Macro const& macro) {
		auto path = uniquePath(macrosDir(), sanitize(macro.levelName));
		GEODE_UNWRAP(writeMacro(path, macro));
		return Ok(path);
	}

	Result<> overwrite(std::string const& path, Macro const& macro) {
		return writeMacro(path, macro);
	}

	std::optional<Entry> latestFor(int levelID, std::string const& levelName) {
		for (auto& e : list()) {
			if (e.macro.isFor(levelID, levelName)) return std::move(e);
		}
		return std::nullopt;
	}

	void importLegacyDumps() {
		auto entries = file::readDirectory(legacyDir());
		if (!entries) return;
		auto dir = macrosDir();
		int imported = 0;
		for (auto const& p : entries.unwrap()) {
			auto path = p.string();
			auto name = fileNameOf(path);
			// "<levelID>_<unix seconds>_complete.txt"
			if (!name.ends_with("_complete.txt")) continue;
			auto stem = name.substr(0, name.size() - std::string("_complete.txt").size());
			auto target = join(dir, "imported_" + stem + gdr2::FILE_EXTENSION);
			if (exists(target)) continue;

			auto text = file::readString(path);
			if (!text) continue;
			auto macro = parseLegacyDump(path, text.unwrap());
			if (!macro) {
				log::warn("import: {} has no header, skipped", name);
				continue;
			}
			if (auto us = stem.find('_'); us != std::string::npos) {
				try { macro->recordedAt = std::stoll(stem.substr(us + 1)); } catch (...) {}
			}
			if (auto res = writeMacro(target, *macro); !res) {
				log::error("import: {}", res.unwrapErr());
				continue;
			}
			imported++;
			log::info("import: {} -> {} (level {} '{}', {} inputs)",
				name, fileNameOf(target), macro->levelID, macro->levelName, macro->inputs.size());
		}
		if (imported) log::info("import: converted {} old text recordings to .gdr2", imported);
	}
}
