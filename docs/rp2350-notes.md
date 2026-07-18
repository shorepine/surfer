# RP2350 port notes (research, no code yet)

Assessment of a surfer port to RP2350 / Adafruit Fruit Jam class
hardware, July 2026. Verdict up front: **medium lift, genuinely viable,
but capped at 320×240 / 400×240 RGB565** — the constraint that matters
is resolution, not architecture.

## The hardware, corrected

- DVI on RP2350 is **not PIO** (that was the RP2040 hack): the RP2350
  has **HSTX**, a dedicated high-speed serial transmit peripheral that
  does TMDS/DVI natively with DMA feeding it. Output modes are 640×480
  or 720×400.
- **RGB565 framebuffers must be half-resolution** — 320×240 or 400×240,
  pixel-doubled by the video path to the DVI signal. True 640×480 exists
  only at ≤ 8-bit palette / grayscale. (RAM arithmetic: 640×480×16bpp =
  614 KB > the chip's 520 KB SRAM.)
- Fruit Jam: RP2350B, dual Cortex-M33 @ 150 MHz, 16 MB flash + 8 MB
  QSPI PSRAM, DVI on HSTX **with the framebuffer in main SRAM**, 2-port
  USB-A host (keyboard/mouse), I2S audio.
- **There is no PPA-style 2D engine.** The blit toolbox is: DMA channels
  (fast linear copies; rect copies via per-row chaining), the SIO
  interpolators (address-generation helpers), and the M33s themselves.
  Fill/blit can ride DMA; **alpha blending is CPU, full stop**.

## Why surfer fits anyway

The M2/M5 measurements flip in surfer's favor here:

- On the P4, the PPA's ~85 µs/op floor punished small ops. A CPU
  compositor has **zero per-op overhead** — cost is purely pixels
  touched, which is exactly what dirty-rect discipline minimizes. The
  textgrid lesson generalizes: on RP2350, *everything* is the textgrid
  model, and surfer already runs that way on desktop (the SDL hal's
  software fill/blit/blend loops are portable C — an rp2350 hal reuses
  them nearly verbatim, swapping SDL for HSTX+DMA scanout).
- The arithmetic pencils out: 320×240 RGB565 = 150 KB in SRAM
  (single-cycle-ish). Full-screen blend at ~10 cycles/px ≈ 0.8 M cycles
  ≈ **5 ms @ 150 MHz** — 60 fps with headroom, and typical widget
  interactions are small rects at a fraction of that. Opaque cells /
  fills go through DMA cheaper still. This is *better* than the P4's
  overhead-dominated worst cases.
- Core, widgets, text, scrollview, the MicroPython binding: unchanged.
  The port surface is the proven shape: `src/hal/rp2350/` + a pico-sdk
  port dir + `port_rp2350.c` for the binding (TinyUSB host keyboard —
  the Fruit Jam's USB-A ports — mirrors the P4's usb_kbd.c role).

## The gotchas (what makes it less useful)

1. **Resolution ceiling.** 320×240 or 400×240 logical. Not a Tulip-class
   1024×600 canvas; UIs need QVGA-density layouts (Gamma 9001 as built
   would need a full re-layout, fewer visible steps/knobs). This is the
   real limitation.
2. **True 640×480 means palettized**, which breaks surfer's RGB565 core
   assumption — supporting 8-bit would ripple through hal formats and
   asset baking (palette quantization in surfpack). Not v1 material.
3. **Memory Tetris.** 520 KB SRAM holds: fb (150–188 KB), MicroPython
   heap, stacks, hot assets. Double-buffer-with-damage (2×150 KB) is
   possible but tight; single-buffer direct (compose ≤ 5 ms) is likely
   fine. PSRAM is QSPI-XIP (~60–90 MB/s, shared with code) — good for
   asset storage, not for the scanout buffer. Current baked assets
   (~1.6 MB of strips) live in flash/PSRAM and blit through the XIP
   cache; QVGA-sized rebakes (40 px knobs) would shrink them 2.5×.
4. **No blend hardware** — alpha-stacked scenes cost linearly. surfer's
   opaque-asset + occlusion-early-out style mitigates; heavy translucency
   doesn't.
5. **No touchscreen** on Fruit Jam — input is USB mouse/keyboard, which
   surfer treats like the desktop build. Works fine; the finger-feel
   widgets just lose some of their point.

## Recommendation

Worth doing, after current priorities (AMY hookup, on-device tulip
verify, M6): roughly 1–2 focused sessions to first light given how much
is reused. Target: Fruit Jam, 400×240 RGB565 pixel-doubled, single
buffer first. Prep step that pays for itself immediately: factor the
software pixel ops out of `src/hal/sdl/` into a shared
`src/hal/soft_ops.c` both hals include — that's the whole frame path on
this chip.
