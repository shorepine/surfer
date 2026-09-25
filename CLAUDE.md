# CLAUDE.md — surfer

surfer is a small retained-mode UI compositor: C11 core, ESP32-P4 + SDL2 +
emscripten backends, MicroPython bindings. Read `DESIGN.md` before writing
code; it is the source of truth. If a task conflicts with DESIGN.md, stop and
ask rather than silently diverging.

## Non-negotiable architecture rules

- **No per-pixel software loops in the frame path.** The frame path is
  fill/blit/blend via the hal only. Per-pixel code is allowed exclusively
  inside `src/hal/sdl/` (desktop software blend), in build-time tools, and
  in the textgrid cell composer (`src/text/textgrid.c`) — the measured
  exception: the PPA's ~85µs/op floor makes per-glyph blits unusable for
  full-screen text (DESIGN.md §5.6).
- **No runtime vector rasterization.** Widget visuals are pre-rendered assets
  (filmstrips, 9-slice, baked font atlases). If a widget "needs" runtime
  drawing, the answer is a better asset, not a rasterizer.
- **Platform code lives only under `src/hal/`.** Core and widgets are
  platform-free C11 — no `#ifdef ESP_PLATFORM` outside the hal, ever.
- **Any buffer the PPA touches is 64-byte aligned** (allocation AND width
  stride), with explicit `esp_cache_msync` after CPU writes before PPA reads.
  All device allocations go through `hal->alloc_image`; never raw
  `heap_caps_malloc` in core code.
  The generic form of that writeback is **`surf_image_flush(img)`** (hal op
  `sync_image`, optional and NULL where the blitter is the CPU). Call it after
  writing an image's pixels yourself and before damaging the node that shows
  it. It exists because the hal's older note — "CPU never touches the compose
  buffer, so the only cache sync in the system is after asset uploads" — held
  only while every image was written ONCE. It stops holding the moment
  something renders into an image every frame, which is what the MicroPython
  `Image` buffer below is for. Getting this wrong is invisible on SDL and on
  web and tears on the panel, so the rule is to call it always.
- **The widget set stays small** (knob, slider, button, checkbox, dropdown,
  label, textinput, scrollview, scrollbar, led, selector, colorpicker,
  tabs, radio).
  Do not add widgets, node types, or hal ops without asking first.
- **No new dependencies without asking.** Currently allowed: stb_truetype,
  stb_image (build tools only), SDL2, ESP-IDF, MicroPython headers.

## Build & test loop

Desktop SDL is the iteration loop; touch hardware only at hal-backend
milestones.

```
make sdl        # builds desktop demo → build/surfer_demo
make test       # unit tests (dirty-rect coalescing, wrap, hit test)
make test-sdl   # present-coherence regression (opens an SDL window):
                # fb vs presented texture must match after fast scroll
make test-aspect # ... the window snaps back to the fb's aspect after a
                # resize, keeping the axis that was dragged
make web        # emscripten C demos → build/web/{mixer,settings}.html
make mpy-web    # tulip mode in the browser → build/web/index.html
idf.py build    # from ports/esp32p4/
```

Every core change must keep `make sdl && make test` green. Performance
acceptance test for anything touching the compositor: the M1 demo (6 sliders +
6 knobs) holds 60 fps under continuous drag; print frame time stats with
`SURF_STATS=1`.

## Style

- C11, 4-space indent, `snake_case`, `surf_` prefix on all public symbols.
- Public API is `include/surfer.h` only; keep it flat and boring — it is also
  the MicroPython binding surface, hand-bound in `bindings/micropython/`.
- No dynamic allocation in the frame path; nodes and rect lists come from
  pools sized at init.
- Prefer fixed-point (16.16) over float in core; the P4 has an FPU but the
  habit keeps hal backends honest.
- Comments explain *why* (bandwidth, alignment, PPA quirks), not *what*.

## Layout

```
include/surfer.h        public API (binding surface)
src/core/               scene graph, dirty rects, hit test, anim
src/widgets/            knob, slider, ...
src/text/               atlas text, wrap, textinput logic
src/hal/sdl/  src/hal/p4/  src/hal/web/
bindings/surfer/modsurfer.c
tools/surfpack.py       asset + font atlas packer
assets/                 source art. The Kenney sprite LIBRARY that
                        lived at assets/kenney/lib/ (40k sprites, 182 MB)
                        moved to its own repo, shorepine/kenney — the
                        FULL kenney.nl dump now, 3D kits included, built
                        by that repo's tools/build_tree.py (this repo's
                        tools/kenney_index.py was its ancestor). surfer
                        is a UI library and the art it keeps is only
                        what its own examples bake (assets/kenney/*.png,
                        five Space Shooter sprites)
ports/esp32p4/          ESP-IDF project wrapping the p4 hal
demos/
```

## Current state

M0 + M1 done. Core: group/rect/sprite/filmstrip/ninepatch nodes,
dirty-rect compositor with occlusion early-out (ninepatch stretches by
tiling — no scale_blit in the frame path), hit test, touch dispatch with
pointer capture (`src/core/input.c`). Widgets: knob (vertical-drag
default, angular optional) and slider, written against `surfer.h` only.
`make sdl` builds the M1 mixer demo (6 knobs + 6 sliders) →
`build/surfer_demo`, plus the M0 bounce demo → `build/surfer_bounce`;
placeholder art baked by `tools/gen_widget_assets.py`. Acceptance
verified: 60 fps windowed with all 12 controls animating
(`SURF_AUTODRAG=1`), ~0.5 ms/tick compose headless.

M2 done — **the bet passed on hardware.** p4 hal (`src/hal/p4/`): PPA
fill/SRM/blend, triple-buffer-with-damage presentation — zero-copy DSI
flip + DMA2D damage-forward, flicker-free (the measured buffering
verdict — see DESIGN.md §5.2 for all numbers and rejected paths), GT911
touch. `ports/esp32p4/` targets the ESP32-P4-Function-EV-Board
(IDF ≥5.4, BSP `esp32_p4_function_ev_board_noglib`); boot runs a
bandwidth/PPA benchmark, then the mixer demo. Measured under finger:
62–66 fps, ~2.3 ms/tick. Key hardware rule learned: PPA ops cost
~70–200 µs each regardless of size → bake assets at final size (the
slider uses a sprite track when style art matches exactly; tiled 9-patch
is the fallback). Flash: `idf.py -p <port> flash` from `ports/esp32p4/`.

M3 done (desktop-verified; device run pending a replug): text.
`tools/fontbake.c` (stb_truetype, host tool) bakes TTFs into A8 atlas +
advance/kern headers at build time; runtime text is clipped atlas blits
(`src/text/`: UTF-8, greedy wrap on space/hyphen, kerning, align,
ellipsize; label + textinput nodes with caret/selection/scroll-into-view;
byte-offset indices). A8 images carry a `tint`; SDL blends in software,
P4 uses PPA `PPA_BLEND_COLOR_MODE_A8` + `fg_fix_rgb_val`. Desktop
keyboard feeds textinput via `surf_hal_sdl_poll_key` (hal-adjacent, not
in the vtable — the device path is the M-later OSK widget). Ctrl-C is
the exception to that queue: it goes to the `surf_hal_sdl_on_interrupt`
hook and is swallowed, because the case it exists for is escaping a host
loop that never reads keys. `port_sdl.c` points the hook at
`mp_sched_keyboard_interrupt`, matching what a device USB driver does
with ctrl+C — without it the desktop had no way out of an app's own
`while surfer.tick()` loop.
`build/surfer_type` is the text demo; `SURF_SHOT=x.ppm` dumps any demo's
framebuffer.

M4 done (desktop-verified): scrollview node (`src/core/scroll.c`) with
drag/flick momentum, edge resistance + spring-back, all fixed-point in
core ticks; damage from scrolled content translates through offsets and
clips to ancestor boxes. Input: a scrollable scrollview captures empty-
space drags directly, and steals a child handler's gesture after 8px of
travel along a scrollable axis — unless the handler set
`surf_node_set_gesture_grab` (sliders/knobs/textinput do). Groups with a
handler + size are hittable (hot areas, scrims). Widgets: checkbox
(2-frame filmstrip) and dropdown (popup attaches to the screen root —
detach/reattach as overlay). `build/surfer_settings` is the M4 demo.

M5 done on the unix port (esp micropython is next): hand-written binding
`bindings/surfer/modsurfer.c` (two MP types — Node and Widget — plus flat
factory functions; capitalized aliases; callbacks fire from tick on the
same thread; a GC-root registry keeps C-referenced objects alive).
`make mpy` builds it (MPY_DIR ?= ~/micropython, pinned v1.26.0; needs
`make -C ports/unix submodules` once). `bindings/surfer/tulip.py` is
tulip mode: an on-screen REPL on a mono16 textgrid + tulipcc-style
UIScreen — `s = surfer.slider(x,y)`, `screen.add(s)`, `s.y_pos`,
`s.callback = fn` all live. `bindings/surfer/test_surfer.py` is the
headless binding test (uses `surfer._touch` injection).

M6 web: both flavors build and run in a canvas. (1) C demos —
`make web`: the sdl hal compiled with emscripten (`-sUSE_SDL=2
-sASYNCIFY`); the desktop `while (pump()) tick` shape survives via an
EM_ASYNC_JS rAF yield in `surf_hal_sdl_pump` (rAF, not a timer: frames
drawn from timer-resumed contexts are not reliably composited).
(2) Tulip mode — `make mpy-web`: micropython's webassembly port +
the binding via the `bindings/surfer/web/` VARIANT_DIR (freezes
tulip.py + gamma9001; `index.html` is the host page). Key rules
learned, all load-bearing: the MP VM must NEVER suspend (an ASYNCIFY
suspend inside import machinery wedges the VM; inside a sync ccall it
aborts), so the browser drives frames — tulip.py skips its loop on
sys.platform == "webassembly" and JS calls `tulip.frame()` per rAF
(setTimeout fallback when hidden). That requires: pyscript-style
deferred GC (standard variant's gc_collect suspends via
emscripten_scan_registers), `SDL_HINT_EMSCRIPTEN_ASYNCIFY=0` +
no-PRESENTVSYNC (SDL sleeps in SwapWindow/Delay by default under
ASYNCIFY), and hal_sdl compiled as a direct usermod TU with
SURF_HAL_SDL_NO_YIELD (emscripten drops EM_JS bodies that come from
static archives — links fine, JS function silently missing).

M7 sprites: runtime images + transformed sprites, all three backends.
`surf_image_from_png` (stb_image, vendored in tools/stb/, PNG-only,
decode at load time — never in the frame path) → ARGB8888 with the
64-byte stride rule; any size. Sprites gained `surf_sprite_set_xform`:
uniform scale (Q16, clamped to the PPA's 1/16..16) and rotation in
quarter turns CCW (the PPA SRM limit). Transformed draws go through the
new hal op `xform_blend` (see DESIGN.md §5.4-decided); moving a sprite
is just node damage — the compositor repaints what it uncovers. MP API:
`surfer.image(png_bytes)` → Image (w/h, destroy(); sprites hold a ref
so the GC can't free pixels in use), `surfer.sprite(img, x, y)` with
`.scale` (float) and `.rot` (degrees, multiples of 90). Demo:
`import space` in tulip mode (examples/space.py + space_assets.py —
Kenney CC0 art baked to bytes by tools/pngwrap.py; the five source
PNGs live in assets/kenney/). Frozen into web + SURFER_P4. Verified: unix shot,
web (anim delta + frame dump), P4 runs it without PPA errors
(on-panel eyeball pending).

Tulip mode for the P4 is VERIFIED ON HARDWARE — REPL on the panel,
USB keyboard typing, touch live (MICROPY_HW_ENABLE_USBDEV=0 in the
board config is what frees the OTG PHY for host mode). Build: `make mpy-p4` — micropython v1.28.0 (`~/micropython-1.28`,
first P4-capable release) + IDF v5.5.1 (`~/esp/esp-idf-v5.5.1`, MP's P4
code needs 5.5 APIs; the native firmware in `ports/esp32p4/` defaults to
5.4.1 — but see the rev v3.x note below). The binding is split over a tiny port layer
(`bindings/surfer/surfer_port.h`): `port_sdl.c` for desktop,
`port_p4.c` for device — EK79007 DSI panel + GT911 touch brought up on
core-IDF APIs only (no BSP/managed components; wiring constants
documented in-file), assets copied flash→PSRAM at init, and
`usb_kbd.c`, a raw-usb_host HID boot-protocol keyboard for the USB-A
port. Board def `bindings/surfer/boards/SURFER_P4/` freezes tulip.py
(6MiB app partition — the binary carries the baked assets). Flash with
`make mpy-p4-flash PORT=...`; the board boots straight into tulip
mode (frozen main.py) — Ctrl-C on the serial console drops to the REPL.
Soft-reset re-inits the C scene (mod_init tears down on re-entry).
Remaining: on-device tulip verify, M6 web build + real art.

**Two P4 silicon revisions — images are NOT interchangeable.** Espressif
split the P4 at chip rev v3.x (marketed "P4X"); IDF < 5.5.3 cannot build
for v3.x at all, and a v1.x image will not boot on it (or vice versa).
The bench board reports rev **v3.2**. For it, build `ports/esp32p4/`
with IDF v5.5.3 and the rev-3 overlay:

```
source ~/esp/esp-idf-v5.5.3/export.sh
idf.py -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.rev3" \
       -p /dev/cu.usbmodem<...> flash
```

Plain `idf.py build` (IDF 5.4.1, no overlay) still targets the v1.x
board. One code-level trap, not config: leave `bsp_display_config_t`'s
`.phy_clk_src` at 0 — on IDF ≥ 5.5.3 `MIPI_DSI_PHY_CLK_SRC_DEFAULT` is a
compat alias for the LEGACY PLL_F20M reference, illegal on v3.x, and the
image compiles clean then abort()s inside `esp_lcd_new_dsi_bus` at boot
with no message. Measured on tulip2's identical part: compose cost at
large damaged areas fell 42–51% (wider PPA SRM block, 8×8 → 32×32); the
~85 µs per-op floor is unchanged, so "bake at final size" still holds.

## The P4 hal writes back before a PPA copy, or the CPU's cells are lost

**IDF's PPA driver invalidates the ROWS it is about to write, whole and
`pic_w` wide, before an SRM copy or a fill** (`ppa_srm.c` /
`ppa_blend.c`: M2C over the output's "extended window"). The textgrid
paints its cells with the CPU straight into the framebuffer through
`fb_ptr`, and this hal wrote them back once per frame at present — so
within one compose, cells painted BEFORE an opaque blit on the same
rows were dropped from cache before they reached memory, and memory
kept what the forward copy had put there: the previous frame. On the
glass that was a sprite leaving copies of itself along the launcher
panel's edge in tulip2 — the erase of its old position, CPU-painted by
the console grid, discarded when the panel's skin was copied on those
rows, and only outside the panel because the panel repainted the rest.
Never on the SDL hal, and not the compositor's fault.

A BLEND is safe: the driver writes back its `in_bg` first and `in_bg`
is this buffer. A COPY or FILL is not. So `h_blit` and `h_fill` write
back the rows their block spans (`fb_writeback_rows`) — only while
`cpu_wrote` says `fb_ptr` has been handed out since the last present,
which clears it on every path, so a frame with no textgrid pays
nothing and a frame with one pays a C2M over mostly clean lines. The
rotated path already synced `S.comp` before its first rect; it is the
same flag now.

## The P4 vsync callback lives in IRAM, for a host that writes flash

The DPI panel is refreshed by an interrupt: IDF's driver restarts the
frame DMA from its transfer-done ISR every frame. During a flash erase
or program the cache is off and every ISR not marked cache-safe is
masked, so the panel is not fed, the DSI bridge underruns and the
glass goes blank or blue for the length of the operation -- a host
writing its own flash (a USB disk handed to a computer, a settings
save) sees the screen flash on every sector. IDF's answer is
`CONFIG_LCD_DSI_ISR_CACHE_SAFE`, and under it
`esp_lcd_dpi_panel_register_event_callbacks` REFUSES a callback that
is not in IRAM. So `vsync_cb` carries `IRAM_ATTR`: it reads and writes
statics in DRAM and gives a semaphore from ISR context, which is in
IRAM unless a host moves FreeRTOS into flash. The option itself is the
host's to set; nothing here needs it to be on.

## The hal-shift smear rule

Four paths hand a rect to the hal to shift in place — the layer's
band_shift, the **sprite's fast pan** (a camera window walked over a big
opaque image), the scrollview's scroll_rect, and the textgrid's. All four
drag the pixels of whatever is painted ON TOP of that rect, which then
has to be repainted where it actually is.

The layer used to repair only its LATER SIBLINGS, which is right only
when every overlay is a sibling. It is not: tulip's task bar and its
console scrollbar are siblings of an app's GROUP, not of the scrolling
node inside it, so they smeared across the screen at whatever rate the
thing under them was scrolling — three layers, three different rates, one
very funny bug report.

`surf_damage_above(n, area, gx, gy)` (node.c) walks the whole paint order
after `n`, skipping its own subtree, and damages anything overlapping.
All four paths use it. tests/test_layer.c and tests/test_sprite.c have
the regression: an overlay in a different branch, which fails against the
sibling-only walk.

The sprite copy of the walk was found a day later, by forest (a sprite
camera over a baked world) smearing the same task bar the layer fix had
just stopped smearing — so if a fifth shift path ever appears, this is
the paragraph it has to read. Its test needs an **opaque** image: fast
pan is gated on `img->opaque`, and with a transparent one the slow path
runs, damages everything, and the test passes with the bug in place.

**The walk skips a HIDDEN subtree whole**, the same early out `collect()`
takes — a branch that paints nothing had no pixels to drag. It used to
gate the flag PER NODE and recurse regardless, which is wrong for the way
a host actually hides things: tulip hides a backgrounded app's GROUP and
never touches its children, so all ~1100 of them passed the test
individually, went through `surf_node_abs_pos` and landed in the dirty
list — where past `SURF_MAX_DIRTY` (32) entries degrade to a bounding
union, turning one scroll into a full-screen compose (20.7 ms on the P4X,
most of a 30 fps budget). It stayed latent only because `UIScreen.present()`
re-adds the presented group LAST, putting every backgrounded app before
it where `after` is still false; the launcher's full-screen scrim, added
after, is what reaches it.

The skip needs no exception for `stop`. It briefly had one — the walk
descended into a hidden subtree when the shifting node was under it,
because back then a node inside a hidden group could still `band_shift`
and an overlay above it would have been left smeared with nothing ever
repairing it. Fixing the gates below removed the case that hatch existed
for, and it went with them: a branch that paints nothing can no longer be
the branch that moved pixels.

### ...and a shift path must first ask whether it is visible AT ALL

Repairing what is above the band is the second question. The first is
whether this node owns those pixels at all, and **the node's own HIDDEN
flag is not that test** — a node inside a hidden group paints nothing
either. Every gate asked `n->flags & SURF_NF_HIDDEN` alone, so a layer or
camera animating inside a hidden group handed the hal a `band_shift` for
a band it does not own and dragged whatever IS on screen there sideways
at its own scroll rate. The same smear as the sibling-only walk, one
level up: the walk had the *repair* right and the *permission* wrong.

`surf_node_effectively_hidden(n)` (node.c, beside `surf_node_attached`)
walks self-then-ancestors, and **six** gates ask it, not four — the four
`can_fast` tests plus the layer's and sprite's sub-pixel keep-alive,
which issue a ZERO band_shift and drag pixels just as well. The fallback
needed no code of its own: the gate goes false, the slow path's
`surf_damage_subtree` runs, nothing is shifted.

Why the four paths were the only ones wrong, and where to look if a fifth
appears: `hit()` and `collect()` are recursive descents from the ROOT, so
a hidden ancestor prunes the subtree before recursion ever reaches the
child, and a per-node flag test is sufficient there. The shift paths are
the only ones a host calls **directly on a node**, sideways into the
tree, with no ancestor on the stack to have already said no.

Two things about testing it. The keep-alive only runs when a shift
already ran (`shifted`/`pan_shifted`), so a test has to do a **visible**
whole-pixel step first to arm it — hide the group first and that branch
is never reached and the assertion passes with the bug in place. And the
regressions live one per path (test_layer.c the layer and the sprite,
test_scroll.c the scrollview, test_grid.c the textgrid), because these
are four separate copies of one rule and always have been.

tulip2 cannot reach any of this today — a backgrounded app's `frame()` is
never called, so nothing animates while hidden. It was fixed anyway
because surfer is a general UI library and nothing stops a host from
animating a hidden group; the cost of being wrong is the whole screen
smearing, and the fix is one predicate.

## Animation is the filmstrip node, finally bound

`surf_filmstrip` has been in the core since M1 — it is how checkbox,
knob, led and selector are drawn — and was never reachable from Python,
so a strip of frames was a sprite you `set_src`'d by hand every frame.
`surfer.filmstrip(img, frame_w, frame_h, x, y)` binds it, with `.frame`
to pick a cel and `.fps` to play one. No new node type; the animation
support a host wants was already sitting here.

- **`fps` 0 is the default and means the CALLER owns the frame.** That is
  what a cel editor wants, and what a game stepping a walk cycle off its
  own physics wants. Anything else advances from `surf_tick` and wraps.
- **A COUNT of playing strips** (`surf_g.playing`), so the per-tick scan
  costs nothing at all when nothing is playing — which is almost always.
  Without it every tick would walk the whole 4096-node pool to find out
  that no animation exists. `node_free` gives the count back, or the
  scan keeps running for an animation nobody owns.
- **Late frames are DROPPED, not replayed.** A backgrounded tab or a
  long stall would otherwise flip through thousands of cels catching up
  on a cycle nobody watched.
- **`play([fps])` / `stop()` / `.playing` are the TRANSPORT, and it is
  a second axis on purpose.** stop() freezes the strip KEEPING its fps,
  so play() resumes at the speed it had — which zeroing fps cannot do —
  and play(fps) sets the speed first. fps is the SPEED (0 = the caller
  owns the frame); the transport is whether it runs. Conflating the two
  is how "how do I stop an animation?" got asked from the bench — fps=0
  then fps=N does restart (and always did: set_fps re-anchors and keeps
  the count parity), but it reads as a trick rather than an API. One
  `strip_active()` — fps set AND not stopped — feeds the playing-count
  parity in set_fps, set_playing and node_free, so the two axes cannot
  disagree about the bookkeeping; `test_filmstrip_play` drives both
  routes through the mock clock.
- The mock hal's clock used to be frozen at 0, which is fine for
  everything that only reads it and useless for anything that waits on
  it. `mock_advance_us()` drives it now; `test_filmstrip_play` is the
  regression.

### ...and it scales, which it silently did not

`surf_sprite_set_xform` opened with `if (n->type != SURF_NODE_SPRITE)
return;`, so `.scale` on a filmstrip **did nothing at all and read back
as 1.0**. That is the worst shape a setter can have: no error, no
exception, code that looks right, and a picture that never changes.
Reported from a Tulip as three turns spent asking an assistant to scale
an explosion — the assistant wrote the correct line every time.

The fix is a **shared `surf_xform`** (scale/rot/mirror) held by BOTH the
sprite and the strip variants, rather than a second copy of the triple.
A filmstrip is a sprite that picks its source rect from a frame index,
and past that point the two are the same picture-on-a-node — so the
clamp, the footprint arithmetic and the compose branch are one
implementation and cannot drift. `surf_node_xform()` is the accessor;
`NULL` for a node type that carries no transform, which is what makes
the setter and the three getters type-agnostic in one line each.

Two things it had to get right:

- **THE SOURCE IS THE CELL, NOT THE SHEET.** The sprite path hands the
  hal `n->u.sprite.src`; the obvious copy of it for a strip hands over
  the whole image, which scales every cel at once into one frame's
  footprint. It passes `{fx, fy, fw, fh}` — the same cell arithmetic the
  untransformed path already does, which is why that path offsets into
  the frame instead of starting at 0,0.
- **The footprint is one FRAME scaled**, not the image, so
  `sprite_update_size` asks `xform_source()` which of the two it is
  looking at. That is the only line where the variants differ.

`surf_filmstrip_new` sets `scale_q16 = SURF_ONE` explicitly for
`surf_sprite_new`'s reason: `node_alloc` zeroes the union, and a scale
of 0 is a footprint of nothing rather than "unscaled".

`test_filmstrip_xform` covers the footprint at scale and at a quarter
turn, that a transformed strip composes through `xform_blend` **with
the frame's own cell**, and that going back to 1:1 returns to the plain
blit — a fast path lost to a node that merely COULD be transformed is a
cost nobody would notice until the panel.

## A node can FADE, and the parameter was there all along

`node.opacity` (`surf_node_set_opacity`, 0..255 in C, 0.0..1.0 from
MicroPython) is one alpha multiplier over everything a node draws.

**It is not a new mechanism — it is a parameter nothing ever set.**
`hal->blend` has taken an `opa` since the hal was first sketched, and the
compositor called it with a hardcoded 255 in the one place it is reached:

```c
surf_g.hal->blend(img, src, dst, 255);       /* compose.c, before */
```

Both backends already honoured it end to end — the SDL loop multiplies it
into every one of its three format paths, and on the P4 it is the PPA
blend unit's own `fg_alpha_scale_ratio`. So the feature cost a byte on
the node and threading it through `paint()`, and on the device the fade
is done by the same hardware op that was already running.

**`xform_blend` had to grow the parameter, and that is the one real API
change.** It is `blend` with a transform in front of it, and a fade that
worked on a sprite and silently stopped working the moment somebody set
`.scale` is exactly the kind of half-working this repo pays for twice.
On the P4 it costs nothing: that function already ends in an `h_blend`
of its scratch buffer, so the opacity just rides it — and an OPAQUE
source now takes the blend path rather than the blit when faded, because
`h_blit` has nowhere to put an opacity.

**WHICH NODES, and the line is drawn by how they are painted.** Anything
that reaches the screen through `hal->blend`: sprite, filmstrip,
ninepatch, layer, and label (its glyphs are A8 blends, so it was one
argument). A rect, a textgrid and a textinput's chrome are `hal->fill`,
which has no opacity — a blended fill is a hal op that does not exist,
and adding one is a bigger decision than this was. `surf_node_can_fade()`
is the question.

**A GROUP IS REFUSED, permanently.** Fading a group means compositing it
whole and then blending the result once, which needs an offscreen render
target; blending each child separately is a different picture, and two
overlapping children show through each other where they overlap. surfer
composites straight into the framebuffer by construction (DESIGN.md §1),
so the honest answer is "fade the children" and not a plausible-looking
approximation.

**A REFUSAL RAISES.** `group.opacity = 0.5` is a TypeError naming what
can fade and what to do instead, following `.rot`'s precedent three
sections up — and for its reason, which is written down in tulip2's own
notes as an afternoon lost to a kitty that would not turn. A silent
no-op on the wrong node type is the most expensive recurring bug in this
codebase. Reads stay lenient and answer 1.0.

**IT IS VISUAL ONLY.** `surf_hit_test`, `surf_node_overlaps` and the ink
test all ignore it, so a fade cannot change what a finger or a collision
does halfway through. `hidden` still takes a node out of both.

**THREE THINGS IT TURNS OFF, and every one of them is a correctness bug
that leaves a plausible picture** — which is why `tests/test_opacity.c`
exists and why each guard was verified by breaking it on purpose:

- **the occlusion early-out.** A faded node covers nothing, so
  `node_opaque()` returns false for it and the front-to-back walk keeps
  going. Miss this and everything the node was hiding is simply not
  painted: not a wrong colour, a HOLE showing whatever the framebuffer
  last held.
- **`band_shift` streaming**, for a fast-panning sprite and for a layer.
  A shifted band holds pixels that are ALREADY this node composited over
  what was behind it; blending the node again over its own result blends
  twice. Both fall back to repainting and both come back at 255.
- **the 9-patch's solid-centre fill.** `region_is_solid` only says yes to
  a fully opaque block, so that one-op shortcut would paint the centre at
  full strength straight through the fade. Faded, the centre tiles.

**opa 0 never reaches the hal.** `collect()` drops the node outright —
free, and it keeps a zero away from the PPA, which documents
`fg_alpha_scale_ratio` as exclusive of zero. That is the shape of bug
this repo keeps meeting: fine on a laptop, an error return on the panel.

**And the byte is free.** `opa` lands in the padding before `parent` on
both a 32- and a 64-bit build — `sizeof(surf_node)` is 136 before and
after — so the node pool does not grow.

**What it COSTS is the occlusion loss, and it is real.** Fading a 64px
sprite is nothing; fading a full-screen backdrop means everything under
it is composited every frame it is faded. That is inherent to
transparency rather than a limitation of this implementation, and the
device's answer for a whole-screen fade is still the one the P4 shares
with a SNES: `INIDISP`-style, at the panel, not per node.

### ...and ANY property tweens, not just opacity

```python
spr.tween("x_pos", 300, 1000, ease="out")
spr.tween("scale", 2.0, 400, ease="in_out")
spr.tween("opacity", 0.0, 300)      # what fade_out is
spr.tween_cancel("x_pos")           # or tween_cancel() for all of them
spr.tweening("x_pos")               # or tweening() for any
```

**THE PROPERTY IS A NAME, AND IT HAS TO BE.** `spr.tween(spr.x_pos, ...)`
is the spelling everybody reaches for first and it cannot work: `spr.x_pos`
evaluates to the INTEGER 40 before `tween` is called, and Python has no
way to hand over the slot it came out of. A string is what
QPropertyAnimation takes and what Cocoa's keyPath is, for exactly this
reason.

**KEYED BY (node, prop), NOT BY NODE.** A dying enemy drifts up AND
fades, which is two tweens on one sprite; keying by node alone makes the
second replace the first, and the shape of that bug is a sprite that
fades perfectly and never moves. Starting a second tween on the SAME
property still replaces it, which is what reversing a fade mid-flight
means. A direct write cancels only ITS OWN property's tween —
`set_pos` kills an X or Y tween and leaves a fade running.

**VALUES ARE Q16 WHATEVER THE PROPERTY IS**, so the tick interpolates
one type and only `tw_write` knows the difference: opacity 0..255, x/y
in pixels, scale the Q16 the transform already used. The binding
converts from the property's own units, so a caller says `0.4` for
opacity and `300` for x_pos and never sees a fixed-point number.

**THERE IS NO ROT TWEEN, and that is the PPA rather than an omission.**
`surf_sprite_set_xform` refuses anything that is not a quarter turn
because that is what the hardware rotates in — so a "smooth" rotate
could only ever be a four-frame flip-book. Naming it raises with that
sentence rather than serving it badly.

**EASING ARRIVED WITH MOTION AND NOT WITH FADE**, deliberately. A fade
does not need a curve; a slide does — ease-out is the whole difference
between cheap and polished. Linear, in, out, in_out, quadratic, in fixed
point. Every curve lands EXACTLY on the target, which the test checks
per curve, because an ease that arrives at 0.998 is a sprite that never
quite gets there.

**...AND A VALUE NO NODE CARRIES TWEENS IN PYTHON**, not here:
`self.ui.tween(a, b, ms, fn)` in tulip2 walks a number and calls `fn(v)`,
on the app's own timers. The split is the one `after`/`every` already
make against task.py — node properties run in C (free, no Python per
frame, survives backgrounding), an app's own angle or volume runs in
Python where being general is cheap and freezing with the app is right.

### ...and it fades OVER TIME, without the app holding a clock

```python
spr.fade_out(300)          # to 0 over 300 ms
spr.fade_in(300)
spr.fade_to(0.4, 200)
spr.opacity = 0.4          # still the manual control, and it CANCELS a fade
spr.fading                 # is one running
```

`surf_tick` drives it, beside the filmstrip's own advance and for the
same reasons: the app writes one line and never touches it again, and
the fade **keeps running through frames the app is not being called
for** — so a tulip2 app backgrounded mid-fade comes back finished
rather than frozen half way.

**THE STATE IS A SIDE TABLE, NOT A FIELD ON THE NODE.** A tween is 16
bytes and almost no node ever fades; on tulip's 4096-node pool that
would be 64 KB of PSRAM to serve the handful of sprites fading at any
moment. 32 slots allocated with `surf_g` — DESIGN.md's rule that pools
are sized at init and the frame path never allocates — and the tick is
gated on a counter exactly the way `surf_filmstrip_tick` is, so it costs
one comparison when nothing is fading.

**IT LIVES IN `node.c` AND NOT IN A `fade.c`, and that is a build fact
rather than taste.** surfer's own Makefile globs `src/core/*.c`, so a new
file is picked up for the desktop and the web — and **tulip2's
`micropython.cmake` LISTS the core sources by name**, so the same file
would compile everywhere except the device and link with an undefined
symbol at the very end of a ten-minute build. That is this repo's
worked-on-the-mac shape with a new mechanism behind it.

Five decisions, four of which are things that fail silently:

- **A DIRECT `.opacity` WRITE CANCELS THE FADE.** Without it the tween
  overwrites the write on the next tick, so `spr.opacity = 1` in the
  middle of a fade-out appears to do nothing — the exact silent no-op
  the opacity section above spends a paragraph refusing.
- **A DESTROYED NODE DROPS ITS FADE**, in `node_free`. A slot holds a raw
  pointer into the node pool and the pool RECYCLES, so a fade outliving
  its node does not merely write through freed memory — it writes into
  whatever node was allocated next, every frame, invisibly. The test
  destroys a fading node, allocates another, and watches the new one
  stay put.
- **A late fade lands EXACTLY on its target**, and the slot is cleared
  BEFORE the last write: the write damages, and a half-freed slot seen
  from a damage path would be a fade that never ends.
- **A full table is INSTANT, not ignored.** A caller that asked to end up
  at 0 ends up at 0; what it loses is the animation, which is the only
  part that can be dropped without lying. Same for `ms <= 0`.
- **It does NOT hide the node at the end.** Opacity 0 already paints
  nothing and never reaches the hal, so auto-hiding buys nothing — and it
  would silently change hit testing, which the opacity note promises it
  never does.

Linear, deliberately. A fade is the one tween where an ease buys nothing
anybody can see, and easing curves are a menu that never stops growing.

**AND THE BAKED-FRAMES VERSION WAS MEASURED AND REJECTED**, because it is
the obvious idea and somebody will have it again. Pre-baking N
alpha-scaled copies of the image at load and swapping them with
`set_image` buys **nothing**: measured on a 128x128 sprite over the same
scene, 8.396 ms/frame against 8.335 for `node.opacity` — 1.01x, inside
the noise — for **704 KB** of extra image memory. The reason is
structural rather than incidental: a pre-faded ARGB copy is not opaque
and a node at opa < 255 is not opaque, so both take the identical
compositor path, both lose the occlusion early-out and both blend
instead of blit. The prebake dodges not one cost. It was the right
answer only while the alternative was a per-pixel Python loop, and it
stopped being one the moment the hal's own `opa` was wired up. Where a
load-time bake DOES earn its keep is COLOUR — a red ship in green —
which node opacity cannot do and A8 tint cannot do for multi-colour art.

## A sprite's picture is NOT fixed at birth any more

`surf_sprite_new(const surf_image *img, ...)` took the image and nothing
ever repointed it, so a slime that died meant **destroying the node and
building another in its place**. Reported from a Tulip in exactly those
words, and the answer up to now was the cheap one: bake both states into
one image and `set_src` a cell. That really is cheaper — one decode, one
allocation, one thing the compositor tracks, and it is how the widgets,
a 52-card deck and an icon set are all drawn — but it only covers
pictures somebody baked TOGETHER. Two files, two sizes, or an image
rendered at runtime had no answer at all.

`surf_sprite_set_image(n, img)` -> bool, `node.set_image(img)` from
Python. Sprites and filmstrips both, for the reason the xform above
shares one implementation: past the source rect they are the same
picture-on-a-node.

- **The src window RESETS to the whole new picture**, which is what the
  constructor would have given, and the footprint follows. Keeping a
  window into the OLD image would be a cell number that means nothing
  in the new one. Swapping in the image already there is a NO-OP and
  deliberately does not reset a window — every other setter here
  early-returns on no change, and a caller wanting the whole picture
  can say `set_src` in the same breath.
- **DAMAGE BOTH RECTS, old before the write and new after** —
  `set_src`'s own slow path. The two-line assignment version passes
  every "is it drawing the new picture" check and leaves the old one on
  screen wherever the new one is SMALLER, which is the shape of bug
  that reads as a compositor fault.
- **`pan_shifted` is cleared.** It is a claim about a band shift of the
  picture that just left; left set, the next `set_src` refreshes a band
  out of an image nothing points at.
- **Everything the CALLER put on the node stays** — scale, rotation,
  mirror, hitboxes, fast_pan, and a strip's fps and transport. Those
  describe how the node behaves, not what it is a picture of, and a
  swap that reset them would make the cheap thing (a state change) cost
  the expensive thing (a rebuild) by another route, which is the whole
  reason this exists. Hitboxes are the one worth arguing about, since
  boxes drawn round a live slime may not fit a dead one — but only the
  caller knows that, and clearing them silently is the worse failure: a
  node that has quietly stopped colliding looks perfect.
- **A strip smaller than its own cel is REFUSED**, the check
  `surf_filmstrip_new` already makes at construction, because a
  filmstrip with zero frames draws from a source rect that is not in
  the image. On a strip the cel size is kept and the frame COUNT is
  recomputed, the frame clamping into it — so a walk cycle becomes a
  death cycle in one call.
- **THE BINDING'S OWN HALF IS `img_ref`**, and it is why this could not
  be a one-liner over the core call: a node object anchors the Image
  object it draws from, so a swap that moved the C pointer and left the
  reference behind would root the picture nobody draws and leave the
  new one collectable — pixels freed under a live sprite, which on a
  Tulip is an exit with status 0 and no traceback anywhere. Rebound
  AFTER the core call takes it, never before, so a refusal cannot
  repoint the anchor. Python RAISES rather than returning False:
  every way it can fail is a mistake in the calling line, and a
  silently unchanged picture is precisely what `surf_sprite_set_xform`
  spent a release doing.

`test_sprite_set_image` and `test_filmstrip_set_image` cover the
vacated rect, the surviving properties, the clamp and both refusals.

## hits() counts INK, not the box

`surf_node_overlaps` (Python: `a.hits(b)`) reads the pixels now. Boxes
first, exactly as before — AABB on absolute positions, transformed
footprints, hidden/detached never hit — and where the boxes touch, a
sprite or filmstrip answers from its image's ALPHA: pixels under
`SURF_INK_ALPHA` (128) do not collide. The report that ended box-only
was exact and worth keeping: a ball "bouncing off a sword way before
contact" — a longsword is a diagonal of ink in a mostly transparent
square, so the box collided at the empty corner.

- **A lazy 1-bit mask per image** (`surf_ink`, image.c), built on the
  first overlap that needs it, ~w*h/8 bytes. It lives in a SIDE TABLE
  keyed by the image pointer, not in `surf_image`: images are
  legitimately `static const` (the tests build them that way) and on
  the device that is a struct in flash a cache cannot write into.
- **Every pixel mutator drops the mask** — fill, blit, scale, the four
  shape calls — and so does `surf_image_flush`, which is the publish
  point a buffer-protocol writer already owes the PPA. A writer that
  never flushes gets a stale mask AND stale pixels: same contract, not
  a new one. `surf_deinit` clears the table, because a soft reset frees
  images whose addresses malloc hands out again.
- **The inverse map is `h_xform_blend`'s arithmetic exactly** (rot
  CCW, mirror flips the source before rotation, the filmstrip's source
  is its FRAME cell) — so what collides is what is drawn. If the hal's
  sampling ever changes, `ink_at` in node.c changes with it.
- **The box fallbacks are the design, not gaps**: rects, groups, labels,
  RGB565, anything `opaque`, and an OOM during the build all stay boxes.
  Two boxes still short-circuit before any pixel is read, so
  every-bullet-vs-every-enemy costs what it always did; the per-pixel
  walk runs only over the INTERSECTION of boxes that already touch,
  which at the moment of contact is small. Legal by the colorpicker's
  rule — it runs on an event (an app asking), never in the compose path.

tests/test_ink.c holds all of it: the sword-and-ball case, solid-stays-
box, scale, rot/mirror against the hal's mapping, invalidation after a
fill, and a filmstrip colliding with the frame it shows rather than the
sheet.

## Hitboxes ride the transform

`surf_hitbox_*` (node.c): caller-authored collision rects on a sprite
or filmstrip, at most 32, offsets in the UNROTATED source frame —
negative or past w/h is legal, a reach that sticks out of the picture.
`surf_hitbox_abs` maps a box through the transform with the FORWARD
form of `ink_at`'s inverse, derived case by case from it so the two
cannot disagree about what a quarter turn means — which is the whole
point: a box authored on the head stays on the head through every
mirror and turn, and the class of per-facing reposition code a Tulip
user spent four model-assisted rounds fighting is gone.

- **While a node has hitboxes they ARE its collision shape.**
  `surf_node_overlaps` hands off to `surf_node_overlaps_which` (a
  bitmask of a's boxes that hit), the node's own box and ink stop
  mattering, and the OTHER node's ink still counts. Nothing in the
  hitbox path early-outs on the NODE boxes — a box may sit entirely
  outside its sprite, which is exactly the case the early-out would
  wrongly kill.
- **The binding returns the indices**: `a.hits(b)` is a tuple when `a`
  has hitboxes (empty = falsy, so every `if a.hits(b):` keeps working)
  and the bool it always was when it has none.
- **Debug outlines are REAL RECT NODES built by the binding** in the
  sprite's parent, rebuilt on the mutations that move things (position,
  transform, box edits) — never the frame path. An outline drawn by the
  compose path outside the sprite's own box would break dirty-rect
  damage accounting, which is why this is scene chrome and not a
  compositor feature. `hitboxes[i].visible / .visible_color` drive it.
- Storage is a malloc'd array per node, freed in node_free; add/remove
  are events. tests/test_hitbox.c holds the mapping (against the
  framebuffer-probe rotation convention), the outside-the-node case,
  both-sides-with-boxes, and the four-facings head-box case whole.

## An image can be saved now

`surf_image_to_png` / `surfer.write_png(img)` — the other half of
`surf_image_from_png`. An image can be drawn into (the shape API, a
caller writing its own pixels through the MicroPython buffer) and until
this there was no way to get one back out, so anything that made a
picture could show it and never keep it.

**It is C because that was measured, not assumed.** The same encoder in
MicroPython costs 8 ms for a 320x48 strip on a DESKTOP against 0.51 ms
here, and 43 ms for 704x64 against 1.52 ms — on a device that runs
Python-heavy loops 20-60x slower again. And it does not merely get slow:
the pure-Python path has to build the whole raw image as one bytearray
before deflating, which for a 2556x284 sheet is 2.9 MB and raised
MemoryError on the laptop. The C path encodes that same sheet in 22 ms.

`stb_image_write.h` is vendored beside `stb_image.h` — same author, same
public-domain terms, and the decoder was already a runtime dependency.
`STBI_WRITE_NO_STDIO`, so no `fopen` is linked on a device that has no C
filesystem: the only entry point compiled is the to-memory one, which is
what a binding wants anyway.

Two details worth keeping: A8 encodes as white-with-that-alpha, because
an A8 image is a MASK whose colour lives in the node's tint and baking
the tint in would save a picture nobody drew. And the encoder is
faithful to the pixels it is given — a colour that went in through
`surf_image_fill` as RGB565 comes out 0xf8 rather than 0xff, since that
call widens 5 bits to 8 by shifting, and the encoder does not invent the
missing three bits back.

## A low-poly glTF model renders into an Image

`surfer.mesh(glb_bytes[, tex_png[, textured]])` → Mesh; `m.render(img,
rx, ry, rz, size[, cx, cy[, cull]])`, `.tris`, `.destroy()`. C API:
`surf_mesh_from_glb` / `surf_mesh_render` / `surf_mesh_tris` /
`surf_mesh_destroy` (src/core/mesh.c). This is the software-renderer
case the Image buffer and `surf_image_flush` were built for, so it obeys
their contract exactly: the caller renders on its OWN call (an app's
frame, never the compose path), flushes once after the last write, and
damages the sprite showing the image — and the sprite's `.scale` does
the enlarging, which on the P4 is the PPA's SRM block and free. Like
fill/poly/lines, render does NOT flush for you.

**The loader is the design.** A .glb is parsed by a self-contained
jsmn-shaped tokenizer (no new dependency — the JSON is offsets into the
chunk, never copies), node transforms are FLATTENED at load, and every
triangle gets ONE color: the material's baseColorFactor times its
baseColorTexture sampled at the face's **UV centroid**. That sample is
exact, not approximate, because of what low-poly art is — Kenney's kits
keep every face inside one flat region of a 512x512 palette texture
(measured: a coin's u coordinate is a single constant) — so the texture
is decoded once, read per face, and FREED. What survives a load is ~20
bytes a triangle and no sampler in the inner loop.

**`textured` (SURF_MESH_TEXTURED) is the other bargain**, for painted
models whose detail lives INSIDE a face — bigball's lucky cat carries
its eyes, whiskers and the kanji on its coin as texels, and the
centroid sample collapses every face to one color, which is how those
models shipped faceless. Under the flag the decoded textures stay
resident, per-corner UVs survive the load (texture transform already
applied), and the rasterizer samples per pixel — nearest texel, affine
UV, which at MESH_CAM's mild perspective is under a texel of error on
low-poly faces. A face with no texture renders flat either way, so the
flag is safe on any model; what it costs is the texture's memory for
the mesh's lifetime and a sampler in the inner loop (the lucky cat at
480px measures 325 µs against 233 flat on the desktop).

**The light factor is gamma-lifted before it multiplies the color**
(`lit_gamma`, a 129-entry table over `l^(1/2.2)`), because the color
bytes are sRGB-encoded and sRGB is near enough a pure 2.2 power that
correcting the FACTOR equals lighting in linear:
`encode(decode(c)*l) == c * l^(1/2.2)`. Multiplying the bytes raw
darkened a 0.6-lit face perceptually like a linear 0.33 — reported as
every model rendering darker than the same file in a web viewer, and
the tests/test_mesh.c white-face check pins the lifted value (r5 29,
not the gamma-space 27) so it cannot quietly regress. Face normals are
computed from the world-space triangles, so the NORMAL and TANGENT
accessors are never read — which is also why tulip2's baker strips them
and halves the shipped bytes. COLOR_0 (float/u8/u16) works where a
model has no texture; a texture referenced by URI (Kenney keeps one
colormap.png beside its models) is supplied as `tex_png`. Everything
read out of the bin chunk is BOUNDS-CHECKED and a hostile index drops
its triangle: these are bytes off a network.

**Depth interpolates 1/(CAM − z), and that is correctness, not
preference**: it is the one depth that is affine in screen space under
perspective, so intersecting geometry (a wheel through a car body)
sorts per pixel and exactly. The rasterizer is a scanline fill against
a u16 z-buffer — per pixel it is a shift, a compare and two stores —
into RGB565 or ARGB8888, and the z-buffer and projected-vertex scratch
are shared statics grown on demand and freed by `surf_mesh_reset()`
from `surf_deinit`, the ink table's lifecycle. Float math and malloc
are fine here for shape.c's reason.

**`cull` is an argument because the files lie about it.** UnityGLTF
stamps `doubleSided: true` on everything, and honoring that (cull=-1,
the default) draws every back face into the z-test. Measured on the
desktop at 160x160, 550 tris: 35 µs culled, 77 µs both sides — the same
picture for 2.2x the cost on a closed model. cull=1 forces the cheap
answer; 0 forces two-sided (a flag's cloth, a leaf card). Back faces
that do draw are lit from their own side, so two-sided art shades
rather than going black. glTF front faces are CCW and the projection's
y flip makes them clockwise on screen — negative signed area — which is
the sign the cull tests and `tests/test_mesh.c` pins with a cube whose
every face is a different color.

The model is centered and normalized to radius 1 at load, so `size` is
simply the radius in PIXELS wherever the file's units came from, and
`rx/ry/rz` are degrees — ry spins, rx tilts, rz rolls, a turntable's
order. Loading is the expensive half (~1 ms on the desktop for a 550-tri
model); do it once, at build time.

## A key event carries CTRL, and the tuple is four long

`surfer.keys()` is `(kind, text, shift, ctrl)`. It was three, and the
fourth exists because **a modifier on a key with no control character of
its own had nowhere to live.**

ctrl+LETTER has always worked and still does not use the flag: a driver
turns it into the character a terminal puts on the wire (^S is 0x13) and
it arrives as `KEY_TEXT`. ctrl+Delete, ctrl+arrow, ctrl+Home/End and
ctrl+PgUp/PgDn have no such character, so both drivers did the only thing
they could and DROPPED the modifier — `chord = false; /* ctrl+arrow still
arrows */` in the SDL hal, `break; /* ctrl+arrow etc: plain keys */` in
tulip2's `usb_input.c`. Every one of those chords was therefore
indistinguishable from the bare key, which is how a Tulip user found it:
tulip-pye binds delete-word to ctrl+Del and delete-line to shift+Del, and
they are the only two entries in its keymap with no ^-chord alternative,
so they are the two that had no way to work at all.

- **A flag, not a private code.** ^Tab's answer — invent a character
  (0x1e) and send it as text — is right for ONE chord that means one
  thing and wrong as a general rule: a ctrl+Left delivered as a private
  character stops being a LEFT, so a widget switching on `kind` no
  longer sees an arrow, and every consumer needs the table. A flag
  leaves the key what it is.
- **NOT a bitmask in the `shift` slot**, which was the tempting
  non-breaking version — bit 0 is shift, so `if shift:` keeps working
  and no unpack anywhere has to change. It is wrong: `if shift:` is
  then also true with ctrl alone held, so `surf_textinput_move(n, -1,
  shift)` extends a selection on ctrl+Left. A modifier nobody asked
  about must read as false, so it is a real fourth element and every
  `kind, text, shift = k` in both repos was widened.
- **ctrl+LETTER does NOT set it**, and neither do ctrl+A/ctrl+E, which
  are delivered AS Home/End for readline. The modifier is already spent
  in what was delivered, and a consumer seeing both would apply it
  twice — ctrl+A would jump to the top of the document instead of the
  start of the line.
- **ctrl+DIGIT and ctrl+PUNCTUATION set it on neither backend.** The SDL
  hal has no scancode case for them and pushes nothing at all; the
  device driver clears the flag on its `base_map` path to match. A
  modifier one platform reports and the other cannot is how a host grows
  a chord that works on a laptop and not on the panel.
- **The held set reports it too**, for the reason it reports shift: it is
  a snapshot of the keyboard, and one that answers "shift is down" while
  staying silent about ctrl is lying by omission. The pad mapping
  ignores both.

`surfer._key(kind, text, shift, ctrl)` takes it, which is the only way a
headless test can reach one of these — ctrl+letter is a control character
a test can simply type, and ctrl+Delete exists ONLY as this flag.

`Node.key(k)` reads ctrl off the tuple and ignores it: a textinput is one
line with no word motion, so every chord means what the bare key means.
It takes `len < 2` and looks no further, so a tuple of either length
still works there.

## Textinput, from MicroPython

`surfer.textinput(x, y, w, color, font)` is the editable-text node, and
it draws the TEXT and nothing else — no box, no border, no keyboard, per
DESIGN.md §2.5 — so a field in practice is a `rect` with one of these on
top of it.

Two things the binding adds over a literal wrapping of the C calls,
because every caller would otherwise write them:

- **A tap places the caret, a drag extends the selection.** That is what
  a text field *is*, so `ti_touch` is installed at creation. Setting
  `.on_touch` from Python does not lose it: the setter installs the same
  handler, which moves the caret and then calls the Python callback.
- **`.key(k)` applies ONE event from `surfer.keys()`** — the
  `(kind, text, shift)` tuple — and returns whether it consumed it, so
  Enter and hotkeys fall through to the app. **Tab is refused too**:
  the hal pushes it as TEXT `"\t"` (there is no KEY_TAB), and inserting
  it put an invisible tab into a wifi password nobody could then see.
  A host that moves focus between fields does it on the False:

  ```python
  for k in surfer.keys():
      if not field.key(k):
          ...     # yours
  ```

Everything else is a thin pass: `.text`, `.caret`, `.focus(on)`,
`.insert()`, `.backspace()`, `.delete()`, `.move(delta, extend)`,
`.index_from_x(local_x)`. Every `surf_textinput_*` entry point guards on
the node type, so these are safe no-ops on any other node — except
`set_text`, which the binding routes explicitly, since the two node types
have separate C setters that each ignore the other's node.

`surfer._key(kind, text, shift)` pushes one event into the queue a driver
feeds — the counterpart of `_touch`, and the only way a headless test can
reach anything that reads the keyboard.

**`KEY_ESC` is a key, not a window command.** The SDL pump used to
`return false` on Escape, which closes the window — fine for a C demo,
catastrophic for a host: on tulip2 one Esc took down the REPL, every
running app and anything unsaved, from the key people press to mean
"cancel what I just started". It is queued like Home or End now and what
it MEANS belongs to the host; the demos still close on the window button
and on ctrl+C. The device path agrees by construction (HID usage 0x29 in
tulip2's `usb_input.c`) — a chord that works on one platform and not the
other is the exact shape of the old ctrl+letter bug.

## LED and selector

Two panel controls, added together for tulip2's TB-303.

`surf_led` is the only widget that **reports nothing** — a lamp is an
output, so it has no callback. The art is A8, and each LED keeps its own
COPY of the `surf_image` struct (shared pixels, its own `tint`), which is
how one asset serves every colour: on the P4 the tint is a palette
register the PPA applies at blend time, so `set_color` costs a repaint
and no pixels. Brightness is a **level, not a bool**, so a blink can
fade, and frame 0 is the unlit lens rather than nothing — a dead LED is a
visible dark bead. Its unlit alpha is 0.55 because these sit on white
piano keys as well as black panels, and a faint red over white reads as
pink rather than as an off lamp.

`surf_selector` is a knob with **detents**: N fixed positions, reporting
an index. It shares the knob's filmstrip and lands on the frame nearest a
detent, so N is a runtime number needing no art of its own. Two gestures,
because a panel control wants both — a vertical DRAG that snaps as it
goes, and a TAP that advances one position and wraps, which is how you
nudge a 4-position mode switch with a finger. A tap is a press that
travelled under 6px, decided at UP.

Both are bound: `surfer.led(x, y, color)` and `surfer.selector(x, y, n)`,
with `.value` a brightness (or True/False) and an index respectively.

## Tabs, and the half a caller cannot do well

`surf_tabs` is a strip of labelled buttons with a PAGE behind each, and
the widget owns which page is showing. Added for tulip2's settings app,
which had four panels tiled into one screen and no room for a fifth.

**Drawing the strip is the easy half.** What is not is what happens
underneath: every node of page 2 hidden while page 1 is up, and the swap
in ONE place when the index changes. A caller doing that by hand keeps a
list of groups AND its own shadow of which is showing — `hidden` is
write-only on a node, deliberately, so there is nothing to read back —
and gets it wrong the first time a page is added after the fact. Here
the page is the widget's: `surf_tabs_page(t, i)` hands back a group to
fill and nothing else ever has to know it exists.

- **The art is a TAB, and that is not decoration.** A tab is a card whose
  bottom edge IS the page it belongs to: rounded at the top, dead flat at
  the foot, drawn in the page's own background so the join disappears.
  The first version reused the button's 9-patch — rounded all round, in
  the button's baked colours — and came back from the bench as "more like
  buttons than tabs", which was exactly right: nothing about it said the
  page below was the same object.
- **A8, so the colours are the CALLER'S.** `style->face` is meant to be
  the page background and `style->dim` is every other tab; the widget
  keeps tinted copies of the image struct, which is the knob's trick
  for the knob's reason — a tint is a palette register on the P4, so
  another colour costs no asset and no pixels. `surf_tabs_set_face` /
  `set_dim` move the whole strip for a caller whose theme changes
  underneath — and the copies are PER TAB now (a struct per tab per
  state, bytes and no pixels), so `surf_tabs_set_face_at` / `set_dim_at`
  dress one tab in its own pair. That is for a strip whose pages each
  carry their own paper (tulip2's world app: grey files, blue chat,
  green games): the bright face is that page's background, so the join
  rule holds on every page with nothing chasing the selection — and
  press feedback shows the colour you are about to get — while the dim
  one is a darker shade of the same, so an unselected tab still says
  which page it opens. Bound as `tabs.set_face(i, c)` /
  `tabs.set_dim(i, c)`, beside `set_label`'s per-tab shape; `.color`
  keeps its old meaning, every face at once.
- **Two labels per tab, one hidden.** A label's colour is baked when the
  node is made (`set_color` is a silent no-op on text), and the current
  tab has to read louder than the rest — so the bright one and the dim
  one are separate nodes, which also lets a caller hand a BOLD face to
  the active one alone (`style->font_active`).
- **ONE handler on the strip**, not one per tab. The index is arithmetic
  on the x that came in — dropdown does the same with its rows — so a
  five-tab bar costs five nodes for its faces rather than fifteen.
- **A touch below the strip is the page's business.** The handler is on
  the strip and nothing else, or every control on a page would change
  the page.
- **A page is a clipped group**, so content cannot spill past the area
  the caller asked for, and the group can carry a handler of its own.
- `h` is the WHOLE height, tab strip included; pages get `h - tab_h`.
  That is the number a caller laying out a panel actually knows.

`test_tabs` in tests/test_widgets.c checks the hiding by COMPOSING and
looking at what the hal was told to fill, since there is no way to read
`hidden` back. Worth knowing if you write a test like it: the colours
have to be bright. The first version used `SURF_RGB(1, 2, 3)`, which
packs to 0 in 565 — the same value the screen is cleared to — so the
check could not fail.

## Radio: one of N, in a column or a row

`surf_radio` is the checkbox's sibling and deliberately not a variant of
it. A checkbox answers yes/no about ITSELF; a radio answers "which one"
on behalf of a group, and every platform draws that distinction — a ring
with a dot, not a box with a tick — so users read it without being told.
Three checkboxes and a rule is not the same widget.

- **Both axes.** A column is the settings-panel shape (macOS's
  "Automatically / When scrolling / Always"); a row is what a strip
  wants — `( ) AMY out  (o) Audio in` on one line. Only where the next
  option starts differs, so it is one widget with a flag.
- **A row's options are as wide as their LABELS**, so the per-option
  extents are measured at build time and kept rather than being
  arithmetic on a pitch the way a tab strip's are. `surf_radio_size()`
  reports what it measured, because a caller laying out around one
  cannot know it either.
- **One handler on the root**, tabs' rule: a group and a closure per
  option would be three nodes each for a widget that is mostly text.
- **It fires on RELEASE**, like the checkbox and the button — a press
  that slides off is a mind changed, not a choice made.
- The art is a two-frame filmstrip in ARGB rather than A8, matching the
  checkbox beside it: a radio and a checkbox on one panel that disagree
  about their own greys look like two libraries.

## The display can change shape under you

`surfer.init(w, h)` on a LIVE scene is the soft-reset path — the VM
dropped every Python object, so the C scene is rebuilt on the surviving
hal. That was true right up until a host asked for a **different size**:
the hal allocates its framebuffer at the size it was built with and has
no way to be told otherwise, so re-initialising the core alone composes
w-wide rows into the old stride and writes past the end of every one.

So a size that MOVED re-shapes the display, through the port:
`surfer_port_resize(w, h)` → `surf_hal_sdl_resize`, a new texture and
framebuffer on the SAME window, renderer untouched. The same size keeps
everything as it was, which is the common case by far and is what makes
a soft reset free. A panel port answers false and is never asked — the
size it reports cannot change.

- **IN PLACE, not a quit-and-init pair**, which is what this was first
  and is the part worth keeping. Tearing the display down destroys the
  platform's own window and view objects, and on iOS that left a
  KEYBOARD NOTIFICATION aimed at an `SDL_uikitviewcontroller` that had
  gone: the app segfaulted inside UIKit — `objc_retain` under
  `-[SDL_uikitviewcontroller updateKeyboard]` — on the SECOND rotation,
  from an observer nothing in this repo can see. Re-shaping touches
  only what the hal itself allocated, and the whole class goes away.
- **Both allocations succeed before either old one is freed**, so a
  failed resize leaves the caller the display it already had rather
  than none.
- **`SDL_SetWindowSize` is called too, and on iOS that is load-bearing
  rather than cosmetic**: `UIKit_GetSupportedOrientations` derives the
  orientations the app is ALLOWED from the window's own aspect when the
  window is not resizable (SDL_uikitwindow.c), so a host that has just
  become landscape stays locked to portrait until this lands. A
  fullscreen window keeps the screen's size regardless; what changes is
  what SDL believes it was asked for. (The host still has to ask UIKit
  to re-evaluate — `setNeedsUpdateOfSupportedInterfaceOrientations` —
  which is its business, not this library's.)
- **`prepare_assets()` is NOT re-run.** It re-homes each atlas once, in
  place — on the device, flash .rodata into a fresh PSRAM allocation —
  so a second pass would leak the first copy. Those pixels belong to
  the process, not to the hal, and outlive it.

## Host chrome at BOTH ends

`surf_host_chrome_q16` is how much of the window height a host wants
kept clear at the bottom for chrome of its own (tulip2's iOS key bar);
`surf_host_chrome_top_q16` is the same at the top, which is a notch, an
island or a status bar. Q16 fractions rather than points, because the
host measures in its own coordinate space and SDL's window height does
not always agree with it. Weak zeroes in input.c, so a host that draws
none needs to know nothing about either.

**The top one exists because clearance cannot be bought with slack.**
The obvious way for a host to dodge an island is to ask for a SHORTER
framebuffer and let `update_view`'s centring push the picture down —
which works, costs double (slack is split, so 62 points at the top
costs 62 at the bottom), and then **fails completely the moment a
screen keyboard is up**: the fit is against the short band above the
keyboard, there is no slack at all, and the machine lands hard against
the top of the screen with its first line under the island. Reported
from a real iPhone as a Tulip cut off at the top. Reserved here, the
band is clear in both cases and the doubled cost goes away.

## The desktop window

`update_view` fits the drawable, preserving aspect: an exact multiple
when the window is one (within ~2.5%, so a hair off 2x IS 2x), the
largest aspect-preserving fit otherwise, centred and letterboxed.

**On a platform with a SCREEN KEYBOARD (iOS) the view anchors to the
TOP while the keyboard is up** rather than centring under it — SDL
neither resizes the window for the keyboard nor says how tall it is, so
a centred view sits half-hidden behind the thing you type on. Its
comings and goings also generate NO SDL event (the user dismisses it
from its own key), so the pump polls `SDL_IsScreenKeyboardShown` once
per frame and re-anchors on the edge. `surf_screen_keyboard(op)` /
`surfer.screen_keyboard([show])` is the toggle: -1/no-arg asks, 1/True
summons, 0/False dismisses, and the answer is always what is ACTUALLY
shown. Summon is Stop-then-Start deliberately — after a user dismissal
SDL still believes text input is active, and a plain Start is a no-op.
The weak default in input.c answers -1 (None from Python) wherever
there is no screen keyboard — the desktop, the P4 — which is the gate a
caller puts its keyboard-toggle chrome behind. Whole
multiples ONLY is the tempting rule — every surfer pixel then covers the
same count of screen pixels — but it means a window dragged to 1.8x
still draws at 1x inside bars, which nobody reads as "not a whole
multiple yet". They read it as the view having collapsed.

**The window itself is held to the framebuffer's aspect.** SDL2 has no
aspect constraint (SDL3 added one), so the backend puts the window back
on shape after a resize, keeping the axis that was dragged and deriving
the other — drag the bottom edge down and it gets wider to match. Two
things make it feel right rather than fight the mouse:

- it happens on a DELAY, once the resize events have gone quiet, because
  SDL's Cocoa driver reports every intermediate size of a live drag and
  resizing from inside that stream jitters;
- and not while a mouse button is down, or pausing mid-drag would yank
  the window out from under the pointer.

The delta is measured against the size the drag STARTED from. Against the
current size it reads as "nothing moved" — the events have already been
folded in — and the snap then undoes the drag instead of following it.
`SURF_FREE_ASPECT=1` turns the whole thing off; `SURF_VIEW_DEBUG=1`
prints drawable/fb/view on every resize.

## Sliders run either way

`surf_slider_new(parent, x, y, w, h, style)` gives a HORIZONTAL slider
when `w > h`. The shape is the orientation — a caller asking for 240x40
means a horizontal one and should not have to say so twice — and the
style carries `track_h`/`cap_h`, the same art transposed at generation
time, because a 9-patch slices along fixed axes and the upright groove
cannot be stretched sideways (the scrollbar taught this first).

The track keeps the ART'S OWN cross-axis size, centred, and stretches
only along its length — **both ways round**. Stretching across the groove
tiles it: the middle band of the 9-patch repeats, and a slider with two
parallel grooves is what that looks like. The vertical case used to slice
all four edges and so had the same defect standing up; it stayed hidden
only because the default upright art is baked at the mixer's exact size
and never reaches the 9-patch at all.

The widget also **clips its own group to the size it was asked for**,
which is what makes the whole declared box the grab area rather than the
union of whatever happens to be drawn (a group is hittable only with a
clip — see the colour picker). That is free for a full-size fader and
load-bearing for the compact one below, whose bar is a third of its
width: without it the gutter either side is a hole, and a tap on the
track is how you jump the value.

### ...and in two sizes

The same shape argument again, on the other axis: a cross-axis narrower
than the full fader cap (30px) gets the **compact** art — a thin 8px bar
with a 24x14 rounded-rectangle handle riding across it, wider than the
bar so the overhang is what you read the value off. `surfer.slider(x, y,
24, 200)` is one; `slider(x, y, 200, 24)` is one lying down. The binding
picks it; C callers pass whichever style they want, since the widget
itself knows nothing about either.

It is also the difference between working and not — `surf_slider_new`
refuses a slider narrower than its own cap, so before this a 24-wide one
was a `RuntimeError`.

**The compact handle is FLAT and fully opaque, which is the opposite of
what the full-size cap does**, and the reason is worth keeping. In A8,
alpha is the only variable there is: shading a body means making it
see-through, and what shows through a handle this small is the bar
directly under it — two vertical seams down the middle of the block,
which reads as a lozenge of glass rather than as a handle. The full cap
gets away with its grooves because they sit on 30px of an even 48px
moulding. Here opacity wins and the shape carries it.

24 x 14 is 3.6 x 2.1 mm on the P4's 169 dpi panel, under every fingertip
guideline there is. That is the trade a dense panel makes, and it is a
smaller trade than it looks, because of the clip above: the cap centres
on the finger and a tap anywhere in the box jumps to it, so what a finger
has to hit is the slider's declared WIDTH, never the handle.

## Colour picker

`surf_colorpicker_new(parent, x, y, size)` — a saturation/value square
beside a hue strip, reporting a packed `surf_color`. HSV rather than
three RGB sliders, because picking by eye means moving one axis at a
time, and because three sliders is something a caller can already build.

**The one widget whose art cannot be baked**: the square's colours depend
on which hue you are standing on. So it is drawn per pixel, in C, into
two runtime images — and the rule that keeps that legal is that it
happens on an EVENT and never in the frame path. The strip is drawn once
at creation; the square again only when the hue actually changes. After
that they are two ordinary opaque sprites.

Its group takes a size from `surf_group_set_clip`, which is how a group
becomes hittable at all — without it the gutter between square and strip
is a hole.

The fixed-point conversion uses **64-bit intermediates**, because at full
value and full saturation `v * (SURF_ONE - s)` is 65536 * 65536 and an
int32 wraps to zero. That corner is the most-used pixel on the widget:
it came out pure red instead of white.

## Password fields

`surf_textinput_set_mask(n, '*')` draws one character in place of every
other. The buffer is untouched — `surf_textinput_text()` still returns
what was typed, since this is a mask and not a cipher — and all THREE
walks over the text measure the mask: the caret's, the hit test's and the
paint's. Getting one of them wrong puts the caret somewhere the asterisks
are not. MicroPython: `ti.mask = "*"`, and None to show the text again.

## Scrollbar

`surf_scrollbar_new(parent, x, y, len, vertical, style)` is a thumb on a
track that knows **nothing about what it scrolls**. The caller owns the
content model — `set_range(total, visible, pos)` in whatever unit suits
it — and the widget only does ratios, reporting a new `pos` through
`on_change` when dragged. It hides itself while `total <= visible`, so a
caller can set the range unconditionally and the bar appears when there
is somewhere to go. Both pieces are 9-patched capsules, so the ends stay
round at any length and nothing is drawn at frame time.

Three consumers in tulip2, deliberately in three different units: the
console (rows of scrollback), the editor (lines of a document), and
gamma9001's sound chooser (pixels of scrollview offset).

Two things the widget got wrong until tulip2's `widgets` demo put a
horizontal one on screen next to a vertical one:

- **Horizontal needs its own art.** A 9-patch slices along fixed axes, so
  a bar laid on its side cannot reuse the upright capsule — stretching it
  sideways tiles the round *cap* and the thumb comes out as a string of
  beads. `thumb_h`/`track_h` in the style are the lying-down pair, and
  the insets move to the left/right edges. They are optional; without
  them a horizontal bar still works, it just looks wrong.
- **The MicroPython callback reported a Q16 fraction.** `pos` is in the
  caller's unit, but the binding fell through to the knob/slider branch
  and divided by SURF_ONE, so every `int(pos)` handler saw 0 — all three
  tulip2 bars snapped to the top when dragged instead of landing where
  the thumb was dropped. `.value` was always right, which is what hid it.

## Capture is per CONTACT

Three fingers on three faders is three independent drags. It was one:
`surf_g.capture` was a single node for the whole scene, so the first
finger down owned the machine and the other two were dropped. Reported
from a bench panel of sixteen faders as "I can only move one".

`surf_g.contacts[SURF_MAX_CONTACTS]` replaces it — five slots, keyed by
the controller's track id. **Everything that was one is now per finger**:
the captured node, the scrollview waiting to steal the gesture, and the
position the gesture started from. All three are answers to "what is THIS
finger doing", which is why none of them could stay global.

`surf_touch` carries the id, and it is LAST in the struct on purpose:
every positional `(surf_touch){x, y, phase}` keeps compiling and gets
contact 0, which is exactly what a mouse is. (They are all written out
explicitly now anyway — `-Wextra` warns on the short form, and a test
that names its contact reads better beside one that uses three.)

Three things this had to get right, and each is a way it can break:

- **A DOWN for an id already in flight REPLACES its slot** rather than
  opening a second. A controller that misses an UP — the GT911 does, when
  a finger lifts during an i2c hiccup — would otherwise leak slots until
  the table is full and every later finger is silently ignored.
- **A contact with nothing captured is still LIVE.** A finger that lands
  on empty space gets a slot with a NULL capture, so its MOVEs are
  discarded rather than being mistaken for a fresh press. Hence `used`
  rather than testing the capture pointer.
- **Both capture-cleanup paths loop.** Destroying a node and detaching
  one each used to clear the single capture; a destroyed node may be
  holding any of the five.

### ...and a widget follows ONE finger

The other half, in `src/widgets/widget_touch.h`. Per-contact capture
means two fingers on the SAME fader are two captures of the same node,
and without a guard the cap jumps between them on every event and the
value lands wherever the last one happened to be. So a widget claims the
first contact that presses it and ignores every other until that one
lifts — the second finger is dropped, not queued, which is what a
physical control does.

`busy` is the contact id **plus one**, so zero means idle and a calloc'd
widget starts right with no constructor to remember. Worth the small
ugliness: an `int8_t active = -1` would have needed a separate
initialisation in each of the five draggable widgets and would have been
silently wrong in whichever one got forgotten.

### ...and so does the SDL one, on a touchscreen

The desktop backend synthesised ONE contact from the mouse, which is
right for a mouse and wrong for the two places this code meets a real
touchscreen: a tablet running the SDL build, and — the case it was added
for — **a phone browser**. emscripten's SDL turns page touches into
`SDL_FINGER*` and synthesises a mouse from the PRIMARY finger only, so
before this a second finger on the web build simply did not exist. Three
fingers on three faders worked on the panel and not in a tab; every
layer above was already per-contact, and the hal was the half that never
fed it.

`S.fing[SURF_MAX_CONTACTS]` maps SDL's `SDL_FingerID` — an int64 that
counts up for ever — onto the five slots the core has, and the slot
index IS the contact id, so it must be stable for the life of a finger.
A DOWN for an id already in flight reuses its slot, the same rule (and
the same reason) as the core's.

Two things it must get right, and both are ways to make one finger into
two:

- **DIRECT devices only.** A mac trackpad is an SDL touch device as well
  (`INDIRECT_ABSOLUTE`), so without `SDL_GetTouchDeviceType` a palm
  resting on a laptop would inject contacts into whatever is on screen.
  On a laptop a trackpad is a mouse here, and a wheel, and nothing else.
- **The synthetic mouse is dropped.** SDL sends a mouse event for the
  primary finger too; taking both would make one finger two contacts,
  and the second would never lift cleanly. `which == SDL_TOUCH_MOUSEID`
  is the test, rather than turning the synthesis off — a real mouse has
  to keep working on the same build.

Coordinates arrive NORMALISED to the window, so they are multiplied back
into window points and go through the same letterbox mapping every click
does.

Verified in a browser by dispatching real `TouchEvent`s at the canvas:
three contacts reported at once, one moving while the others stand
still, the middle one lifting without disturbing the other two's ids,
and the table empty at the end.

### ...so the wheel is the desktop's second finger

The rule above has a consequence worth stating on its own: **a PINCH
CANNOT HAPPEN ON A LAPTOP.** Two fingers on a trackpad are an
INDIRECT_ABSOLUTE touch device we deliberately ignore, and what SDL
sends instead is a wheel. So anything offering pinch-to-zoom on the
panel needs a second way in on the desktop, and the wheel is the same
gesture with the same hand — the one the hal can actually deliver.

`surf_input_wheel` therefore **queues what no scrollview took**, drained
by `surf_wheel_poll` (`surfer.wheel()` in Python, `surfer._wheel` to
inject one). The scrollviews under the pointer still get first refusal,
which is touch's own bargain — a dialog's file list scrolls while the
same gesture over the app behind it reaches the app — and what is left
over is the app's to mean something else with: zoom a picture, step a
value, spin a knob. One ring, the key queue's shape, dropped on overflow
and reset with the session.

It also fixed a coordinate bug the queue would otherwise have exported.
The SDL wheel path took the pointer straight from `SDL_GetMouseState`
and hit-tested with it, skipping the letterbox mapping every click goes
through — so on any display where the drawable is not the window 1:1
(every retina Mac) it scrolled whatever sat at roughly double the
pointer's position. `map_pt` is that mapping, factored out of
`push_touch`, and both callers use it now.

`test_wheel_queue` in tests/test_scroll.c is the regression: a scrollable
list eats it, a list with nothing to scroll does not, bare screen queues
it, the queue drains once, and overflow drops rather than wrapping.

### The hal owes dispatch a per-contact stream

`hal_p4.c` used to synthesise ONE pointer from `s_pts[0]`, which was
wrong twice over: it threw four fingers away, and **`s_pts[0]` is not a
stable finger** — lift the first of two and the remaining one shuffles
down into slot 0, so the single pointer TELEPORTED across the screen
mid-gesture instead of reporting an UP and a MOVE. It now tracks up to
five contacts by track id and queues DOWN/MOVE/UP per finger, draining
one event per `poll_touch` call (the core already polls in a loop).

The release hysteresis stayed and is now per contact, for the reason it
was added: the GT911 blinks a contact out for a poll or two when a finger
rolls or lifts, and declaring UP on the first empty read synthesised a
phantom second tap — visible as a toggle button flipping twice.

**MicroPython still gets three arguments**, `fn(phase, x, y)`. Adding the
id would break every `lambda phase, x, y:` in every host, and there is no
portable way to ask a callable how many arguments it takes, so it would
have to be mandatory for everyone. The C widgets are where multitouch
pays; Python that genuinely wants per-finger data has `surfer.touches()`,
which reports every contact with its id. `surfer._touch(x, y, phase, id)`
takes an optional contact so a test can drive three fingers.

`test_multitouch` in tests/test_widgets.c is the regression: three
sliders, three contacts, each dragging its own; an UP on one leaving the
others captured; a MOVE for a contact that never went down doing nothing;
and a second finger on an already-held slider being ignored.

## The tinted widgets: knob, selector, slider cap

`.color` on a knob, a selector or a slider — and the LED, which got there
first and taught the trick. The art is **A8**, so one asset is every
colour on the panel: each widget keeps its own COPY of the `surf_image`
struct (pixels shared, its own `tint`), and `set_color` is
`surf_node_damage` — a repaint, no pixels. On the P4 the tint is a
palette register the PPA applies during the blend it was doing anyway,
so a coloured panel costs exactly what a grey one did.

It also made the assets 4x smaller: `widget_assets.h` went 6.2 MB to
2.7 MB, and the device image 7.1 MB to 5.4 MB — the knob strip alone was
1 MiB of ARGB and is 256 KiB now.

**What A8 costs is SHADING, not speed, and the art has to be drawn for
it.** Alpha is coverage, not lightness: a colour image's dark rim becomes
*see-through* rather than dark, so this art reads on a DARK panel and
would look hollow on a light one. `ink()` in the generator does the
conversion (Rec.601 luma × coverage) and is the one place that decision
lives.

Which way round the tones go is the thing to get right, and the fader cap
had it backwards first: the **body** is the ink — near-opaque, so the cap
is a solid coloured block — and the grooves are where alpha drops away
and the panel shows through, which is what a groove looks like. Making
the ridges the ink gave a ghost of a cap with bright stripes floating in
it. The body also stops short of full: with one tint nothing can be
*brighter* than the tint, so the index line only reads if the body leaves
it headroom.

## The knob strips are BAKED AT FIRST USE, not shipped

`src/widgets/art.c`. The three 64-frame A8 filmstrips — the 64 px knob,
the 40 px knob and the 56 px selector — were **565 KB of flash
.rodata**, a tenth of a device image that had 192 bytes of partition
left, and on the P4 they were being memcpy'd into PSRAM at init anyway
(`surfer_port_prepare_image`: the PPA cannot DMA from memory-mapped
flash). So they are rendered into that PSRAM instead, the first time a
widget of that size is made — `surf_art_knob_strip(size)` /
`surf_art_selector_strip(size)` — and shared by every widget after it.
The FRAME PATH is untouched: a knob still picks a pre-rendered frame, and
the blend is the same A8 op over the same pixels. `widget_assets.h` went
2.7 MB to 310 KB.

**The faces are `tools/gen_widget_assets.py`'s, ported line for line** —
`knob_strip()` and `selector_strip()` stay in that file as the reference,
and `--ref` emits them as C arrays for the TEST build alone, where
`test_art_strips` holds the port to them byte for byte (zero differing
pixels of 565,248 on the mac; the test allows a unit of slack per pixel
because the C is `float` where the Python is double, and the P4 has no
hardware double). Change the look in the generator first, then mirror
it; the test says when the two have drifted.

**The body is computed once, the pointer 64 times, and only where it
can land.** Everything in a face but the pointer (the knob) or the wedge
(the selector) is the same in every frame, so its ink is one pass and
every frame starts as a copy of it; the sweep re-rasterises the pixels
inside the pointer's bounding box, or tests the wedge's candidate pixels
with a rotation and two compares. Same bytes out, and it is what made
the bake affordable on the glass — measured on the P4X, first widget of
each size, PANEL=normal:

| | full face x64 | body once + patch |
|---|---|---|
| knob 64 px | 155 ms | **30 ms** |
| knob 40 px | 59 ms | **13 ms** |
| selector 56 px | 145 ms | **21 ms** |

So the first knob of a session costs its app's `build()` 30 ms, once,
and every knob after it 0.1 ms; all three strips together are 64 ms.
Lazy rather than in `surf_init` because a game never makes one, and
never freed because every widget of that size points at the pixels — and
a soft reset frees the node pool, not images, so the second session pays
nothing. The image went 7,339,840 -> 6,778,400 bytes.

What was NOT drawn procedurally per value change, and why: the same
rasteriser through the shape API costs **2.6 ms per knob per change on
the P4X** (tulip2 measured it) against a frame-index write today, which
is one dragged knob's worth of frame and ten MIDI-driven knobs' worth of
dropped frame. Baking keeps the strip and drops the flash; that was the
whole trade.

## Writing a RUN of cells

`grid.set_cells(col, row, s, fg, bg)` writes a whole same-coloured run in
one call. `set_cell` is per character, so a program painting a screen of
text pays a MicroPython call per cell plus the interpreter loop driving
it — measured 2244 cells at 19 ms on a P4X, **4.5 ms** batched. Same
clipping and the same per-cell early-out as `set_cell`, so it damages
exactly what changed; it is that loop, moved down.

**It only pays for a caller that keeps no shadow of its own.** tulip2
has both cases and they came out opposite ways: its console writes and
forgets, so batching is a straight 4x; its VT terminal keeps a per-cell
Python shadow it must update either way, and batching there measured
*slower* at every run length, because recording a span by slice costs
three list allocations whose churn outweighs the C loop. Worth knowing
before reaching for it: the win is the loop, not the call.

## Textgrid scrollback

`surf_textgrid_set_scrollback(n, mult)` keeps `mult` screens of rows so
lines that scroll off the top stay reachable: drag the grid to look back,
a thin macOS-style bar appears on the right while there is history, and
any write snaps the view to the bottom the way a terminal does.
`surf_textgrid_view/set_view/history` drive it programmatically, and the
visible bar is a separate `surf_scrollbar` the caller places and keeps in
step — the grid draws no chrome of its own. Dragging the TEXT still
scrolls (a touchscreen wants that), so a caller that shows a bar should
poll `surf_textgrid_view` to follow it.

The cells become a **ring** of `total_rows`, with `head` the ring row at
screen row 0 and `view` how far back the display is. Scrolling then moves
the window instead of the contents — O(exposed rows), not O(screen), and
the rows leaving the top become the history rather than being discarded.
Without scrollback `total_rows == rows`, head/view stay 0, and every path
reduces to the old arithmetic, so a plain grid is untouched.

Opt-in because it costs `cols*rows*mult*sizeof(surf_textcell)` — a 128x50
console at 10x is ~500 KB. That is a plain `calloc`, which on a PSRAM board reaches external RAM
(IDF's SPIRAM_USE choice defaults to SPIRAM_USE_MALLOC, and allocations
over SPIRAM_MALLOC_ALWAYSINTERNAL — 16 KB by default — prefer it). It
returns false rather than trapping if the heap cannot serve it, so a
caller can fall back (tulip2 tries 10, 4, 2 screens).
Enabling it installs the grid's own touch handler, so a node with
scrollback must not also have `on_touch` set.

## Fonts

surfer ships **45 baked fonts from 31 source files**, all reachable at
runtime by name via `surf_font_builtin("helvR12")`. `tools/fontbake.c`
has three front ends: stb_truetype for outlines, FreeType for *hinted*
outlines, and a BDF reader that copies designed bitmap fonts
pixel-for-pixel (SIZE is ignored — a BDF *is* one size).

**fontbake's SIZE is ppem.** It used to mean ascent−descent, which made
every name in the build a lie by ~32%: `ui12` was a 9.1 ppem bake with a
5-pixel x-height — about 6pt on a 110dpi screen, and the actual reason
small text looked fuzzy. `FONTBAKE_LINE=1` restores the old meaning;
`FONTBAKE_EM=1` is now a no-op kept so old command lines still run.

Two numbers in the summary line, both measured before gamma/threshold:
**gray %** (partial coverage — 0.0% means a genuine bitmap) and
**solid %** (≥7/8 coverage — actual ink). On an outline face watch
*solid*: hinting barely moves gray, because the stems go solid while
their AA sidebands stay partial. Unhinted Roboto at 9 ppem is solid
0.0% — not one pixel of real ink in the atlas.

`FONTBAKE_HINT=full` grid-fits through FreeType's autohinter, which is
the single biggest lever on small text and the thing stb_truetype cannot
do (it interprets no hints and has no autohinter). Roboto's `l` at 15
ppem goes from `220 128` — a 1.4px gray smear that never reaches ink —
to `68 255 24`, a solid column. `=light` grid-fits vertically only, so it
does *not* help here: the problem at UI sizes is horizontal. `=bytecode`
runs the font's own hints instead. **FreeType is a host build dependency
of fontbake only** — nothing links it at runtime, the device still blits
the same A8 atlas. Both the Makefile and `ports/esp32p4/main/CMakeLists.txt`
detect it with pkg-config and fall back to unhinted with a warning; keep
the two in step or the panel gets fuzzier text than the SDL preview did.
Other knobs: `FONTBAKE_GAMMA`, `FONTBAKE_THRESHOLD[_CUT]`.

**Two ranges, and which one a face gets is decided by its SHAPE.**
`SURF_RANGE_BASE` (proportional) is ASCII + the Latin-1 supplement +
dashes + ellipsis, 194 codepoints. `SURF_RANGE_MONO` (fixed width) is
ASCII + **CP437** + ellipsis + `SURF_RANGE_TERM`, ~280 — box drawing,
the block/shade run, arrows, the card suits, the 55 Latin-1 characters
CP437 happens to carry, and the modern-TUI set (rounded box corners,
typographic dashes/quotes, check/cross, chevrons, the spinner
asterisks) that an ssh session running any current terminal program
lands constantly — added when Claude Code over tulip2's ssh came out as
rows of `?`. A source face missing some of the TERM set just skips them
with a warning (DejaVu Sans Mono lacks U+23BF and U+23FA); tulip2's
vt.py substitutes lookalikes for what a face cannot draw. Deliberately
not the union with Latin-1: a terminal face has no use for the 41
Latin-1 characters CP437 never had, and a proportional face has none
for box drawing. All are `#define`s in `tools/fontbake.c` so the
Makefile and `ports/esp32p4/main/CMakeLists.txt` cannot drift; a build
file asks by name (`fontbake NAME PPEM src.ttf out.h mono`).

**A fully-solid atlas is stored ONE BIT PER PIXEL** (`SURF_FMT_A1`),
decided by MEASURING the bake rather than by a flag, so a face cannot be
marked 1-bit and then smeared by a wrong ppem. `surf_image_expand_a1()`
unpacks it to A8 the first time the registry hands the font out; nothing
below that ever sees A1 (the PPA has no A1 blend and the hal no
bytes-per-pixel for it). On the device this is FREE — the port already
copies every atlas out of memory-mapped flash into PSRAM, so the unpack
replaces a memcpy into an allocation that already existed. It took the
45 atlases from 3.17 MB to 0.98 MB. **It must happen after
`surfer.init()`**, which is where the allocator appears: expanding before
it left atlases packed, and a packed atlas blitted as A8 draws its own
bits as alpha — text as coloured noise. `mod_init` calls `surf_init`
before `prepare_assets` for exactly this reason.

Sources: Roboto (ui12/16/16b/23/28/36/48 — the 36 and 48 are display
sizes, plain AA, where partial coverage reads as a smooth curve rather
than the lumpiness thresholding an off-grid outline gives at small
sizes) + DejaVu Sans Mono (mono16, the one AA fixed-width face and the
house default), BigBlue Terminal (bigblue12), **ten oldschool PC ROM
faces** (VileR's pack, CC BY-SA 4.0 — see assets/fonts/LICENSE.txt, and
note it is the only copyleft asset here), 4 Kenney pixel faces (CC0),
and 18 Adobe X11 BDFs — helvR/helvB/ncenR at 08/10/12/14/18/24, each a
separately *designed* size.

**The oldschool faces bake at ppem = unitsPerEm/100 and are NEVER
hinted.** That number is not the cell height — an 8x14 face has em 1600
and wants ppem 16, while an 8x8 one has em 800 and wants 8; get it wrong
and the bake is 39-80% gray instead of 0.0%. Use the `Px` (pixel
outline) variants: `Ac` is aspect-corrected for 4:3 CRTs and measures
57% gray on a square-pixel bake, and `Mx` carries embedded bitmap
strikes our bake ignores and the autohinter then destroys (97% gray).
`PxPlus` covers all of CP437 and Latin-1; `Px437` covers CP437 only, and
fontbake skips what a face has not got.
`assets/fonts/LICENSE.txt` has the terms; BigBlue's provenance is still
unpinned (TODO before shipping) and the oldschool pack is share-alike.
The UI ramp is hinted; `ui16b` (the one surviving *specimen* bake)
deliberately is not — it exists to show what thresholding does to a raw
outline. The pixel faces and the oldschool ROM faces never are: the
grid-fit they want is the one they were drawn on.

**ui16 and ui23 are the same physical size on different screens**, which
is why the ramp carries both rather than scaling one. The desktop window
puts a framebuffer pixel on a 110-140dpi point (it varies with the
display-scaling setting); the P4's 7" 1024x600 panel
is 169dpi. So ui16/mono16 are the desktop body sizes (~10pt) and
ui23 the panel's; there is one AA fixed-width face (mono16) and the
rest of the fixed-width set is pixel faces, which have exactly one size
each by construction.

**One TU owns every atlas.** `tools/gen_font_registry.py` emits
`font_registry.c`, which includes all the font headers and implements
`surf_font_builtin*`. Font headers declare `static const` atlases, so
including one anywhere else silently duplicates its pixels into that
object file — don't. Device backends call
`surf_font_builtin_prepare(fn)` once at startup to re-home every atlas
into DMA-able RAM. Cost: 1.25 MiB of atlas, P4 image 2.70 MiB of the
8 MiB partition (66% free), plus the same again in PSRAM.

The binding's two unnamed defaults are `DEFAULT_FONT` (what
`surfer.label` uses with no font argument) and `WIDGET_FONT` (button
labels and dropdown items). **Both are `ui12`**, so chrome matches the
text beside it. WIDGET_FONT used to be `helvR08`, a drawn bitmap, on the
argument that a thresholded outline smears at the size chrome renders at
— which was true right up until fontbake started sizing in ppem and
hinting through FreeType. Both resolve by NAME through `font_named()`;
never `surf_font_builtin_at(0)`, since index 0 is only whatever comes
first in the Makefile list and reordering it would silently restyle every
widget.

`surfer.widget_font(name_or_font)` overrides the chrome face and returns
what is in force **by name** (call it with no argument to just ask; it
answers `None` after a `Font` object, which has no name to report). It
applies to widgets built AFTER the call — a button bakes its label node
at construction — so a host sets it once, early, rather than expecting
the screen to change under it. tulip2 does exactly that, from its house
style in `ui.py`.

Early, but **after `surfer.init()`** — see the root-pointer rule below.
Setting it at import time is what killed the P4X on every soft reset.

MicroPython takes a font as a name, a `Font` object, or a legacy index
anywhere (`FONT_UI16`/`FONT_UI28`/`FONT_MONO16` ARE the names: as
indices they had drifted to ui12/ui16/ui16b as the registry grew, the
last a proportional face textgrid refuses): `surfer.label(s, x, y, c, "helvR12")`,
`surfer.textgrid(cols, rows, fg, bg, "toshiba9x16")`, `surfer.font(name_or_blob)`,
`surfer.fonts([mono_only])`. `surf_font_is_mono` gates the textgrid — it
sizes its cell from 'M', so a proportional face is refused.

`build/surfer_fonts` (desktop) and `DEMO_MODE = DEMO_FONTS` in
`ports/esp32p4/main/app_main.c` render the same 3-page specimen from the
same source (`demos/fonts_scene.c`); tap/click cycles pages. `SURF_TAP=x,y`
injects a synthetic tap so the page flip is testable headlessly.

## A label baked into pixels — text that scales

`surfer.text_image(str, color, font, wrap_w)` (`surf_text_bake` in C)
renders a string into a freshly allocated ARGB image: glyphs, colour,
wrap and the fallback face exactly as a label draws them. It exists
for the one thing a label cannot do — SCALE. Text nodes have no
transform, deliberately: a per-glyph blit every frame cannot ride the
sprite path, and big text drawn per frame would be the per-pixel
frame-path loop the first rule forbids. So chunky text is a BAKE: the
image on a sprite with `.scale`, the compositor (the PPA's SRM on the
device) doing the enlarging — "bake at final size" pointed the other
way, priced once at the call.

The implementation is the label's own layout walk aimed at
`surf_image_blit` instead of the framebuffer, which already composites
every atlas format this can meet: A8 with the tint carrying the
colour, ARGB for an emoji that keeps its own. A fresh ARGB image
starts transparent, so blitting IS rendering. The Image is the
caller's to destroy — unlike a label it holds no reference to a
runtime Font, because the pixels are copied out at the call.
`tests/test_text.c` bakes against the synthetic font, where the
expected alpha and colour are exact numbers.

## Emoji are a FALLBACK FACE, not a second way to draw

`surfer.emoji("fire")` is the name lookup; `"\U0001F525"` is the same
glyph and always works. Both end up in the same place: a codepoint the
text face has not got, found in an emoji set instead of becoming `'?'`.

**The whole feature is one pointer.** `surf_font.fallback` is tried
between "this face has it" and "draw a question mark", and
`surf_font_glyph_in()` reports WHICH face answered — without that a
caller cannot know which atlas to read, and an emoji's rect blitted out
of the text atlas draws a convincing piece of a letter. That is the only
new concept; everything else is arithmetic.

- **One level, never a chain.** A chain is a lookup whose cost depends
  on how many faces are loaded, and a set pointing at a set is how a
  lookup stops terminating. The registry wires it and nothing else may.
- **The face wins over the set.** They overlap — ✔ U+2714 and ★ U+2B50
  are in territory some faces draw as line art — and a font that
  genuinely carries a codepoint must never be overridden.
- **`'?'` is still last**, so a miss in BOTH is visible rather than
  nothing.

**The atlas is ARGB8888, and that is the one thing that is different.**
Every other face here is A8 — one coverage byte, tinted to whatever
colour the caller asked for — because a letter is a SHAPE and its colour
belongs to the caller. An emoji is a PICTURE: the red of the heart and
the green of the check mark are the content, and at the sizes this
matters they are what make it legible after the shape has stopped being.
So the caller's colour is ignored for these, `surf_font_is_color()` is
the question, and both paint paths ask it.

That costs 4 bytes a pixel against A8's 1 and A1's 1/8. The A1 saving
does not apply and cannot: it is measured on a mask. Hence a CURATED set
(`assets/emoji/set.txt`, 403 entries, which says why) at **two sizes —
12 and 16, for 284 + 474 KB**, against 1.25 MiB for all 45 text faces
put together. The registry gives each face the largest set that does not
overflow its line box.

**There was a 24 and it is the right size for the display ramp**, since
the faces here run from a 12px line to a 65px one. It came out on flash
pressure, not on taste: tulip2's P4X app partition is 7 MiB and the
image reached 99% of it — 77 KB spare is one font away from a build that
does not link, and that board's 16 MiB is fully allocated, so growing
the partition reformats it. Dropping the 24 bought 998 KB back and every
face above ui16 now wears a 16px emoji, which beside 28px text reads as
a slightly small picture rather than as a bug. Both levers are one edit
— a size in `EMOJI_NAMES`, or lines out of set.txt.

**AN EMOJI OWNS TWO CELLS IN A TEXTGRID**, and that is arithmetic rather
than convention: an emoji is square, a mono cell is not, so "as tall as
the line" and "one cell wide" cannot both hold. mono16 is 10x19 and two
of its cells are 20x19, which is as square as a cell grid gets — which
is also why every terminal settled on East-Asian-Wide. Three things fall
out, and each fails silently on its own:

- **Damage has to extend one cell RIGHT.** A wide glyph is painted by
  its left cell across two, so damaging only that cell draws half an
  emoji and leaves the other half for whenever something unrelated
  damages the neighbour. `cell_is_wide()` is asked at damage time for
  exactly this. Found by the test, not by looking.
- **Paint has to start one cell EARLY**, for the mirror reason: a damage
  rect beginning on the right half would find a cell owned by somebody
  off-screen and paint nothing there.
- **A fallback glyph is CENTRED in the cell box, not baselined.** It was
  baked against its own baseline, and borrowing this face's puts a 16px
  picture 1px above a 19px cell and clips its top row. A LABEL centres
  in the LINE box for the same reason (`glyph_top()` in font.c). It
  used to baseline there — "the emoji sits in flowing text" — and that
  reasoning was wrong by arithmetic: the wire attaches the largest set
  that fits the LINE, every ascent is smaller than its line height, so
  a baselined emoji pokes `size − ascent` above the line top and is
  clipped wherever the label is clipped at all. ui12 (ascent 13,
  emoji16) lost the top 3px of every emoji in a widget legend, reported
  as the world app's chat tab "cut off at the top".

**Baked by its own tool.** `tools/emojibake.c` shares none of fontbake's
rasterizers, hinting, kerning or A1 packing; what it shares is the
emitted shape, so an emoji set IS an ordinary `surf_font` and the text
path needed no idea it was special. Sources are Twemoji's shipped 72x72
PNGs through the already-vendored stb_image — no colour-font rasteriser,
because every colour emoji font ships one bitmap strike and hands back a
downscale of it, which is what this does minus a 10 MB dependency.

**The downscale is box-filtered in PREMULTIPLIED alpha.** Averaging
straight RGBA weights the colour of fully transparent pixels — black, in
these PNGs — into every edge, and the set comes out with a dark fringe
that reads as dirty at small sizes.

Two measured facts behind the shape, both of which inverted an
assumption:

- **Colour beats monochrome at small sizes.** The obvious cheap answer
  is a 1-bit emoji face, which needs no new format at all. But
  monochrome emoji use hatching to stand in for colour, and at 8-12px
  that hatching is noise — the mono set read WORSE than a downscaled
  colour one. Below about 12px colour is the only thing still carrying
  meaning, because the shape has stopped.
- **One master downscales to every size.** Going direct from the 72px
  source and going via a 32px intermediate are indistinguishable at 8,
  10, 12, 16 and 22 — so the per-size bakes cost build time and nothing
  in quality, and a size nobody baked could be derived at load if that
  ever became worth it.

**Twemoji is CC-BY 4.0** (`assets/emoji/LICENSE.txt`) and needs a visible
attribution — the second share-alike-ish asset here after VileR's
oldschool pack. GNU Unifont was the alternative and is genuinely better
at exactly 16px, its designed size, where the strokes are on the pixel
grid; it lost because it only works at 16 (a 2:1 downscale shreds 1px
outlines) and because a hatched circle cannot say "orange".

`build/surfer_emoji` is the specimen — labels at three sizes and a
textgrid, so both paint paths are on one screen. `make test` covers the
lookup order, that a label blits the emoji out of the SET's atlas
untinted, that a wide glyph reaches pixels in the second cell, and that
it survives a partial repaint of only that second cell.

## A root pointer dies with the VM. A C static does not.

`MP_REGISTER_ROOT_POINTER` fields live in `mp_state_ctx`, and **`mp_init()`
does not clear them**. It clears its OWN — `vfs_cur`, `dupterm_objs`,
`persistent_code_root_pointers`, each one spelled out by hand in
`py/runtime.c` — and knows nothing about a usermod's. So after a soft
reset every root the binding registered still points into a heap
`gc_init()` has just handed back, and a stale pointer is not
`MP_OBJ_NULL`: the usual `if (x == MP_OBJ_NULL) x = new_list()` guard
does not fire, and the append writes through it. On the P4 that is a
store access fault at boot, i.e. a board that never comes back
(measured 5/5 on the P4X, and as old as the registry — a bisect
exonerated the wrapper rework it was first blamed on).

There is **no per-session hook** to fix this with. A built-in module's
`__init__` looks like one and is not: a module with a const globals dict
is never stored in `sys.modules`, so `mp_module_get_builtin` re-resolves
it and calls `__init__` on EVERY import, not once per VM. Measured — set
the chrome font, `import surfer` again, and the hook has already reset
it. Nor can C detect a new session on its own: statics and root pointers
both survive a soft reset with identical values, and `gc_init()` does not
zero the heap, so a canary's bytes are typically still sitting there
intact and compare *equal* in exactly the case worth catching.

So the rule is structural, not defensive:

- **`surfer.init()` is the session boundary**, and it is the only place a
  root is dropped and rebuilt. Anything a host may call BEFORE it must
  not write through one.
- **The node registry is the only root**, it is only reachable with a
  live scene, and `mod_init` nulls it on re-entry. `surfer_pins`, an
  append-only list for objects with no node to hang off, was the one root
  a pre-init call could reach — through `widget_font()` — and it is gone.
- Deleting it cost nothing, which is the part worth remembering: a `Font`
  a node draws from is already anchored by that node's own `img_ref`, and
  `surfer_font_type` has **no finaliser** (`surf_font_free` runs only
  from an explicit `.destroy()`), so collecting an unused wrapper leaves
  the C font allocated rather than leaving a pointer dangling. The pin
  was buying a leak it already had.
- **A C static must not hold an `mp_obj_t`.** It outlives the VM that
  made the object. `widget_font_spec` did, so `widget_font()` could hand
  a caller an object the GC freed a session ago; it is a `char[]` name
  now, which is also why the getter reports a name rather than whatever
  you passed in.

## Looking at the sdl window

The scene, framebuffer and `SURF_SHOT` dumps are always 1024x600. Only
how big that lands on screen changes, and it is always an **exact whole
multiple** — the window is resizable and the view snaps to the largest
integer zoom that fits, centred, letterboxed. Nothing is ever resampled;
a smoothed upscale would invent edge pixels and make every bake look
antialiased, which for this backend is the one unforgivable bug.

- **default**: a 1024x600pt window, one framebuffer pixel per point. This
  is the baseline and stays put — the two knobs below are for looking
  closer, not for moving it. Drag the window instead if you just want it
  bigger; the view re-snaps to the next whole multiple that fits.
- **`SURF_SCALE=N`** asks for an N× window in points, clamped to the
  usable desktop — a zoom the display cannot hold is worse than no zoom,
  since the window runs off screen and takes the part you wanted with it.
  The clamp bites early: 2× of 1024x600 is 2048x1200pt, more than a
  laptop desktop has, so `SURF_SCALE=2` gets you the biggest exact zoom
  there is room for instead (1.5× on a 1710x1107pt desktop — a 3× view in
  real pixels on a 2× display).
- **`SURF_NATIVE=1`** goes the other way: one framebuffer pixel per
  **physical** display pixel, which on a 2× laptop is ~220dpi. It is an
  absolute density, not a multiplier, so it *overrides* SURF_SCALE.

Why SURF_NATIVE exists: the P4's 7" 1024x600 panel is **169dpi**, and a
surfer pixel drawn one-per-point on a laptop is 110-140dpi depending on
the display-scaling setting — i.e. always coarser than the device, often
by 1.5×. Every jaggy and AA fringe in the window is therefore bigger than
anything the panel will ever show, which is most of why bitmap faces look
worse here than on hardware. SURF_NATIVE lands *denser* than the panel
rather than coarser, so it errs the other way; the truth is between the
two and neither is reachable exactly.
