#include "present_effects.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace {

using gbarecomp::runtime::LcdGrid;

int failures = 0;

void expect(bool ok, const char* message) {
    if (!ok) {
        std::printf("FAIL: %s\n", message);
        ++failures;
    }
}

void expect_near(double got, double want, const char* label) {
    if (std::fabs(got - want) > 0.0006) {
        std::printf("FAIL: %s = %.5f, expected %.3f\n", label, got, want);
        ++failures;
    }
}

double weight(const LcdGrid& grid, int dx, int dy) {
    return static_cast<double>(grid.weight[static_cast<std::size_t>(dy)]
                                          [static_cast<std::size_t>(dx)]) /
           4294967296.0;
}

// G_t = 0.5 * F_t + 0.5 * F_{t-1}, each channel on its own, rounded to the
// nearest level; 255 and 0 average to 128.
void test_ghost_blend() {
    const std::uint8_t current[6] = {255, 0, 100, 1, 200, 7};
    const std::uint8_t previous[6] = {0, 255, 150, 2, 200, 0};
    std::uint8_t out[6] = {};
    gbarecomp::runtime::blend_ghost_frame(current, previous, out, 6);
    const std::uint8_t want[6] = {128, 128, 125, 2, 200, 4};
    for (int i = 0; i < 6; ++i)
        expect(out[static_cast<std::size_t>(i)] == want[static_cast<std::size_t>(i)],
               "ghost blend averages each channel");
    // A frame blended with itself is unchanged (the first frame after the
    // effect is switched on has no predecessor).
    std::uint8_t same[6] = {};
    gbarecomp::runtime::blend_ghost_frame(current, current, same, 6);
    for (int i = 0; i < 6; ++i)
        expect(same[static_cast<std::size_t>(i)] == current[static_cast<std::size_t>(i)],
               "ghost blend is a no-op for two identical frames");
    // In place.
    std::uint8_t inplace[6] = {255, 0, 100, 1, 200, 7};
    gbarecomp::runtime::blend_ghost_frame(inplace, previous, inplace, 6);
    expect(inplace[0] == 128, "ghost blend works in place");
}

// The shading of one logical pixel, sampled at the centre of each sub-pixel
// of an exact integer scale. With one sub-pixel per axis its centre is the
// peak (factor 1); with two both sit at the same height; a grid appears from
// three up.
void test_grid_factors() {
    const LcdGrid one = gbarecomp::runtime::make_lcd_grid(1);
    expect(one.factor == 0, "a 1x grid is no grid");

    const LcdGrid two = gbarecomp::runtime::make_lcd_grid(2);
    expect(two.factor == 2, "2x grid exists");
    for (int dy = 0; dy < 2; ++dy)
        for (int dx = 0; dx < 2; ++dx)
            expect_near(weight(two, dx, dy), 0.904, "2x grid weight");

    const LcdGrid three = gbarecomp::runtime::make_lcd_grid(3);
    const double kThree[3][3] = {{0.857, 0.912, 0.857},
                                 {0.940, 1.000, 0.940},
                                 {0.857, 0.912, 0.857}};
    for (int dy = 0; dy < 3; ++dy)
        for (int dx = 0; dx < 3; ++dx)
            expect_near(weight(three, dx, dy), kThree[dy][dx], "3x grid weight");

    const LcdGrid four = gbarecomp::runtime::make_lcd_grid(4);
    expect_near(weight(four, 0, 0), 0.838, "4x grid corner");
    expect_near(weight(four, 1, 1), 0.971, "4x grid inner");
    expect_near(weight(four, 3, 2), 0.916, "4x grid edge");
    expect(weight(four, 3, 2) == weight(four, 0, 1),
           "the grid is symmetric under a half turn");

    const LcdGrid seven = gbarecomp::runtime::make_lcd_grid(7);
    expect(seven.factor == 0, "factors past the cap are rejected");
}

void test_grid_factor_for_destination() {
    auto factor = [](int w, int h) {
        return gbarecomp::runtime::lcd_grid_factor(w, h, 240, 160);
    };
    expect(factor(240, 160) == 0, "1x window shows no grid");
    expect(factor(480, 320) == 2, "2x window");
    expect(factor(720, 480) == 3, "3x window keeps full grid resolution");
    expect(factor(1000, 666) == 4, "fractional scale takes the whole part");
    expect(factor(3840, 2160) == gbarecomp::runtime::kLcdGridMaxFactor,
           "huge windows stop at the cap");
    expect(factor(200, 400) == 0, "a window narrower than native shows no grid");
    expect(gbarecomp::runtime::lcd_grid_factor(0, 0, 240, 160) == 0,
           "an empty destination shows no grid");
}

// A white frame through a 3x grid: untouched at the centre of every logical
// pixel, darkest in its four corners, dimmer along its edges (more so across
// the scanline than along it), laid out as three rows of three sub-pixels
// per logical pixel.
void test_grid_expansion() {
    const std::uint8_t frame[2 * 2 * 3] = {255, 255, 255, 255, 255, 255,
                                           255, 255, 255, 255, 255, 255};
    const LcdGrid grid = gbarecomp::runtime::make_lcd_grid(3);
    std::array<std::uint32_t, 6 * 6> out{};
    gbarecomp::runtime::expand_lcd_grid(frame, 2, 2, grid, out.data());
    const auto pixel = [&](int x, int y) { return out[static_cast<std::size_t>(y) * 6 +
                                                   static_cast<std::size_t>(x)]; };
    const auto red = [&](int x, int y) { return (pixel(x, y) >> 16) & 0xFF; };
    expect(red(1, 1) == 255 && red(4, 4) == 255, "grid centre is untouched");
    expect(red(0, 0) == 219 && red(5, 5) == 219, "grid corner is 255 * 0.857");
    // 255 * 15.5/17 is exactly 232.5, a rounding tie.
    expect(red(1, 0) == 232 || red(1, 0) == 233,
           "grid edge across the scanline is 255 * 0.912");
    expect(red(0, 1) == 240, "grid edge along the scanline is 255 * 0.940");
    expect((pixel(1, 1) & 0xFF) == 255 && ((pixel(1, 1) >> 8) & 0xFF) == 255,
           "green and blue are shaded like red");
    expect((pixel(0, 0) >> 24) == 0, "the X of XRGB8888 stays zero");

    // Black stays black, and a frame is laid out row-major without bleeding
    // between logical pixels.
    const std::uint8_t two[3 * 1 * 3] = {255, 0, 0,   0, 255, 0,   0, 0, 255};
    std::array<std::uint32_t, 9 * 3> wide{};
    gbarecomp::runtime::expand_lcd_grid(two, 3, 1, grid, wide.data());
    const std::uint32_t left = wide[static_cast<std::size_t>(1) * 9 + 1];
    expect((left >> 16 & 0xFF) == 255 && (left >> 8 & 0xFF) == 0,
           "first logical pixel keeps its own colour");
    const std::uint32_t right = wide[static_cast<std::size_t>(1) * 9 + 7];
    expect((right & 0xFF) == 255 && (right >> 16 & 0xFF) == 0,
           "last logical pixel keeps its own colour");
}

}  // namespace

int main() {
    test_ghost_blend();
    test_grid_factors();
    test_grid_factor_for_destination();
    test_grid_expansion();

    if (failures) {
        std::printf("present_effects_tests: %d FAILURE(S)\n", failures);
        return 1;
    }
    std::printf("present_effects_tests: PASS\n");
    return 0;
}
