#pragma once
/// @file theme.hpp
/// @brief Standalone Sovereign Light & Dark Mode Design System & Theme Model for Nisaba Engine.
/// Completely decoupled from external desktop environment theme state files.

#include "nisaba/color/color.hpp"
#include "nisaba/math/rect.hpp"
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace sysmon {

namespace detail {

[[nodiscard]] constexpr nisaba::Color rgb_hex(uint32_t hex, uint8_t alpha = 255) noexcept {
    return nisaba::Color::from_rgba8(
        static_cast<uint8_t>((hex >> 16) & 0xFF),
        static_cast<uint8_t>((hex >> 8) & 0xFF),
        static_cast<uint8_t>(hex & 0xFF),
        alpha
    );
}

[[nodiscard]] constexpr nisaba::Color with_alpha(const nisaba::Color& c, float alpha) noexcept {
    float a = alpha < 0.0f ? 0.0f : (alpha > 1.0f ? 1.0f : alpha);
    return nisaba::Color::from_rgba_unchecked(c.red(), c.green(), c.blue(), a);
}

[[nodiscard]] inline nisaba::Color tint_color(const nisaba::Color& base, const nisaba::Color& overlay) noexcept {
    auto bu = base.to_color_u8();
    auto ou = overlay.to_color_u8();
    const float alpha = static_cast<float>(ou.a) / 255.0f;
    auto mix = [alpha](uint8_t from, uint8_t to) -> uint8_t {
        float v = static_cast<float>(from) + (static_cast<float>(to) - static_cast<float>(from)) * alpha;
        return static_cast<uint8_t>(std::clamp(v + 0.5f, 0.0f, 255.0f));
    };
    return nisaba::Color::from_rgba8(mix(bu.r, ou.r), mix(bu.g, ou.g), mix(bu.b, ou.b), bu.a);
}

[[nodiscard]] inline nisaba::Color lerp_color(const nisaba::Color& a, const nisaba::Color& b, float t) noexcept {
    t = std::clamp(t, 0.0f, 1.0f);
    auto ca = a.to_color_u8();
    auto cb = b.to_color_u8();
    auto mix = [t](uint8_t x, uint8_t y) -> uint8_t {
        float v = static_cast<float>(x) + (static_cast<float>(y) - static_cast<float>(x)) * t;
        return static_cast<uint8_t>(std::clamp(v + 0.5f, 0.0f, 255.0f));
    };
    return nisaba::Color::from_rgba8(mix(ca.r, cb.r), mix(ca.g, cb.g), mix(ca.b, cb.b), mix(ca.a, cb.a));
}

} // namespace detail

/// Resource Accent Palette
namespace accent {
    inline constexpr nisaba::Color blue   = detail::rgb_hex(0x3385FF);
    inline constexpr nisaba::Color red    = detail::rgb_hex(0xFF5C6D);
    inline constexpr nisaba::Color green  = detail::rgb_hex(0x35BF56);
    inline constexpr nisaba::Color purple = detail::rgb_hex(0x8266FF);
    inline constexpr nisaba::Color pink   = detail::rgb_hex(0xCA64AC);
    inline constexpr nisaba::Color orange = detail::rgb_hex(0xFEA042);
    inline constexpr nisaba::Color grey   = detail::rgb_hex(0x4F596B);
} // namespace accent

/// Complete Light & Dark Mode Theme Specification (`AppTheme::light()` and `AppTheme::dark()`)
struct AppTheme {
    bool darkMode{false};

    // Core Appearance Colors
    nisaba::Color background;
    nisaba::Color secondBackground;
    nisaba::Color alternateBackground;
    nisaba::Color text;
    nisaba::Color disabledText;
    nisaba::Color highlight;
    nisaba::Color highlightedText;

    // Semantic & Widget Colors
    nisaba::Color mutedPlate;
    nisaba::Color separator;
    nisaba::Color headerSeparator;
    nisaba::Color destructiveAction;
    nisaba::Color segmentedControlTrack;
    nisaba::Color segmentedControlPlate;
    nisaba::Color segmentedControlPlateShadow;
    nisaba::Color segmentedControlTextPressed;
    nisaba::Color scrollBarThumb;
    nisaba::Color scrollBarThumbDragging;
    nisaba::Color textFieldPlaceholder;
    nisaba::Color textFieldFocusRing;
    nisaba::Color alertBackground;
    nisaba::Color alertText;
    nisaba::Color dialogButtonBackground;
    nisaba::Color dialogButtonBackgroundPressed;
    nisaba::Color dialogButtonDestructiveBg;
    nisaba::Color dialogButtonDestructiveText;
    nisaba::Color menuHighlight;
    nisaba::Color menuSeparator;
    nisaba::Color menuDisabledText;
    nisaba::Color toastShadow;

    // Exact Metrics (logical pixels)
    float smallRadius  = 8.0f;
    float mediumRadius = 10.0f;
    float bigRadius    = 12.0f;
    float hugeRadius   = 14.0f;
    float windowRadius = 14.0f;

    float smallSpacing  = 6.0f;
    float mediumSpacing = 9.0f;
    float largeSpacing  = 12.0f;

    /// Converts point size to logical pixels (96 DPI: points * 4 / 3)
    [[nodiscard]] static constexpr float points(float pt) noexcept {
        return pt * 4.0f / 3.0f;
    }

    /// Light Mode Theme (`AppTheme::light()`)
    [[nodiscard]] static constexpr AppTheme light() noexcept {
        AppTheme t;
        t.darkMode            = false;
        t.background          = detail::rgb_hex(0xF5F6FA);
        t.secondBackground    = detail::rgb_hex(0xFFFFFF);
        t.alternateBackground = detail::rgb_hex(0xF2F4F5);
        t.text                = detail::rgb_hex(0x323238);
        t.disabledText        = detail::rgb_hex(0x64646E);
        t.highlight           = accent::blue;
        t.highlightedText     = detail::rgb_hex(0xFFFFFF);

        t.mutedPlate                  = detail::with_alpha(detail::rgb_hex(0x000000), 0.03f);
        t.separator                   = detail::with_alpha(t.disabledText, 0.10f);
        t.headerSeparator             = detail::with_alpha(t.text, 0.07f);
        t.destructiveAction           = detail::rgb_hex(0xD92D20);
        t.segmentedControlTrack       = detail::with_alpha(detail::rgb_hex(0x000000), 0.04f);
        t.segmentedControlPlate       = t.secondBackground;
        t.segmentedControlPlateShadow = detail::with_alpha(detail::rgb_hex(0x000000), 0.12f);
        t.segmentedControlTextPressed = detail::with_alpha(t.text, 0.50f);
        t.scrollBarThumb              = detail::with_alpha(detail::rgb_hex(0x000000), 0.40f);
        t.scrollBarThumbDragging      = detail::with_alpha(detail::rgb_hex(0x000000), 0.50f);
        t.textFieldPlaceholder        = detail::with_alpha(t.text, 0.40f);
        t.textFieldFocusRing          = detail::with_alpha(t.highlight, 0.25f);
        t.alertBackground             = t.secondBackground;
        t.alertText                   = detail::with_alpha(detail::rgb_hex(0x000000), 0.847f);
        t.dialogButtonBackground      = detail::with_alpha(detail::rgb_hex(0x000000), 0.0756f);
        t.dialogButtonBackgroundPressed = detail::with_alpha(detail::rgb_hex(0x000000), 0.165f);
        t.dialogButtonDestructiveText = detail::rgb_hex(0xEB4C46);
        t.dialogButtonDestructiveBg   = detail::with_alpha(detail::rgb_hex(0xEB4C46), 0.25f);
        t.menuHighlight               = detail::with_alpha(t.highlight, 0.90f);
        t.menuSeparator               = detail::with_alpha(detail::rgb_hex(0x000000), 0.10f);
        t.menuDisabledText            = detail::with_alpha(t.text, 0.40f);
        t.toastShadow                 = detail::with_alpha(detail::rgb_hex(0x000000), 0.14f);
        return t;
    }

    /// Dark Mode Theme (`AppTheme::dark()`)
    [[nodiscard]] static constexpr AppTheme dark() noexcept {
        AppTheme t;
        t.darkMode            = true;
        t.background          = detail::rgb_hex(0x1C1C1D);
        t.secondBackground    = detail::rgb_hex(0x2C2C2D);
        t.alternateBackground = detail::rgb_hex(0x3C3C3D);
        t.text                = detail::rgb_hex(0xFFFFFF);
        t.disabledText        = detail::rgb_hex(0x888888);
        t.highlight           = accent::blue;
        t.highlightedText     = detail::rgb_hex(0xFFFFFF);

        t.mutedPlate                  = detail::with_alpha(detail::rgb_hex(0xFFFFFF), 0.05f);
        t.separator                   = detail::with_alpha(t.disabledText, 0.30f);
        t.headerSeparator             = detail::with_alpha(t.text, 0.07f);
        t.destructiveAction           = detail::rgb_hex(0xFF453A);
        t.segmentedControlTrack       = detail::with_alpha(detail::rgb_hex(0xFFFFFF), 0.06f);
        t.segmentedControlPlate       = detail::rgb_hex(0x58585A);
        t.segmentedControlPlateShadow = detail::with_alpha(detail::rgb_hex(0x000000), 0.18f);
        t.segmentedControlTextPressed = detail::with_alpha(t.text, 0.50f);
        t.scrollBarThumb              = detail::with_alpha(detail::rgb_hex(0xFFFFFF), 0.50f);
        t.scrollBarThumbDragging      = detail::with_alpha(detail::rgb_hex(0xFFFFFF), 0.40f);
        t.textFieldPlaceholder        = detail::with_alpha(t.text, 0.40f);
        t.textFieldFocusRing          = detail::with_alpha(t.highlight, 0.25f);
        t.alertBackground             = detail::rgb_hex(0x343434);
        t.alertText                   = detail::with_alpha(detail::rgb_hex(0xFFFFFF), 0.847f);
        t.dialogButtonBackground      = detail::with_alpha(detail::rgb_hex(0xFFFFFF), 0.0756f);
        t.dialogButtonBackgroundPressed = detail::with_alpha(detail::rgb_hex(0xFFFFFF), 0.165f);
        t.dialogButtonDestructiveText = detail::rgb_hex(0xDC3929);
        t.dialogButtonDestructiveBg   = detail::with_alpha(detail::rgb_hex(0xEC544C), 0.25f);
        t.menuHighlight               = detail::with_alpha(t.highlight, 0.82f);
        t.menuSeparator               = detail::with_alpha(detail::rgb_hex(0xFFFFFF), 0.14f);
        t.menuDisabledText            = detail::with_alpha(t.text, 0.40f);
        t.toastShadow                 = detail::with_alpha(detail::rgb_hex(0x000000), 0.14f);
        return t;
    }

    /// Smooth interpolation between Light and Dark themes
    [[nodiscard]] static AppTheme lerp(const AppTheme& a, const AppTheme& b, float t) noexcept {
        AppTheme res = (t < 0.5f) ? a : b;
        res.darkMode                    = b.darkMode;
        res.background                  = detail::lerp_color(a.background,                  b.background,                  t);
        res.secondBackground            = detail::lerp_color(a.secondBackground,            b.secondBackground,            t);
        res.alternateBackground         = detail::lerp_color(a.alternateBackground,         b.alternateBackground,         t);
        res.text                        = detail::lerp_color(a.text,                        b.text,                        t);
        res.disabledText                = detail::lerp_color(a.disabledText,                b.disabledText,                t);
        res.highlight                   = detail::lerp_color(a.highlight,                   b.highlight,                   t);
        res.highlightedText             = detail::lerp_color(a.highlightedText,             b.highlightedText,             t);
        res.mutedPlate                  = detail::lerp_color(a.mutedPlate,                  b.mutedPlate,                  t);
        res.separator                   = detail::lerp_color(a.separator,                   b.separator,                   t);
        res.headerSeparator             = detail::lerp_color(a.headerSeparator,             b.headerSeparator,             t);
        res.destructiveAction           = detail::lerp_color(a.destructiveAction,           b.destructiveAction,           t);
        res.segmentedControlTrack       = detail::lerp_color(a.segmentedControlTrack,       b.segmentedControlTrack,       t);
        res.segmentedControlPlate       = detail::lerp_color(a.segmentedControlPlate,       b.segmentedControlPlate,       t);
        res.segmentedControlPlateShadow = detail::lerp_color(a.segmentedControlPlateShadow, b.segmentedControlPlateShadow, t);
        res.segmentedControlTextPressed = detail::lerp_color(a.segmentedControlTextPressed, b.segmentedControlTextPressed, t);
        res.scrollBarThumb              = detail::lerp_color(a.scrollBarThumb,              b.scrollBarThumb,              t);
        res.scrollBarThumbDragging      = detail::lerp_color(a.scrollBarThumbDragging,      b.scrollBarThumbDragging,      t);
        res.textFieldPlaceholder        = detail::lerp_color(a.textFieldPlaceholder,        b.textFieldPlaceholder,        t);
        res.textFieldFocusRing          = detail::lerp_color(a.textFieldFocusRing,          b.textFieldFocusRing,          t);
        res.alertBackground             = detail::lerp_color(a.alertBackground,             b.alertBackground,             t);
        res.alertText                   = detail::lerp_color(a.alertText,                   b.alertText,                   t);
        res.dialogButtonBackground      = detail::lerp_color(a.dialogButtonBackground,      b.dialogButtonBackground,      t);
        res.dialogButtonBackgroundPressed = detail::lerp_color(a.dialogButtonBackgroundPressed, b.dialogButtonBackgroundPressed, t);
        res.dialogButtonDestructiveBg   = detail::lerp_color(a.dialogButtonDestructiveBg,   b.dialogButtonDestructiveBg,   t);
        res.dialogButtonDestructiveText = detail::lerp_color(a.dialogButtonDestructiveText, b.dialogButtonDestructiveText, t);
        res.menuHighlight               = detail::lerp_color(a.menuHighlight,               b.menuHighlight,               t);
        res.menuSeparator               = detail::lerp_color(a.menuSeparator,               b.menuSeparator,               t);
        res.menuDisabledText            = detail::lerp_color(a.menuDisabledText,            b.menuDisabledText,            t);
        res.toastShadow                 = detail::lerp_color(a.toastShadow,                 b.toastShadow,                 t);
        return res;
    }
};

struct UnitScale {
    float u = 1.0f;
    float screen_w = 907.0f;
    float screen_h = 600.0f;

    static UnitScale compute(float width, float height, float dpi_scale = 1.0f) noexcept {
        UnitScale s;
        s.screen_w = width;
        s.screen_h = height;
        s.u = std::clamp(dpi_scale, 0.5f, 3.0f);
        return s;
    }
};

inline bool rect_contains(const nisaba::Rect& r, float px, float py) noexcept {
    return px >= r.left() && px <= r.right() && py >= r.top() && py <= r.bottom();
}

} // namespace sysmon

