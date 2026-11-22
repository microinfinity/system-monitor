#pragma once
/// @file embedded_font.hpp
/// @brief Fallback in-binary embedded typography for Obsidian Shell.
/// Ensures the shell has guaranteed font rendering even without external filesystem font dependencies.

#include <cstddef>
#include <cstdint>

namespace sysmon {

struct EmbeddedFont {
    /// Pointer to Inter-Regular TrueType font data compiled into the binary
    static const uint8_t* inter_regular_data() noexcept;

    /// Size in bytes of the embedded Inter-Regular font
    static size_t inter_regular_size() noexcept;
};

} // namespace sysmon
