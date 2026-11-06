#pragma once

#include <cmath>
#include <cstdint>
#include <optional>
#include <vector>

// The recorded run's x position over time, for level start positions (M8).
//
// Horizontal speed only changes at speed portals (and direction changes), so x is a straight line
// in time between those points. The path stores only the corner points; between two of them x is
// exactly linear. That gives the exact tick at which the run passed any x, which an input-only
// macro can't (inputs can be seconds apart, with speed changes in between).
namespace db {
	struct PathPoint {
		uint32_t tick = 0;
		float x = 0.f;
	};

	namespace path {
		// x per tick must change by more than this to count as a corner. Player x is a float
		// (about 0.004 resolution at x = 50000), so per-step noise stays far below this, while the
		// smallest speed change (0.5x -> 1x portal) is about 0.2 per tick.
		constexpr double CORNER_EPS = 0.02;

		// Exact (fractional) tick at which the path first reaches `x`, or nothing.
		inline std::optional<double> tickAtX(std::vector<PathPoint> const& pts, double x) {
			for (size_t i = 1; i < pts.size(); ++i) {
				auto const& a = pts[i - 1];
				auto const& b = pts[i];
				if (b.x <= a.x) continue; // not moving right on this piece
				if (x >= a.x && x <= b.x) {
					return a.tick + (x - a.x) * double(b.tick - a.tick) / (b.x - a.x);
				}
			}
			return std::nullopt;
		}

		// x of the path at `tick` (linear between corner points).
		inline std::optional<double> xAt(std::vector<PathPoint> const& pts, uint32_t tick) {
			for (size_t i = 1; i < pts.size(); ++i) {
				auto const& a = pts[i - 1];
				auto const& b = pts[i];
				if (tick >= a.tick && tick <= b.tick) {
					if (b.tick == a.tick) return b.x;
					return a.x + (double(tick) - a.tick) * (double(b.x) - a.x) / double(b.tick - a.tick);
				}
			}
			return std::nullopt;
		}
	}

	// Builds a path from per-step samples (tick, x) at the end of every physics step.
	class PathBuilder {
	public:
		void reset() {
			m_points.clear();
			m_has = false;
			m_haveSlope = false;
		}

		void add(uint32_t tick, float x) {
			if (!m_has) {
				m_points.push_back({tick, x});
				m_last = {tick, x};
				m_has = true;
				m_haveSlope = false;
				return;
			}
			if (tick <= m_last.tick) return;
			double slope = (double(x) - m_last.x) / double(tick - m_last.tick);
			if (m_haveSlope && std::abs(slope - m_slope) > path::CORNER_EPS) {
				// The previous sample was the last one on the old straight line.
				if (m_points.back().tick != m_last.tick) m_points.push_back(m_last);
			}
			m_slope = slope;
			m_haveSlope = true;
			m_last = {tick, x};
		}

		// Practice respawn at `tick`: drop everything after it and continue from the respawn state.
		void cutBack(uint32_t tick, float x) {
			if (!m_has) return;
			while (!m_points.empty() && m_points.back().tick > tick) m_points.pop_back();
			if (m_points.empty() || m_points.back().tick != tick) m_points.push_back({tick, x});
			m_last = {tick, x};
			m_haveSlope = false;
		}

		bool empty() const { return !m_has; }

		std::vector<PathPoint> finish() const {
			auto out = m_points;
			if (m_has && (out.empty() || out.back().tick != m_last.tick)) out.push_back(m_last);
			return out;
		}

	private:
		std::vector<PathPoint> m_points;
		PathPoint m_last;
		double m_slope = 0.0;
		bool m_has = false;
		bool m_haveSlope = false;
	};
}
