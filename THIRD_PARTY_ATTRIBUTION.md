# Third-Party Attribution

Portions of this project are ported from other open-source projects,
used with permission and under their licenses. Facts (struct offsets,
byte signatures, documented hardware/driver behavior) and, where noted,
ported logic are credited below. License texts of permissively-licensed
upstreams are reproduced in their repositories.

## JRickey/gba-recomp

- **Upstream:** https://github.com/JRickey/gba-recomp
- **Author:** Jrickey
- **License:** MIT OR Apache-2.0 (used with the author's permission)

| Our file | Upstream source | What was ported |
|---|---|---|
| `src/gba/gba_m4a.{h,cpp}` | `crates/gba-core/src/mp2k.rs` | MP2K ("m4a") SDK driver detection (SoundMain literal-pool signature + CRC fallback) and the SoundInfo / SoundChannel / WaveData guest-struct offset map. Re-implemented in C++; the live-state dump path is original to this project. |
| `src/runtime/color_lut.{h,cpp}` | `crates/screen/src/{color,profile,lut}.rs` | Screen-color simulation: CIE colorimetry core, measured per-revision panel models, and the BGR555→RGBA8 LUT build. Re-implemented in C++; the present-time RGB888-input apply path is ours. |
| `src/gba/gba_rtc.{h,cpp}` | `crates/gba-core/src/{rtc,hostclock}.rs` | Cartridge GPIO port + S-3511A RTC (serial state machine, BCD date/time, Seiko signature detection) and the civil↔linear host-clock helpers (Hinnant day algorithms, public domain). Re-implemented in C++ (Win32 `GetLocalTime` / POSIX `localtime_r`). |
| `src/gba/audio_shadow.{h,cpp}` | `crates/gba-core/src/shadow.rs` | Engine-agnostic HLE-shadow differential verifier: envelope-correlation self-check vs the canon FIFO stream, probation auto-gain, prove/strike/pause-and-reprobe. Re-implemented in C++. (Permitted under PRINCIPLES.md "Verified-enhancement HLE".) |
| `src/gba/mp2k_shadow.{h,cpp}` | `crates/gba-core/src/mp2k.rs` | MP2K HLE shadow mixer: float voice re-render (envelope mirror, PCM/DPCM sampling, intra-tick gain interpolation, reverb ring) driving the differential verifier. Re-implemented in C++. |

Each ported file carries an attribution header pointing here. Ported
code remains under the upstream's MIT OR Apache-2.0 terms; this notice
and those headers satisfy the attribution requirement.

## NanoBoyAdvance

- **Upstream:** https://codeberg.org/nba-emu/NanoBoyAdvance
- **Authors:** Mireille Meyer, Gloria Goertz, and contributors
- **License:** BSD-2-Clause

| Our file | Upstream source | What was ported |
|---|---|---|
| `src/runtime/present_effects.h` | `Sources/NanoBoyAdvanceQt/Content/Shaders/Filters.hlsl` (`FLCDGhost`, `VLCD1x` / `FLCD1x`) | The LCD ghosting blend (each frame averaged with the previous one) and the 1x LCD grid (a horizontal and a vertical sine of one-pixel period, phased to peak at pixel centres). Re-implemented in C++ for the CPU present path. |
| `src/runtime/color_lut.{h,cpp}` | same file, `FColorCorrect` | The constants of the `agb` screen model: input gamma 3.2, 0.94 luminance, the 3x3 cross-talk matrix, 2.2 output gamma. |

Both files carry an attribution header pointing here, and the full notice —
copyright line, conditions and disclaimer — is reproduced in
[`third_party/BSD-2-Clause.txt`](third_party/BSD-2-Clause.txt). Both effects
are opt-in and default off (see `ENHANCEMENTS.md` §4).

Where the two effects come from, further back than `Filters.hlsl` (which
itself carries no per-effect notes):

- **The `agb` model** takes its numbers from hunterk's "Color Mangler" as
  modified by Pokefan531. That shader's own header reads "License: Public
  domain" (seen in NanoBoyAdvance's history at `643ba25b`,
  `color_agb.glsl.hpp`).
- **The ghosting blend** is a plain average of two frames, with no earlier
  source.
- **The LCD grid** is Gigaherz's `lcd3x` formula, `(B + sin(angle)) / (B + 1)`
  on each axis with `B = 16` on the scanline axis. RetroArch ships `lcd3x` as
  public domain (`libretro/glsl-shaders`, `handheld/shaders/lcd3x.glsl`). The
  `lcd1x` variant NanoBoyAdvance carried is nearly the same text as RetroArch's
  `lcd1x.glsl` ("Edited by jdgleaver" there; "Ported to NanoBoyAdvance by
  Destiny" in `60fdedcc`); both dropped lcd3x's per-channel colour separation,
  added a quarter-pixel phase shift, and carry a GPL-2.0-or-later notice.
  NanoBoyAdvance rewrote it as `FLCD1x`
  for 2.0 and relicensed the project under BSD-2-Clause in `34de0ff5`
  (2026-06-26); this port follows that file's label. Of the grid, only the
  phase shift and one constant (`BRIGHTEN_LCD` 24, where `lcd3x` has 4) are
  not already in `lcd3x`.

## mGBA

- **Upstream:** https://github.com/mgba-emu/mgba (cloned locally for the
  oracle target; `third_party/mgba/` is gitignored and NOT tracked here)
- **Author:** Jeffrey Pfau and contributors
- **License:** MPL-2.0

| Our file | Upstream source | What was ported |
|---|---|---|
| `src/runtime/bios_hle.{h,cpp}` | `src/gba/bios.c` | GBA BIOS SWI High-Level Emulation: the Div/Sqrt/ArcTan/ArcTan2 fixed-point routines and stall formulas, the LZ77 / Huffman / run-length / diff-unfilter decompressors, BitUnPack, the (float) affine-matrix builders, and MidiKey2Freq. Re-implemented in C++ against this project's `g_cpu` + bus bridge. HLE is opt-in; LLE (the recompiled real BIOS) remains the default and the correctness oracle (PRINCIPLES.md "verified-enhancement HLE"). |

`src/runtime/bios_hle.{h,cpp}` are derived from mGBA's `src/gba/bios.c` and
are therefore Covered Software under the MPL-2.0: **those two files are
licensed MPL-2.0, not PolyForm Noncommercial**, and each carries the Exhibit A
notice. The full license text is in
[`third_party/MPL-2.0.txt`](third_party/MPL-2.0.txt).

MPL-2.0 is file-scoped weak copyleft, so this does not affect the license of
the rest of gbarecomp: section 3.3 expressly permits distributing a Larger
Work under other terms provided the Covered Software stays under the MPL. The
source-availability requirement (section 3.2) is met by those two files being
present in this public repository — they *are* the Source Code Form of the
modified Covered Software. An earlier revision of this notice instead claimed
the obligation was satisfied by a vendored copy at
`third_party/mgba/src/gba/bios.c`; that path is gitignored and has never been
tracked here, so the claim was wrong. Nothing else in this repository is
MPL-covered.

libmgba itself is linked only into the opt-in oracle binary
(`-DGBARECOMP_BUILD_ORACLE=ON`), never into a shipping native build.
