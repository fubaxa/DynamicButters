#include "dynamic/DynamicAssist.hpp"

#include <Geode/Geode.hpp>

#include <algorithm>
#include <cmath>
#include <deque>
#include <optional>
#include <vector>

#include "dynamic/WindowScan.hpp"
#include "macro/MacroManager.hpp"
#include "safety/SafeMode.hpp"

using namespace geode::prelude;

namespace db::dynamic {
	namespace {
		// A moment where the macro's player starts or stops holding, with its window in ticks.
		struct Transition {
			uint32_t frame = 0;
			bool down = false;
			bool assist = false;    // has a usable window
			uint32_t earliest = 0;  // first tick of the window
			uint32_t latest = 0;    // last tick of the window
		};

		struct Pending {
			bool down = false;
			int button = 1;
			bool isPlayer1 = true;
			uint32_t at = 0;
		};

		struct State {
			std::vector<Transition> transitions;
			Macro const* source = nullptr; // macro the transitions were built from (rebuilt when it changes)
			size_t sourceInputs = 0;
			std::vector<bool> used;     // transitions already matched to a player input this attempt
			bool keys[2] = {false, false}; // player's own key state (P1 key, P2 key)
			std::deque<Pending> pending;
			bool dispatching = false;
			uint32_t respawnTick = 0;
			std::string lastClick;      // overlay feedback
			unsigned held = 0, inWindow = 0, early = 0, late = 0, noWindow = 0;
		} s;

		void build(Macro const& macro) {
			s.transitions.clear();
			bool held[2] = {false, false};
			for (auto const& in : macro.inputs) {
				bool before = held[0] || held[1];
				held[in.player2 ? 1 : 0] = in.down;
				bool after = held[0] || held[1];
				if (before == after) continue; // the other key was already held / still held
				Transition t;
				t.frame = in.frame;
				t.down = after;
				auto const& w = in.window;
				t.assist = w.scanned && !(w.flags & (InputWindow::BaselineFailed | InputWindow::ShipSkipped));
				t.earliest = in.frame - std::min<uint32_t>(in.frame, 2u * w.early);
				t.latest = in.frame + 2u * w.late;
				s.transitions.push_back(t);
			}
			s.source = &macro;
			s.sourceInputs = macro.inputs.size();
			s.used.assign(s.transitions.size(), false);
			size_t usable = std::count_if(s.transitions.begin(), s.transitions.end(), [](Transition const& t) { return t.assist; });
			log::info("dynamic: {} presses/releases, {} with a window", s.transitions.size(), usable);
		}

		int leniencyTicks() {
			return 2 * static_cast<int>(std::clamp<int64_t>(Mod::get()->getSettingValue<int64_t>("dynamic-leniency"), 1, 20));
		}

		void queue(Pending p) {
			// Keep order: an input can't happen before one that's still waiting.
			if (!s.pending.empty()) p.at = std::max(p.at, s.pending.back().at + 2);
			s.pending.push_back(p);
		}
	}

	bool active() {
		auto& mgr = MacroManager::get();
		return scan::enabled() && mgr.isDynamic() && !scan::active() && std::abs(safe::speedhackFactor() - 1.f) < 1e-4f;
	}

	bool isDispatching() { return s.dispatching; }

	void onAttemptStart(PlayLayer* pl, uint32_t tick) {
		s.pending.clear();
		s.keys[0] = s.keys[1] = false; // GD releases the player on every respawn
		s.respawnTick = tick;
		if (!active()) return;
		auto const& play = MacroManager::get().playMacro();
		if (!play) return;
		// Rebuilt on every fresh attempt: a window scan updates the same macro in place.
		if (tick == 0 || s.source != &*play || s.sourceInputs != play->inputs.size() || s.transitions.empty()) build(*play);
		// Everything up to this point (start or checkpoint) is behind the player; the rest is open again.
		s.used.assign(s.transitions.size(), false);
		for (size_t i = 0; i < s.transitions.size() && s.transitions[i].frame <= tick; ++i) s.used[i] = true;
		if (tick == 0) {
			s.held = s.inWindow = s.early = s.late = s.noWindow = 0;
			s.lastClick.clear();
		}
	}

	bool onPlayerInput(GJBaseGameLayer* gl, bool down, int button, bool isPlayer1, uint32_t tick) {
		if (!active() || button != 1) return false;
		int key = isPlayer1 ? 0 : 1;
		// GD's own release 2 ticks after a respawn isn't the player's.
		if (!down && tick == s.respawnTick + 2 && !s.keys[key]) return false;

		bool before = s.keys[0] || s.keys[1];
		s.keys[key] = down;
		bool after = s.keys[0] || s.keys[1];
		if (before == after) {
			// Second key while the first is held, etc.: no change for the player. Still waits behind
			// a held input, or it would reach GD before it.
			if (s.pending.empty()) return false;
			queue({down, button, isPlayer1, tick});
			return true;
		}

		// Which macro press/release this is: matched by time, not by order, so a skipped or extra click
		// doesn't shift every later match. The nearest unused macro press/release of the same kind,
		// measured to its window; nothing within MATCH_RADIUS = an extra click, let through as is.
		std::optional<size_t> match;
		{
			constexpr int64_t MATCH_RADIUS = 60; // ticks (30 steps, 1/8 s) outside a window
			int64_t bestDist = MATCH_RADIUS + 1;
			int64_t t = tick;
			// Windows reach at most 20 steps (+ leniency) from their click, so only nearby clicks matter.
			int64_t from = t - 2 * 80 - MATCH_RADIUS;
			auto it = std::lower_bound(s.transitions.begin(), s.transitions.end(), from,
				[](Transition const& tr, int64_t f) { return int64_t(tr.frame) < f; });
			for (size_t c = it - s.transitions.begin(); c < s.transitions.size(); ++c) {
				auto const& tr = s.transitions[c];
				if (int64_t(tr.frame) > t + 2 * 80 + MATCH_RADIUS) break;
				if (s.used[c] || tr.down != down) continue;
				int64_t dist = t < int64_t(tr.earliest) ? int64_t(tr.earliest) - t
					: t > int64_t(tr.latest) ? t - int64_t(tr.latest) : 0;
				if (!tr.assist) dist = std::abs(t - int64_t(tr.frame)); // no window: distance to the click itself
				if (dist < bestDist) {
					bestDist = dist;
					match = c;
				}
			}
			if (match) s.used[*match] = true;
		}

		// When this input should happen.
		uint32_t at = tick;
		char const* what = down ? "press" : "release";
		if (!match) {
			s.lastClick = fmt::format("{}: no matching macro click", what);
		} else {
			auto const& tr = s.transitions[*match];
			if (!tr.assist) {
				s.noWindow++;
				s.lastClick = fmt::format("{}: no window", what);
			} else if (tick >= tr.earliest && tick <= tr.latest) {
				s.inWindow++;
				s.lastClick = fmt::format("{}: in window ({:+} steps)", what, (int(tick) - int(tr.frame)) / 2);
			} else if (tick > tr.latest) {
				s.late++;
				s.lastClick = fmt::format("{}: {} steps late", what, (tick - tr.latest) / 2);
			} else if (int(tr.earliest) - int(tick) <= leniencyTicks()) {
				s.held++;
				s.lastClick = fmt::format("{}: held {} steps", what, (tr.earliest - tick) / 2);
				at = tr.earliest;
			} else {
				s.early++;
				s.lastClick = fmt::format("{}: {} steps too early", what, (tr.earliest - tick) / 2);
			}
		}

		// Goes through now unless it's held, or something before it is still waiting (order is kept:
		// a release never happens before its held press).
		if (at <= tick && s.pending.empty()) return false;
		queue({down, button, isPlayer1, at});
		return true;
	}

	void dispatchDue(GJBaseGameLayer* gl, uint32_t tick) {
		if (!gl) return;
		while (!s.pending.empty() && s.pending.front().at <= tick) {
			auto p = s.pending.front();
			s.pending.pop_front();
			s.dispatching = true;
			gl->handleButton(p.down, p.button, p.isPlayer1);
			s.dispatching = false;
		}
	}

	std::string status() {
		if (!active()) return {};
		std::string out = "DYN";
		if (!s.lastClick.empty()) out += "  " + s.lastClick;
		return out;
	}
}
