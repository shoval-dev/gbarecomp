// present_effects.h — present-time LCD effects that need no GPU: the panel's
// response-time ghosting and its 1x pixel grid. Pure byte math, header-only.
//
// PRESENT-TIME ONLY, exactly like color_lut.h: the host applies these to a
// COPY of the frame at SDL-upload time. They never touch the emulation, the
// framebuffer the PPU produces, or the differential-verify path, and both
// default to off, so default behavior and every hashed/verified frame stay
// byte-identical to a build without this file.
//
// Ported from NanoBoyAdvance's Filters.hlsl / Filters.metal (BSD-2-Clause,
// Copyright 2026 Mireille Meyer, Gloria Goertz): FLCDGhost and VLCD1x/FLCD1x.
// Full notice: third_party/BSD-2-Clause.txt. See THIRD_PARTY_ATTRIBUTION.md.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace gbarecomp::runtime {

// LCD ghosting. The panel's slow response leaves the previous frame visible:
// each channel of the displayed frame is the average of the two most recent
// PRESENTED frames,
//
//     G_t = 0.5 * F_t + 0.5 * F_{t-1}
//
// a two-tap average with no feedback (an older display would keep feeding the
// average back in and trail longer). `current` and `previous` are
// `byte_count` bytes of RGB888; the result is quantized to 8 bits like the
// RGBA8 intermediate the GPU path writes it to. Pointers may alias.
inline void blend_ghost_frame(const std::uint8_t* current,
                              const std::uint8_t* previous,
                              std::uint8_t* out, std::size_t byte_count) {
    for (std::size_t i = 0; i < byte_count; ++i)
        out[i] = static_cast<std::uint8_t>((current[i] + previous[i] + 1) >> 1);
}

// LCD grid ("LCD1x"). Every logical pixel is a small lens on the real panel:
// brightest at its centre, falling off towards the gaps between pixels. The
// shading is the product of a horizontal and a vertical sine whose period is
// exactly one logical pixel, phased so each sine peaks at the pixel centre and
// bottoms out on its edges. It darkens the image by about 10% on average; the
// gap only becomes visible from 3x up, because at 2x both sub-pixels of an
// axis sit at the same height on the sine, and at 1x the single sub-pixel
// centre IS the peak.
constexpr double kLcdGridBrightenScanlines = 16.0;
constexpr double kLcdGridBrightenLcd = 24.0;

// The grid is baked into an integer prescale, so the GPU later scales it once
// per screen pixel exactly as the shader path samples it. Beyond this many
// sub-pixels per logical pixel the per-frame CPU cost (width*height*n*n) stops
// being negligible, so larger windows reuse a coarser grid.
constexpr int kLcdGridMaxFactor = 6;

struct LcdGrid {
    int factor = 0;  // sub-pixels per logical pixel on each axis; 0 = none
    // Brightness of sub-pixel (dy, dx) in Q32: 1.0 = untouched.
    std::uint32_t weight[kLcdGridMaxFactor][kLcdGridMaxFactor] = {};
};

inline LcdGrid make_lcd_grid(int factor) {
    LcdGrid grid;
    if (factor < 2 || factor > kLcdGridMaxFactor) return grid;
    constexpr double kPi = 3.141592653589793;
    grid.factor = factor;
    for (int dy = 0; dy < factor; ++dy) {
        const double angle_y =
            2.0 * kPi * (((dy + 0.5) / factor) - 0.25);
        const double lines =
            (kLcdGridBrightenScanlines + std::sin(angle_y)) /
            (kLcdGridBrightenScanlines + 1.0);
        for (int dx = 0; dx < factor; ++dx) {
            const double angle_x =
                2.0 * kPi * (((dx + 0.5) / factor) - 0.25);
            const double column =
                (kLcdGridBrightenLcd + std::sin(angle_x)) /
                (kLcdGridBrightenLcd + 1.0);
            grid.weight[dy][dx] = static_cast<std::uint32_t>(std::min(
                4294967295.0, column * lines * 4294967296.0 + 0.5));
        }
    }
    return grid;
}

// How many sub-pixels per logical pixel fit a `width` x `height` destination.
// Exact multiples keep their full resolution (a 3x window gets a 3x grid);
// anything else takes the whole part, and the remainder is left to the
// scaler. Below 2x there is nothing to draw: 0.
inline int lcd_grid_factor(int width, int height, int logical_width,
                           int logical_height) {
    if (width <= 0 || height <= 0 || logical_width <= 0 || logical_height <= 0)
        return 0;
    const int factor = std::min(width / logical_width, height / logical_height);
    return factor >= 2 ? std::min(factor, kLcdGridMaxFactor) : 0;
}

// Expand a `width` x `height` RGB888 frame through `grid` into `out`: one
// little-endian XRGB8888 pixel (bytes B,G,R,X) per sub-pixel, `width *
// grid.factor` per row. `out` must hold width*height*factor*factor pixels.
inline void expand_lcd_grid(const std::uint8_t* rgb888, int width, int height,
                            const LcdGrid& grid, std::uint32_t* out) {
    const int factor = grid.factor;
    if (factor <= 0 || width <= 0 || height <= 0) return;
    const std::size_t stride = static_cast<std::size_t>(width) * factor;
    const std::size_t row = static_cast<std::size_t>(width) * 3u;
    for (int y = 0; y < height; ++y) {
        const std::uint8_t* src = rgb888 + static_cast<std::size_t>(y) * row;
        for (int x = 0; x < width; ++x, src += 3) {
            const std::uint64_t r = src[0], g = src[1], b = src[2];
            for (int dy = 0; dy < factor; ++dy) {
                const std::uint32_t* weight = grid.weight[dy];
                std::uint32_t* dst = out +
                    (static_cast<std::size_t>(y) * factor + dy) * stride +
                    static_cast<std::size_t>(x) * factor;
                for (int dx = 0; dx < factor; ++dx) {
                    const std::uint64_t w = weight[dx];
                    // Q32 multiply with round-to-nearest.
                    dst[dx] = static_cast<std::uint32_t>(
                                  ((r * w + 0x80000000u) >> 32) << 16) |
                              static_cast<std::uint32_t>(
                                  ((g * w + 0x80000000u) >> 32) << 8) |
                              static_cast<std::uint32_t>((b * w + 0x80000000u) >> 32);
                }
            }
        }
    }
}

}  // namespace gbarecomp::runtime
