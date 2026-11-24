// color_lut.cpp — see color_lut.h. Present-time only.
//
// Ported from JRickey/gba-recomp (crates/screen/src/{color,profile,lut}.rs),
// © Jrickey, MIT OR Apache-2.0, used with permission. The "agb" model's
// constants are ported from NanoBoyAdvance's Filters.hlsl (BSD-2-Clause;
// full notice: third_party/BSD-2-Clause.txt). See THIRD_PARTY_ATTRIBUTION.md.

#include "color_lut.h"

#include <algorithm>
#include <cstddef>
#include <cmath>
#include <cstring>

namespace gbarecomp::runtime {
namespace {

// ── CIE colorimetry (all f64; runs only at LUT-build time) ─────────
struct Xy { double x, y; };
struct Primaries { Xy red, green, blue, white; };
struct Mat3 { double m[3][3]; };

constexpr Xy kD65 = {0.3127, 0.3290};
constexpr Primaries kSrgb = {{0.64, 0.33}, {0.30, 0.60}, {0.15, 0.06}, kD65};
constexpr Primaries kDisplayP3 = {{0.680, 0.320}, {0.265, 0.690}, {0.150, 0.060}, kD65};

// Measured reflective/frontlit gamut, and the near-sRGB backlit revision.
constexpr Primaries kPanelReflective = {
    {0.4925, 0.3100}, {0.3150, 0.4825}, {0.1625, 0.1925}, kD65};
constexpr Primaries kPanelBacklit = {
    {0.6191, 0.3454}, {0.3269, 0.6003}, {0.1436, 0.0893}, kD65};

bool xy_eq(Xy a, Xy b) { return a.x == b.x && a.y == b.y; }

void mat_apply(const Mat3& a, const double v[3], double out[3]) {
    for (int i = 0; i < 3; ++i)
        out[i] = a.m[i][0] * v[0] + a.m[i][1] * v[1] + a.m[i][2] * v[2];
}

Mat3 mat_mul(const Mat3& a, const Mat3& b) {
    Mat3 r{};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            for (int k = 0; k < 3; ++k) r.m[i][j] += a.m[i][k] * b.m[k][j];
    return r;
}

Mat3 mat_inverse(const Mat3& a) {
    const auto& m = a.m;
    auto cof = [&](int r, int c) {
        int r1 = (r + 1) % 3, r2 = (r + 2) % 3;
        int c1 = (c + 1) % 3, c2 = (c + 2) % 3;
        return m[r1][c1] * m[r2][c2] - m[r1][c2] * m[r2][c1];
    };
    double det = m[0][0] * cof(0, 0) + m[0][1] * cof(0, 1) + m[0][2] * cof(0, 2);
    Mat3 out{};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) out.m[i][j] = cof(j, i) / det;
    return out;
}

void xy_to_xyz(Xy c, double out[3]) {
    out[0] = c.x / c.y;
    out[1] = 1.0;
    out[2] = (1.0 - c.x - c.y) / c.y;
}

// Linear RGB → CIE XYZ for a set of primaries (white maps to Y=1).
Mat3 rgb_to_xyz(const Primaries& p) {
    double r[3], g[3], b[3], w[3];
    xy_to_xyz(p.red, r);
    xy_to_xyz(p.green, g);
    xy_to_xyz(p.blue, b);
    xy_to_xyz(p.white, w);
    Mat3 m = {{{r[0], g[0], b[0]}, {r[1], g[1], b[1]}, {r[2], g[2], b[2]}}};
    double s[3];
    mat_apply(mat_inverse(m), w, s);
    Mat3 out = m;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) out.m[i][j] *= s[j];
    return out;
}

Mat3 bradford_adaptation(Xy from, Xy to) {
    constexpr Mat3 kBradford = {{{0.8951, 0.2664, -0.1614},
                                 {-0.7502, 1.7135, 0.0367},
                                 {0.0389, -0.0685, 1.0296}}};
    double f[3], t[3], src[3], dst[3];
    xy_to_xyz(from, f);
    xy_to_xyz(to, t);
    mat_apply(kBradford, f, src);
    mat_apply(kBradford, t, dst);
    Mat3 scale = {{{dst[0] / src[0], 0, 0},
                   {0, dst[1] / src[1], 0},
                   {0, 0, dst[2] / src[2]}}};
    return mat_mul(mat_mul(mat_inverse(kBradford), scale), kBradford);
}

Mat3 rgb_to_rgb(const Primaries& src, const Primaries& dst) {
    Mat3 to_xyz = rgb_to_xyz(src);
    Mat3 from_xyz = mat_inverse(rgb_to_xyz(dst));
    if (xy_eq(src.white, dst.white)) return mat_mul(from_xyz, to_xyz);
    return mat_mul(mat_mul(from_xyz, bradford_adaptation(src.white, dst.white)),
                   to_xyz);
}

double srgb_oetf(double v) {
    return v <= 0.0031308 ? 12.92 * v : 1.055 * std::pow(v, 1.0 / 2.4) - 0.055;
}

// ── Panel optical model ────────────────────────────────────────────
struct PanelModel { Primaries primaries; double gamma, luminance, black_floor; };

double effective_gamma(double darken) {
    if (darken < 0.0) darken = 0.0;
    if (darken > 1.0) darken = 1.0;
    return 2.2 + 1.6 * darken;
}

double default_darken(ScreenKind s) {
    switch (s) {
        case ScreenKind::Unlit:    return 0.7;
        case ScreenKind::Frontlit: return 0.15;
        default:                   return 0.0;
    }
}

// Returns false for Raw/Classic (handled separately).
bool panel_model(ScreenKind s, double darken, PanelModel& out) {
    switch (s) {
        case ScreenKind::Unlit:
            out = {kPanelReflective, effective_gamma(darken), 0.91, 0.055};
            return true;
        case ScreenKind::Frontlit:
            out = {kPanelReflective, effective_gamma(darken), 0.91, 0.030};
            return true;
        case ScreenKind::Backlit:
            out = {kPanelBacklit, 2.2, 0.935, 0.002};
            return true;
        default:
            return false;
    }
}

uint8_t quantize(double v) {
    if (v < 0.0) v = 0.0;
    if (v > 1.0) v = 1.0;
    return static_cast<uint8_t>(v * 255.0 + 0.5);
}

// The community-canonical gamma-4.0 model, reproduced exactly as published.
std::array<uint8_t, 3> classic_entry(uint16_t px) {
    double r = (px & 31) / 31.0;
    double g = ((px >> 5) & 31) / 31.0;
    double b = ((px >> 10) & 31) / 31.0;
    double lr = std::pow(r, 4.0), lg = std::pow(g, 4.0), lb = std::pow(b, 4.0);
    auto mix = [&](double a, double bq, double c) {
        double v = (a * lb + bq * lg + c * lr) / 255.0;
        double e = std::pow(v, 1.0 / 2.2) * (255.0 / 280.0);
        return e;
    };
    return {quantize(mix(0.0, 50.0, 255.0)), quantize(mix(30.0, 230.0, 10.0)),
            quantize(mix(220.0, 10.0, 50.0))};
}

// ── The "agb" model ──────────────────────────────────────────────────
// hunterk's Color Mangler as tuned for the GBA LCD — the algorithm behind
// NanoBoyAdvance's "Color Correction". An input gamma of 3.2 (a 2.2 target
// gamma plus a full stop of screen darkening) and a 0.94 luminance factor
// push the midtones well down, a cross-talk matrix pulls reds towards pink,
// greens towards yellow and blues towards cyan, and a 2.2 output gamma brings
// the result back to display space. Every row of the matrix sums to 1, so
// white stays neutral and only loses the 0.94. The shader's saturate() is an
// identity here, and its contrast and saturation controls are identities at
// these settings, so neither appears.
constexpr double kAgbInputGamma = 3.2;
constexpr double kAgbLuminance = 0.94;
constexpr double kAgbDisplayGamma = 2.2;
constexpr double kAgbMatrix[3][3] = {
    {0.820, 0.240, -0.060},  // red   ← r,  gr,  br
    {0.125, 0.665,  0.210},  // green ← rg, g,   bg
    {0.195, 0.075,  0.730},  // blue  ← rb, gb,  b
};

double agb_linear(double channel01) {
    return std::min(1.0, std::pow(channel01, kAgbInputGamma) * kAgbLuminance);
}

// The PPU's 5→8 bit expansion of a hardware level.
constexpr int hardware_byte(int level5) { return (level5 << 3) | (level5 >> 2); }

// What level does an 8-bit value represent? The table models are defined on
// the true 15-bit level, which a PPU frame carries as hardware_byte(level).
// A ghosted frame carries averages of two such bytes, so it also has every
// value in between: those decode linearly between the levels around them.
// hardware_byte itself is the identity on this table's 32 anchor points, so
// a frame straight from the PPU decodes to exactly the level it was expanded
// from and grades exactly as the 32768-entry table says.
std::array<double, 256> build_level_decode() {
    std::array<double, 256> decode{};
    decode[0] = 0.0;
    int level = 0;
    for (int byte = 1; byte < 256; ++byte) {
        while (level < 31 && byte > hardware_byte(level + 1)) ++level;
        const int low = hardware_byte(level);
        const int high = level < 31 ? hardware_byte(level + 1) : 255;
        decode[static_cast<std::size_t>(byte)] =
            (level + static_cast<double>(byte - low) / (high - low)) / 31.0;
    }
    return decode;
}

// Saturated blues go negative in the red row (br = -0.06); the shader feeds
// that to pow(), which is undefined, so clamp first.
double agb_transfer(double x) {
    return std::pow(std::max(x, 0.0), 1.0 / kAgbDisplayGamma);
}

const Primaries& target_primaries(DisplayTarget t) {
    return t == DisplayTarget::DisplayP3 ? kDisplayP3 : kSrgb;
}

}  // namespace

bool screen_kind_from_name(std::string_view name, ScreenKind& out) {
    for (int kind = 0; kind < kScreenKindCount; ++kind) {
        if (name == kScreenKindNames[kind]) {
            out = static_cast<ScreenKind>(kind);
            return true;
        }
    }
    return false;
}

std::string_view screen_kind_name(ScreenKind kind) {
    const int index = static_cast<int>(kind);
    return kScreenKindNames[index >= 0 && index < kScreenKindCount ? index : 0];
}

ColorLut::ColorLut(const ColorSettings& settings) {
    kind_ = settings.screen;
    if (settings.screen == ScreenKind::Raw) {
        passthrough_ = true;
        return;
    }

    table_ = std::make_unique<std::array<std::array<uint8_t, 3>, 32768>>();
    auto& table = *table_;
    if (settings.screen == ScreenKind::Classic) {
        for (int px = 0; px < 32768; ++px)
            table[px] = classic_entry(static_cast<uint16_t>(px));
        build_exact(settings);
        return;
    }
    if (settings.screen == ScreenKind::Agb) {
        // Defined, like the shader, on the 8-bit value the PPU expands a
        // level to (hardware_byte / 255), not on the level itself. The two
        // differ by under half a percent, which is invisible except where the
        // matrix's negative term nearly cancels the red row.
        for (int px = 0; px < 32768; ++px) {
            const double lr = agb_linear(hardware_byte(px & 31) / 255.0);
            const double lg = agb_linear(hardware_byte((px >> 5) & 31) / 255.0);
            const double lb = agb_linear(hardware_byte((px >> 10) & 31) / 255.0);
            table[px] = {
                quantize(agb_transfer(kAgbMatrix[0][0] * lr + kAgbMatrix[0][1] * lg +
                                      kAgbMatrix[0][2] * lb)),
                quantize(agb_transfer(kAgbMatrix[1][0] * lr + kAgbMatrix[1][1] * lg +
                                      kAgbMatrix[1][2] * lb)),
                quantize(agb_transfer(kAgbMatrix[2][0] * lr + kAgbMatrix[2][1] * lg +
                                      kAgbMatrix[2][2] * lb))};
        }
        build_exact(settings);
        return;
    }

    double darken = settings.darken < 0.0 ? default_darken(settings.screen)
                                          : settings.darken;
    PanelModel model{};
    panel_model(settings.screen, darken, model);
    Mat3 to_display = rgb_to_rgb(model.primaries, target_primaries(settings.target));

    for (int px = 0; px < 32768; ++px) {
        double c[3] = {(px & 31) / 31.0, ((px >> 5) & 31) / 31.0,
                       ((px >> 10) & 31) / 31.0};
        double lin[3];
        for (int i = 0; i < 3; ++i) {
            double v = std::pow(c[i], model.gamma) * model.luminance;
            lin[i] = v > 1.0 ? 1.0 : v;
        }
        double out[3];
        mat_apply(to_display, lin, out);
        for (int i = 0; i < 3; ++i) {
            double v = out[i] < 0.0 ? 0.0 : (out[i] > 1.0 ? 1.0 : out[i]);
            double lifted = model.black_floor + (1.0 - model.black_floor) * v;
            out[i] = srgb_oetf(lifted);
        }
        table[px] = {quantize(out[0]), quantize(out[1]), quantize(out[2])};
    }
    build_exact(settings);
}

// ── Per-pixel form of the model (map_rgb888_exact) ────────────────────
//
// Same three stages as the table build above — per-channel input gamma and
// luminance, cross-talk, output transfer — but from the true 8-bit level
// instead of one recovered from the top 5 bits. The first two stages are a
// 256-entry table and nine multiplies. Only the transfer is expensive (a pow
// per channel per pixel), so it is tabulated: 8192 linear segments across
// [0, 1.25] hold the interpolation error below a quarter of a level wherever
// the curve is gentle enough, and the darkest stretch (x < 1/256, the lowest
// 16 levels, where it is steepest) and anything past the span are evaluated
// exactly.
constexpr double kTransferSpan = 1.25;
constexpr float kTransferExactBelow = 1.0f / 256.0f;

void ColorLut::build_exact(const ColorSettings& settings) {
    transfer_table_.fill(0.0f);

    Mat3 matrix{};
    if (settings.screen == ScreenKind::Agb) {
        for (int c = 0; c < 256; ++c) linear_[c] = static_cast<float>(agb_linear(c / 255.0));
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c) matrix.m[r][c] = kAgbMatrix[r][c];
        transfer_ = Transfer::Gamma22;
    } else {
        // The other models take the true level; see build_level_decode().
        const std::array<double, 256> decode = build_level_decode();
        if (settings.screen == ScreenKind::Classic) {
            for (int c = 0; c < 256; ++c)
                linear_[c] = static_cast<float>(std::pow(decode[static_cast<std::size_t>(c)], 4.0));
            matrix.m[0][0] = 255.0; matrix.m[0][1] = 50.0;  matrix.m[0][2] = 0.0;
            matrix.m[1][0] = 10.0;  matrix.m[1][1] = 230.0; matrix.m[1][2] = 30.0;
            matrix.m[2][0] = 50.0;  matrix.m[2][1] = 10.0;  matrix.m[2][2] = 220.0;
            transfer_ = Transfer::Classic;
        } else {
            double darken = settings.darken < 0.0 ? default_darken(settings.screen)
                                                  : settings.darken;
            PanelModel model{};
            panel_model(settings.screen, darken, model);
            matrix = rgb_to_rgb(model.primaries, target_primaries(settings.target));
            for (int c = 0; c < 256; ++c)
                linear_[c] = static_cast<float>(std::min(
                    1.0, std::pow(decode[static_cast<std::size_t>(c)], model.gamma) *
                             model.luminance));
            transfer_ = Transfer::Srgb;
            black_floor_ = static_cast<float>(model.black_floor);
        }
    }
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) matrix_[r * 3 + c] = static_cast<float>(matrix.m[r][c]);

    for (int i = 0; i <= kTransferSegments; ++i)
        transfer_table_[i] = exact_transfer(kTransferSpan * i / kTransferSegments);
}

// The output transfer in 8-bit levels, evaluated in double.
float ColorLut::exact_transfer(double x) const {
    switch (transfer_) {
        case Transfer::Gamma22: return static_cast<float>(255.0 * agb_transfer(x));
        case Transfer::Classic:
            // mix(): pow(sum / 255, 1/2.2) * 255/280, which quantize() then
            // multiplies by 255.
            return static_cast<float>(255.0 * 255.0 / 280.0 *
                                      agb_transfer(x / 255.0));
        case Transfer::Srgb: {
            // clamp(x, 0, 1) -> panel black floor -> sRGB OETF.
            const double v = std::clamp(x, 0.0, 1.0);
            return static_cast<float>(255.0 * srgb_oetf(black_floor_ +
                                                        (1.0 - black_floor_) * v));
        }
    }
    return 0.0f;
}

float ColorLut::tabled_transfer(float x) const {
    if (x < kTransferExactBelow || x >= static_cast<float>(kTransferSpan))
        return exact_transfer(x);
    constexpr float kScale = static_cast<float>(kTransferSegments / kTransferSpan);
    const float position = x * kScale;
    int index = static_cast<int>(position);
    // Rounding can land the largest representable x below the span on the
    // segment one past the table.
    if (index >= kTransferSegments) index = kTransferSegments - 1;
    const float base = transfer_table_[static_cast<std::size_t>(index)];
    return base + (transfer_table_[static_cast<std::size_t>(index) + 1] - base) *
                      (position - static_cast<float>(index));
}

void ColorLut::map_rgb888(const uint8_t* src, uint8_t* dst,
                          int width, int height) const {
    if (!table_) {
        if (dst == src) return;
        std::memmove(dst, src, static_cast<std::size_t>(width) * height * 3u);
        return;
    }
    const auto& table = *table_;
    const int n = width * height;
    for (int i = 0; i < n; ++i) {
        const uint8_t* s = src + i * 3;
        // Recover the 5-bit hardware channels (top 5 bits) and re-pack as
        // BGR555 (red is the low 5 bits, matching the LUT index).
        uint32_t idx = (s[0] >> 3) | ((s[1] >> 3) << 5) | ((s[2] >> 3) << 10);
        const auto& e = table[idx];
        uint8_t* d = dst + i * 3;
        d[0] = e[0];
        d[1] = e[1];
        d[2] = e[2];
    }
}

void ColorLut::map_rgb888_exact(const uint8_t* src, uint8_t* dst,
                                int width, int height) const {
    if (!table_) {
        map_rgb888(src, dst, width, height);
        return;
    }
    const float* linear = linear_.data();
    const float* matrix = matrix_.data();
    const int n = width * height;
    for (int i = 0; i < n; ++i) {
        const uint8_t* s = src + i * 3;
        const float r = linear[s[0]];
        const float g = linear[s[1]];
        const float b = linear[s[2]];
        uint8_t* d = dst + i * 3;
        for (int channel = 0; channel < 3; ++channel) {
            const float* row = matrix + channel * 3;
            const float levels = tabled_transfer(row[0] * r + row[1] * g + row[2] * b);
            d[channel] = static_cast<uint8_t>(std::clamp(levels + 0.5f, 0.0f, 255.0f));
        }
    }
}

}  // namespace gbarecomp::runtime
