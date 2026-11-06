#pragma once

#include <Geode/Result.hpp>

#include <optional>
#include <string>
#include <vector>

#include "macro/Macro.hpp"

// Macro files on disk: <mod save dir>/macros/*.gdr2.
namespace db::store {
	struct Entry {
		std::string path;     // full path
		std::string fileName; // e.g. "Stereo Madness.gdr2"
		Macro macro;
	};

	std::string macrosDir();
	std::string fileNameOf(std::string const& path);

	// Every readable .gdr2 file, newest first (recordedAt, then file name). Unreadable files are logged and skipped.
	std::vector<Entry> list();
	geode::Result<Macro> load(std::string const& path);
	// Saves as "<level name>.gdr2", or "<level name> (2).gdr2" etc. if taken. Returns the path.
	geode::Result<std::string> save(Macro const& macro);
	// Overwrites an existing macro file (e.g. after editing its start positions).
	geode::Result<> overwrite(std::string const& path, Macro const& macro);
	// Newest file recorded on this level (by ID, and by name for local levels, which all have ID 0).
	std::optional<Entry> latestFor(int levelID, std::string const& levelName);

	// One-time conversion of the M2-M5 text dumps (recordings/*.txt) to .gdr2. Already converted
	// dumps are skipped; the text files are left in place.
	void importLegacyDumps();
}
