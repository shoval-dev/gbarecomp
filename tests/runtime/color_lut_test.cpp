#include "color_lut.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>

namespace {

using gbarecomp::runtime::ColorLut;
using gbarecomp::runtime::ColorSettings;
using gbarecomp::runtime::ScreenKind;

std::array<uint8_t, 32768u * 3u> make_hardware_expanded_rgb() {
    std::array<uint8_t, 32768u * 3u> out{};
    for (uint32_t px = 0; px < 32768u; ++px) {
        const uint8_t r = static_cast<uint8_t>(px & 31u);
        const uint8_t g = static_cast<uint8_t>((px >> 5) & 31u);
        const uint8_t b = static_cast<uint8_t>((px >> 10) & 31u);
        out[px * 3u + 0u] = static_cast<uint8_t>((r << 3) | (r >> 2));
        out[px * 3u + 1u] = static_cast<uint8_t>((g << 3) | (g >> 2));
        out[px * 3u + 2u] = static_cast<uint8_t>((b << 3) | (b >> 2));
    }
    return out;
}

bool same_bytes(const std::array<uint8_t, 32768u * 3u>& a,
                const std::array<uint8_t, 32768u * 3u>& b) {
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (a[i] != b[i]) return false;
    }
    return true;
}

int failures = 0;

void expect(bool ok, const char* message) {
    if (!ok) {
        std::printf("FAIL: %s\n", message);
        ++failures;
    }
}

std::array<uint8_t, 3> map(const ColorLut& lut, int r, int g, int b, bool exact) {
    const uint8_t src[3] = {static_cast<uint8_t>(r), static_cast<uint8_t>(g),
                            static_cast<uint8_t>(b)};
    std::array<uint8_t, 3> dst{};
    if (exact)
        lut.map_rgb888_exact(src, dst.data(), 1, 1);
    else
        lut.map_rgb888(src, dst.data(), 1, 1);
    return dst;
}

ColorLut make(ScreenKind kind, double darken = -1.0) {
    ColorSettings settings{};
    settings.screen = kind;
    settings.darken = darken;
    return ColorLut(settings);
}

// The "agb" shader's formula for true 8-bit inputs, in double: pow(x, 3.2)
// * 0.94, the cross-talk matrix, pow(x, 1/2.2). A negative matrix result is
// clamped before the second power (the shader leaves it undefined).
std::array<int, 3> agb_reference(int r, int g, int b) {
    const double s[3] = {
        std::pow(r / 255.0, 3.2) * 0.94, std::pow(g / 255.0, 3.2) * 0.94,
        std::pow(b / 255.0, 3.2) * 0.94};
    static const double kMatrix[3][3] = {{0.82, 0.24, -0.06},
                                         {0.125, 0.665, 0.21},
                                         {0.195, 0.075, 0.73}};
    std::array<int, 3> out{};
    for (int c = 0; c < 3; ++c) {
        double m = kMatrix[c][0] * s[0] + kMatrix[c][1] * s[1] + kMatrix[c][2] * s[2];
        if (m < 0.0) m = 0.0;
        out[static_cast<std::size_t>(c)] =
            static_cast<int>(255.0 * std::pow(m, 1.0 / 2.2) + 0.5);
    }
    return out;
}

struct Rgb { int r, g, b; };

// "agb" on 8-bit inputs: white only loses the 0.94 luminance factor, the
// midtones drop much further (128 -> 91), pure red goes pink, pure green
// yellow-green, pure blue cyan with red clamped to 0.
const Rgb kAgbInputs[] = {
    {0, 0, 0},      {255, 255, 255}, {128, 128, 128}, {64, 64, 64},
    {255, 0, 0},    {0, 255, 0},     {0, 0, 255},     {248, 56, 0},
    {0, 168, 0},    {64, 120, 248},  {248, 184, 0},   {255, 200, 168},
    {32, 40, 72}};
const Rgb kAgbOutputs[] = {
    {0, 0, 0},      {248, 248, 248}, {91, 91, 91},    {33, 33, 33},
    {227, 96, 118}, {130, 206, 76},  {0, 122, 215},   {218, 94, 113},
    {71, 112, 42},  {0, 133, 208},   {228, 154, 121}, {238, 179, 167},
    {9, 23, 35}};

void test_tokens() {
    using namespace gbarecomp::runtime;
    for (int kind = 0; kind < kScreenKindCount; ++kind) {
        ScreenKind parsed = ScreenKind::Raw;
        expect(screen_kind_from_name(kScreenKindNames[kind], parsed),
               "every screen token parses");
        expect(parsed == static_cast<ScreenKind>(kind),
               "screen token parses to its own kind");
        expect(screen_kind_name(parsed) == kScreenKindNames[kind],
               "screen kind names round-trip");
    }
    ScreenKind parsed = ScreenKind::Classic;
    expect(!screen_kind_from_name("ags", parsed) && parsed == ScreenKind::Classic,
           "an unknown screen token is rejected and leaves the output alone");
    expect(screen_kind_name(static_cast<ScreenKind>(99)) == "raw",
           "an out-of-range kind names the default");
}

void test_agb_exact_matches_formula() {
    const ColorLut lut = make(ScreenKind::Agb);
    expect(!lut.is_passthrough(), "agb is not passthrough");
    const std::size_t count = sizeof(kAgbInputs) / sizeof(kAgbInputs[0]);
    for (std::size_t i = 0; i < count; ++i) {
        const Rgb& in = kAgbInputs[i];
        const std::array<uint8_t, 3> got = map(lut, in.r, in.g, in.b, true);
        const Rgb& want = kAgbOutputs[i];
        if (got[0] != want.r || got[1] != want.g || got[2] != want.b) {
            std::printf("FAIL: agb(%d,%d,%d) = {%u,%u,%u}, expected {%d,%d,%d}\n",
                        in.r, in.g, in.b, got[0], got[1], got[2], want.r, want.g,
                        want.b);
            ++failures;
        }
    }
}

// The exact path and the 32768-entry table are two evaluations of one model:
// on a hardware-expanded frame (what the PPU emits) they must agree to within
// the last bit, which is all the table's interpolation and the float stage
// can cost. Checked for every model, since they all gained the exact path.
void test_exact_agrees_with_table_on_hardware_colors() {
    const ScreenKind kinds[] = {ScreenKind::Unlit, ScreenKind::Frontlit,
                                ScreenKind::Backlit, ScreenKind::Classic,
                                ScreenKind::Agb};
    const auto src = make_hardware_expanded_rgb();
    std::array<uint8_t, 32768u * 3u> table{};
    std::array<uint8_t, 32768u * 3u> exact{};
    for (ScreenKind kind : kinds) {
        const ColorLut lut = make(kind);
        lut.map_rgb888(src.data(), table.data(), 32768, 1);
        lut.map_rgb888_exact(src.data(), exact.data(), 32768, 1);
        int worst = 0;
        std::uint32_t worst_px = 0;
        for (std::size_t i = 0; i < table.size(); ++i) {
            const int delta = std::abs(static_cast<int>(exact[i]) -
                                       static_cast<int>(table[i]));
            if (delta > worst) {
                worst = delta;
                worst_px = static_cast<std::uint32_t>(i / 3);
            }
        }
        if (worst > 1) {
            std::printf("FAIL: %s: exact path differs from the table by %d at bgr555 %u\n",
                        std::string(gbarecomp::runtime::screen_kind_name(kind)).c_str(),
                        worst, worst_px);
            ++failures;
        }
    }
}

// Off the 5-bit grid the exact path is still the formula. A dense random
// sweep against the double reference covers the tabulated transfer across
// its whole domain, including the exact stretch below 1/256.
void test_agb_exact_matches_reference_off_grid() {
    const ColorLut lut = make(ScreenKind::Agb);
    std::mt19937 rng(0xA6B5u);
    int worst = 0;
    for (int i = 0; i < 400000; ++i) {
        // Two thirds of the samples are forced dark: that is where the
        // curve is steepest and a table is most likely to be wrong.
        const int ceiling = (i % 3 == 0) ? 256 : 40;
        const int r = static_cast<int>(rng() % static_cast<unsigned>(ceiling));
        const int g = static_cast<int>(rng() % static_cast<unsigned>(ceiling));
        const int b = static_cast<int>(rng() % static_cast<unsigned>(ceiling));
        const std::array<uint8_t, 3> got = map(lut, r, g, b, true);
        const std::array<int, 3> want = agb_reference(r, g, b);
        for (int c = 0; c < 3; ++c) {
            const int delta =
                std::abs(static_cast<int>(got[static_cast<std::size_t>(c)]) -
                         want[static_cast<std::size_t>(c)]);
            if (delta > worst) {
                worst = delta;
                std::printf("  note: agb(%d,%d,%d) differs from the reference by %d\n",
                            r, g, b, delta);
            }
        }
    }
    expect(worst <= 1, "agb exact path stays within 1 level of the formula");
}

// A ghosted frame is an average of two hardware colors: off the 5-bit grid.
// The table path snaps such a level to the hardware color below it; the
// exact path must not.
void test_exact_does_not_snap_to_the_grid() {
    const ColorLut lut = make(ScreenKind::Agb);
    // Hardware level 16 renders as 132; 128 is between levels 15 (123) and 16.
    const std::array<uint8_t, 3> table = map(lut, 128, 128, 128, false);
    const std::array<uint8_t, 3> exact = map(lut, 128, 128, 128, true);
    const std::array<uint8_t, 3> level15 = map(lut, 123, 123, 123, true);
    const std::array<uint8_t, 3> level16 = map(lut, 132, 132, 132, true);
    expect(exact[0] == 91, "agb of 128 gray is 91");
    expect(table[0] != exact[0],
           "the table path recovers a hardware level, the exact path does not");
    expect(level15[0] < exact[0] && exact[0] < level16[0],
           "128 grades between the hardware levels around it");
}

void test_passthrough() {
    ColorSettings raw{};
    raw.screen = ScreenKind::Raw;
    ColorLut lut(raw);
    if (!lut.is_passthrough()) {
        std::printf("FAIL: raw screen should be passthrough\n");
        ++failures;
        return;
    }

    const auto src = make_hardware_expanded_rgb();
    std::array<uint8_t, 32768u * 3u> dst{};
    lut.map_rgb888(src.data(), dst.data(), 32768, 1);
    expect(same_bytes(src, dst), "raw LUT changed hardware-expanded RGB");

    dst = src;
    lut.map_rgb888(dst.data(), dst.data(), 32768, 1);
    expect(same_bytes(src, dst), "raw LUT changed in-place hardware-expanded RGB");

    dst.fill(0);
    lut.map_rgb888_exact(src.data(), dst.data(), 32768, 1);
    expect(same_bytes(src, dst), "raw exact path changed hardware-expanded RGB");
}

}  // namespace

int main() {
    test_passthrough();
    test_tokens();
    test_agb_exact_matches_formula();
    test_exact_agrees_with_table_on_hardware_colors();
    test_agb_exact_matches_reference_off_grid();
    test_exact_does_not_snap_to_the_grid();

    if (failures) {
        std::printf("color_lut_tests: %d FAILURE(S)\n", failures);
        return 1;
    }
    std::printf("color_lut_tests: PASS\n");
    return 0;
}
