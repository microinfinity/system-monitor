#pragma once
/// @file history_graph_renderer.hpp
/// @brief Sovereign GPU Monotone Cubic Spline History Graph Renderer on Nisaba.

#include "nisaba/gpu/gpu_canvas.hpp"
#include "nisaba/canvas/paint.hpp"
#include "nisaba/path/path_builder.hpp"
#include "nisaba/shaders/shader.hpp"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <vector>

namespace sysmon {

struct GraphSegment {
    nisaba::Point control1;
    nisaba::Point control2;
    nisaba::Point end;
};

class HistoryGraphRenderer {
public:
    static double effective_maximum(const std::vector<double>& primary,
                                    const std::vector<double>& secondary,
                                    double fixed_max,
                                    double scale_floor = 1024.0) noexcept {
        if (fixed_max > 0.0) return fixed_max;
        double peak = scale_floor;
        for (double v : primary) peak = std::max(peak, v);
        for (double v : secondary) peak = std::max(peak, v);
        return std::pow(2.0, std::ceil(std::log2(std::max(1.0, peak))));
    }

    // Fritsch-Carlson monotone cubic spline interpolation
    static std::vector<GraphSegment> build_curve(const std::vector<nisaba::Point>& pts, bool curved) {
        std::vector<GraphSegment> segments;
        const size_t count = pts.size();
        if (!curved || count < 3) {
            for (size_t i = 1; i < count; ++i) {
                segments.push_back({pts[i - 1], pts[i], pts[i]});
            }
            return segments;
        }

        std::vector<double> slopes(count - 1);
        for (size_t i = 0; i < count - 1; ++i) {
            const double dx = static_cast<double>(pts[i + 1].x - pts[i].x);
            slopes[i] = dx > 0.0 ? static_cast<double>(pts[i + 1].y - pts[i].y) / dx : 0.0;
        }

        std::vector<double> tangents(count);
        tangents[0] = slopes[0];
        tangents[count - 1] = slopes[count - 2];
        for (size_t i = 1; i < count - 1; ++i) {
            const double before = slopes[i - 1];
            const double after = slopes[i];
            if (before * after <= 0.0) {
                tangents[i] = 0.0;
                continue;
            }
            const double h0 = static_cast<double>(pts[i].x - pts[i - 1].x);
            const double h1 = static_cast<double>(pts[i + 1].x - pts[i].x);
            const double denom = h0 + h1;
            const double average = denom > 0.0 ? (before * h1 + after * h0) / denom : 0.0;
            const double sign = before > 0.0 ? 1.0 : -1.0;
            tangents[i] = sign * std::min(std::min(std::abs(before), std::abs(after)) * 2.0, std::abs(average));
        }

        for (size_t i = 0; i < count - 1; ++i) {
            const double third = static_cast<double>(pts[i + 1].x - pts[i].x) / 3.0;
            segments.push_back({
                nisaba::Point{static_cast<float>(pts[i].x + third),
                              static_cast<float>(pts[i].y + tangents[i] * third)},
                nisaba::Point{static_cast<float>(pts[i + 1].x - third),
                              static_cast<float>(pts[i + 1].y - tangents[i + 1] * third)},
                pts[i + 1]
            });
        }
        return segments;
    }

    static std::vector<nisaba::Point> build_points(const std::vector<double>& values,
                                                   const std::vector<int64_t>& timestamps,
                                                   int64_t duration_ms,
                                                   double eff_max,
                                                   const nisaba::Rect& area,
                                                   bool filled) {
        std::vector<nisaba::Point> result;
        if (values.empty() && !filled) return result;

        const double scale = eff_max > 0.0 ? eff_max : 100.0;
        const bool timed = (timestamps.size() == values.size());
        const double dur = static_cast<double>(std::max<int64_t>(1000, duration_ms));

        for (size_t i = 0; i < values.size(); ++i) {
            const int64_t age = timed
                ? (timestamps.back() - timestamps[i])
                : static_cast<int64_t>(values.size() - 1 - i) * 1000;
            const float x = static_cast<float>(area.right() - area.width() * (static_cast<double>(age) / dur));
            const float y = static_cast<float>(area.bottom() - area.height() * std::clamp(values[i] / scale, 0.0, 1.0));
            result.push_back(nisaba::Point{x, y});
        }

        if (result.empty()) {
            result = {nisaba::Point{area.left(), area.bottom()}, nisaba::Point{area.right(), area.bottom()}};
        } else if (result.front().x > area.left()) {
            const float step = result.size() > 1
                ? (result[1].x - result[0].x)
                : static_cast<float>(area.width() * 1000.0 / dur);
            const float ramp_start = std::max(area.left(), result.front().x - step);
            if (ramp_start > area.left()) {
                result.insert(result.begin(), nisaba::Point{ramp_start, area.bottom()});
            }
            result.insert(result.begin(), nisaba::Point{area.left(), area.bottom()});
        }
        return result;
    }

    static void render(nisaba::gpu::GpuCanvas& canvas,
                       const nisaba::Rect& bounds,
                       const std::vector<double>& primary_values,
                       const std::vector<double>& secondary_values,
                       const std::vector<int64_t>& timestamps,
                       int64_t duration_ms,
                       nisaba::Color primary_color,
                       nisaba::Color secondary_color,
                       std::optional<nisaba::Color> grid_color,
                       double fixed_max,
                       double scale_floor,
                       bool curved,
                       float line_width) {
        if (bounds.width() <= 2.0f || bounds.height() <= 4.0f) return;

        auto area = *nisaba::Rect::from_xywh(bounds.left() + 1.0f,
                                             bounds.top() + 2.0f,
                                             std::max(1.0f, bounds.width() - 2.0f),
                                             std::max(1.0f, bounds.height() - 4.0f));

        if (grid_color.has_value()) {
            nisaba::Paint gp(*grid_color);
            nisaba::Stroke gst{1.0f};
            for (int i = 0; i <= 4; ++i) {
                float y = area.top() + area.height() * (static_cast<float>(i) / 4.0f);
                canvas.stroke_line(area.left(), y, area.right(), y, gp, gst);
            }
        }

        const double eff_max = effective_maximum(primary_values, secondary_values, fixed_max, scale_floor);

        canvas.save();
        canvas.clip_rect(bounds);

        auto draw_series = [&](const std::vector<double>& vals, nisaba::Color col, bool filled) {
            auto pts = build_points(vals, timestamps, duration_ms, eff_max, area, filled);
            if (pts.empty()) return;

            auto segs = build_curve(pts, curved);

            if (filled) {
                nisaba::PathBuilder fill_pb;
                fill_pb.move_to(pts.front().x, pts.front().y);
                for (const auto& s : segs) {
                    fill_pb.cubic_to(s.control1.x, s.control1.y, s.control2.x, s.control2.y, s.end.x, s.end.y);
                }
                fill_pb.line_to(pts.back().x, area.bottom());
                fill_pb.line_to(pts.front().x, area.bottom());
                fill_pb.close();

                if (auto fill_path = fill_pb.finish()) {
                    auto cu = col.to_color_u8();
                    nisaba::Color top_col = nisaba::Color::from_rgba8(cu.r, cu.g, cu.b, 46); // 0.18 * 255
                    nisaba::Color bot_col = nisaba::Color::from_rgba8(cu.r, cu.g, cu.b, 5);  // 0.02 * 255
                    auto grad = nisaba::LinearGradient::create(
                        nisaba::Point{area.left(), area.top()},
                        nisaba::Point{area.left(), area.bottom()},
                        {nisaba::GradientStop::create(0.0f, top_col),
                         nisaba::GradientStop::create(1.0f, bot_col)}
                    );
                    if (grad) {
                        canvas.fill_path(*fill_path, nisaba::Paint(nisaba::Shader(*grad)));
                    } else {
                        canvas.fill_path(*fill_path, nisaba::Paint(top_col));
                    }
                }
            }

            nisaba::PathBuilder line_pb;
            line_pb.move_to(pts.front().x, pts.front().y);
            for (const auto& s : segs) {
                line_pb.cubic_to(s.control1.x, s.control1.y, s.control2.x, s.control2.y, s.end.x, s.end.y);
            }
            if (auto line_path = line_pb.finish()) {
                nisaba::Stroke lst;
                lst.width = line_width;
                lst.line_cap = nisaba::LineCap::Round;
                lst.line_join = nisaba::LineJoin::Round;
                canvas.stroke_path(*line_path, nisaba::Paint(col), lst);
            }
        };

        draw_series(primary_values, primary_color, true);
        if (!secondary_values.empty()) {
            draw_series(secondary_values, secondary_color, false);
        }

        canvas.restore();
    }
};

} // namespace sysmon

