#include "macro/Gdr2.hpp"

#include <Geode/loader/Log.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>

namespace db::gdr2 {
	namespace {
		constexpr int FORMAT_VERSION = 2;
		constexpr int GAME_VERSION = 22091;
		constexpr uint64_t EXT_VERSION = 1;
		constexpr char const* PHYS_TAG = "Phys";
		constexpr uint64_t MAX_STRING = 0xFFFF; // same limit as the reference reader

		class Writer {
		public:
			std::vector<uint8_t> bytes;

			void varint(uint64_t v) {
				do {
					uint8_t b = v & 0x7F;
					v >>= 7;
					if (v) b |= 0x80;
					bytes.push_back(b);
				} while (v);
			}
			void str(std::string const& s) {
				varint(s.size());
				bytes.insert(bytes.end(), s.begin(), s.end());
			}
			template <typename T>
			void raw(T value) { // big-endian, like the reference implementation
				uint8_t buf[sizeof(T)];
				std::memcpy(buf, &value, sizeof(T));
				if constexpr (std::endian::native == std::endian::little) std::reverse(buf, buf + sizeof(T));
				bytes.insert(bytes.end(), buf, buf + sizeof(T));
			}
			void block(Writer const& other) {
				varint(other.bytes.size());
				bytes.insert(bytes.end(), other.bytes.begin(), other.bytes.end());
			}
		};

		class Reader {
		public:
			explicit Reader(std::span<uint8_t const> data) : m_data(data) {}

			bool ok() const { return !m_failed; }
			size_t left() const { return m_failed ? 0 : m_data.size() - m_pos; }

			uint64_t varint() {
				uint64_t v = 0;
				for (int shift = 0; shift < 64; shift += 7) {
					if (!need(1)) return 0;
					uint8_t b = m_data[m_pos++];
					v |= uint64_t(b & 0x7F) << shift;
					if (!(b & 0x80)) return v;
				}
				m_failed = true; // more than 10 bytes
				return 0;
			}
			std::string str() {
				auto n = varint();
				if (n > MAX_STRING || !need(n)) { m_failed = true; return {}; }
				std::string s(reinterpret_cast<char const*>(m_data.data() + m_pos), n);
				m_pos += n;
				return s;
			}
			template <typename T>
			T raw() {
				T value{};
				if (!need(sizeof(T))) return value;
				uint8_t buf[sizeof(T)];
				std::memcpy(buf, m_data.data() + m_pos, sizeof(T));
				if constexpr (std::endian::native == std::endian::little) std::reverse(buf, buf + sizeof(T));
				std::memcpy(&value, buf, sizeof(T));
				m_pos += sizeof(T);
				return value;
			}
			// Size-prefixed block: returns a reader over it and skips past it.
			Reader block() {
				auto n = varint();
				if (!need(n)) return Reader({});
				Reader sub(m_data.subspan(m_pos, n));
				m_pos += n;
				return sub;
			}
			void bytes(char* out, size_t n) {
				if (!need(n)) return;
				std::memcpy(out, m_data.data() + m_pos, n);
				m_pos += n;
			}

		private:
			bool need(uint64_t n) {
				if (m_failed || n > m_data.size() - m_pos) { m_failed = true; return false; }
				return true;
			}
			std::span<uint8_t const> m_data;
			size_t m_pos = 0;
			bool m_failed = false;
		};

		uint64_t toFileFrame(uint32_t tick) { return tick / 2; }
	}

	std::vector<uint8_t> write(Macro const& macro) {
		Writer w;
		w.bytes = {'G', 'D', 'R'};
		w.varint(FORMAT_VERSION);
		w.str(PHYS_TAG);
		w.str(macro.author);
		w.str(""); // description
		w.raw<float>(static_cast<float>(macro.endFrame / (2.0 * macro.framerate)));
		w.varint(GAME_VERSION);
		w.raw<double>(macro.framerate);
		w.varint(static_cast<uint32_t>(macro.seed)); // as uint32 so negative seeds stay 5 bytes
		w.varint(0);     // coins
		w.varint(0);     // ldm
		w.varint(0);     // platformer
		w.str(BOT_NAME);
		w.varint(BOT_VERSION);
		w.varint(static_cast<uint32_t>(macro.levelID));
		w.str(macro.levelName);

		Writer ext;
		ext.varint(EXT_VERSION);
		ext.varint(macro.endFrame);
		ext.varint(static_cast<uint64_t>(std::max<int64_t>(macro.recordedAt, 0)));
		w.block(ext);

		w.varint(0); // deaths

		size_t odd = 0;
		size_t p1Count = std::count_if(macro.inputs.begin(), macro.inputs.end(),
			[](MacroInput const& in) { return !in.player2; });
		w.varint(macro.inputs.size());
		w.varint(p1Count);
		for (bool p2 : {false, true}) {
			uint64_t prev = 0;
			for (auto const& in : macro.inputs) {
				if (in.player2 != p2) continue;
				if (in.frame & 1) odd++;
				uint64_t frame = toFileFrame(in.frame);
				w.varint(((frame - prev) << 1) | (in.down ? 1 : 0));
				prev = frame;

				Writer phys;
				phys.raw<float>(in.phys.x);
				phys.raw<float>(in.phys.y);
				phys.raw<float>(in.phys.rotation);
				phys.raw<double>(in.phys.xVel);
				phys.raw<double>(in.phys.yVel);
				w.block(phys);
			}
		}
		if (odd) {
			geode::log::warn("gdr2: {} inputs were on a half-step (odd tick) and were rounded down", odd);
		}
		return std::move(w.bytes);
	}

	geode::Result<Macro> read(std::span<uint8_t const> data) {
		Reader r(data);
		char magic[3] = {};
		r.bytes(magic, 3);
		if (!r.ok() || std::memcmp(magic, "GDR", 3) != 0) return geode::Err("not a GDR file");

		auto version = r.varint();
		if (version != 2) return geode::Err(fmt::format("unsupported GDR version {}", version));
		auto inputTag = r.str();

		Macro m;
		m.author = r.str();
		r.str(); // description
		r.raw<float>(); // duration (recomputed from endFrame when ours)
		auto gameVersion = r.varint();
		m.framerate = r.raw<double>();
		m.seed = static_cast<int>(static_cast<uint32_t>(r.varint()));
		r.varint(); // coins
		r.varint(); // ldm
		bool platformer = r.varint() != 0;
		m.botName = r.str();
		r.varint(); // bot version
		m.levelID = static_cast<int>(static_cast<uint32_t>(r.varint()));
		m.levelName = r.str();

		auto ext = r.block();
		if (m.botName == BOT_NAME) {
			if (ext.varint() >= 1) {
				m.endFrame = static_cast<uint32_t>(ext.varint());
				m.recordedAt = static_cast<int64_t>(ext.varint());
			}
		}

		auto deathCount = r.varint();
		for (uint64_t i = 0; i < deathCount && r.ok(); ++i) r.varint();

		auto total = r.varint();
		auto p1Count = r.varint();
		if (!r.ok()) return geode::Err("file is truncated (header)");
		if (p1Count > total || total > r.left()) return geode::Err("bad input counts");

		if (!(m.framerate > 0)) m.framerate = 240.0;
		// File frames are physics steps at `framerate`; ticks are half-steps at 240 steps/s.
		double tickScale = 2.0 * 240.0 / m.framerate;
		if (m.framerate != 240.0) {
			geode::log::warn("gdr2: replay framerate is {} (not 240); frames are scaled, playback may not be exact",
				m.framerate);
		}

		bool hasPhys = inputTag == PHYS_TAG;
		m.inputs.reserve(total);
		uint64_t frame = 0;
		for (uint64_t i = 0; i < total; ++i) {
			if (i == p1Count) frame = 0; // P2 list starts, deltas restart
			uint64_t packed = r.varint();
			MacroInput in;
			if (platformer) {
				frame += packed >> 3;
				in.button = static_cast<uint8_t>((packed >> 1) & 3);
			} else {
				frame += packed >> 1;
				in.button = 1;
			}
			in.down = packed & 1;
			in.player2 = i >= p1Count;
			in.frame = static_cast<uint32_t>(std::llround(frame * tickScale));
			if (!inputTag.empty()) {
				auto e = r.block();
				if (hasPhys) {
					in.phys.x = e.raw<float>();
					in.phys.y = e.raw<float>();
					in.phys.rotation = e.raw<float>();
					in.phys.xVel = e.raw<double>();
					in.phys.yVel = e.raw<double>();
				}
			}
			if (!r.ok()) return geode::Err(fmt::format("file is truncated (input {} of {})", i, total));
			m.inputs.push_back(in);
		}
		std::stable_sort(m.inputs.begin(), m.inputs.end(),
			[](MacroInput const& a, MacroInput const& b) { return a.frame < b.frame; });

		if (platformer) {
			auto before = m.inputs.size();
			std::erase_if(m.inputs, [](MacroInput const& in) { return in.button != 1; });
			geode::log::warn("gdr2: platformer replay; dropped {} left/right inputs (only jump is supported)",
				before - m.inputs.size());
		}
		if (gameVersion != 0 && gameVersion != GAME_VERSION) {
			geode::log::info("gdr2: replay was recorded on game version {}", gameVersion);
		}
		return geode::Ok(std::move(m));
	}
}
