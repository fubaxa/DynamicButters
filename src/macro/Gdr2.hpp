#pragma once

#include <Geode/Result.hpp>

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "macro/Macro.hpp"

// GDR2 (maxnut's GD Replay Format 2) reader and writer, written from the format's spec and the
// reference implementation's behaviour (the reference library itself isn't used; licence unclear).
//
// Layout (varint = LEB128; float/double = raw IEEE bytes, big-endian; bool = varint 0/1):
//   "GDR" | version=2 | inputTag (string) | author | description | duration (float, s)
//   gameVersion (varint, e.g. 22091) | framerate (double) | seed | coins | ldm | platformer
//   botName | botVersion | levelID | levelName
//   replayExtension: varint size + bytes      (ours when botName == "DynamicButter", see below)
//   deaths: count + delta-encoded frames
//   inputs: total, p1Count, p1 records..., p2 records...
//   record: varint (frameDelta << 1 | down)   (platformer: delta << 3 | button << 1 | down)
//           + if inputTag is set: varint size + bytes  ("Phys": x, y, rot float; xVel, yVel double)
//
// Frames: GDR2 counts physics steps (240/s). Our ticks (m_currentProgress) count half-steps, so
// file frame = tick / 2. With Click Between Steps forced off every tick is even.
//
// DynamicButter replay extension: extVersion, endTick, recordedAt (unix s); version 2 adds
// start ticks (count + delta-encoded ticks, M8 seek points); version 3 adds the x path (count +
// (tick delta, x float) corner points); version 4 adds Dynamic windows (count + (tick delta,
// key bits, early, late, flags) per scanned input); version 5 adds earlyExact, lateExact per entry
// (v4 entries were survival-based and are ignored when read).
// Other readers skip it by its size prefix.

namespace db::gdr2 {
	constexpr char const* FILE_EXTENSION = ".gdr2";
	constexpr char const* BOT_NAME = "DynamicButter";
	constexpr int BOT_VERSION = 1;

	std::vector<uint8_t> write(Macro const& macro);
	geode::Result<Macro> read(std::span<uint8_t const> data);
}
