# ESP32-S3 port notes (research, no code yet)

Assessment of surfer on the ESP32-S3 — the chip tulipcc ships on today,
driving 1024×600 over parallel RGB at RGB332, ~30 fps. July 2026.

Verdict up front: **a straight "surfer owns the display" S3 port at
1024×600 is a poor investment** — the bandwidth economics land you
exactly where tulipcc already is, plus new 8-bit work surfer doesn't
have. But there's a genuinely attractive **hal-only integration path:
surfer as the widget layer inside tulipcc's existing display engine.**

## The hardware

- Dual Xtensa LX7 @ 240 MHz with the PIE 128-bit SIMD extensions (LVGL
  and esp-dsp ship S3-tuned fill/blend/memcpy assembly — usable).
- **No blitter.** GDMA is linear-only (no 2D-DMA — that's P4-only), so
  like the RP2350 the frame path is CPU, with SIMD help.
- Octal PSRAM @ 80 MHz behind a small dcache. Effective throughput is
  tens of MB/s, shared between rendering, code, and — critically — LCD
  scanout. (Numbers below are order-of-magnitude; an M2-style bench
  would pin them, but tulipcc's shipping experience is the ground
  truth.)
- The RGB LCD peripheral scans the framebuffer out of PSRAM
  continuously. That's the whole story of S3 display work:

  | mode | scanout bandwidth |
  |---|---|
  | 1024×600 RGB565 @ 60 Hz | 73.7 MB/s — not sustainable alongside rendering |
  | 1024×600 RGB332 @ 60 Hz | 36.9 MB/s — tulipcc's choice |
  | 1024×600 RGB565 @ 30 Hz | 36.9 MB/s |
  | 800×480 RGB565 @ 60 Hz | 46 MB/s — borderline |
  | 480×320 RGB565 @ 60 Hz | 18 MB/s — comfortable |

## Why a standalone port disappoints at 1024×600

Surfer's model is a retained RGB565 framebuffer + dirty-rect compose.
On the S3 at 1024×600 that framebuffer must be PSRAM (1.23 MB — internal
SRAM is 512 KB total), so every composed pixel is a PSRAM round-trip
*and* the scanout fights for the same bus. To fit the bandwidth you
either:

- drop to RGB332 → surfer needs an 8-bit color path it doesn't have
  (hal formats, blending, asset quantization in the baker — the same
  ripple as RP2350's palette modes), and you land at tulipcc's existing
  numbers anyway; or
- halve the refresh at RGB565 → a 30 fps ceiling by construction; or
- shrink the panel → fine, but that's a different product.

tulipcc's scanline/bounce-buffer compositor is honestly the *better*
architecture for this chip: it composes into internal-SRAM chunks on
the fly instead of maintaining a second full PSRAM surface. Surfer
would be replacing a well-matched engine with a worse-matched one.

## The path that does make sense: surfer inside tulipcc

Surfer's core never touches the display — the hal does. So an S3 hal
doesn't have to own scanout at all; it can render **into tulipcc's
existing BG layer**:

1. Compose each dirty rect in RGB565 into a small internal-SRAM scratch
   tile (fast, cache-friendly, reuses the desktop software ops).
2. Convert 565→332 with a LUT on store into Tulip's BG framebuffer.
3. Tulip's proven bounce-buffer scanout does the rest. `present()` is a
   no-op; Tulip's sprite/text layers still composite above or below.

That's hal-local — zero core changes, no 8-bit support in surfer
proper, and the S3 and P4 Tulips end up on the same Python UI API
(`import surfer` behaves identically). Dirty-rect UI updates are small,
so the double-handling through the scratch tile costs little; full-
screen worst cases inherit Tulip's existing ceiling, which S3 users
already live with. Dual-core helps as tulipcc already does it: UI/
MicroPython on one core, display on the other.

## Recommendation

- Skip the standalone S3 port; the chip's bandwidth shape rewards
  tulipcc's current engine, not a retained-fb compositor, and the
  ceiling is the 30 fps / RGB332 you already have.
- When Tulip-on-P4 (surfer-native) is real, do the **tulipcc-BG hal**
  (scratch-tile + LUT store) as the S3 compatibility story — medium
  lift, mostly the shared-software-ops refactor the RP2350 notes
  already call for (`src/hal/soft_ops.c`), plus the 565→332 store path.
- Smaller RGB565 panels (≤ 800×480) on S3 for non-Tulip products would
  work fine with that same soft-ops hal, RP2350-style, with PSRAM
  latency as the main tax — bench before promising 60 fps.
