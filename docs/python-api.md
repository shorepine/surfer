# surfer Python API

The MicroPython binding to surfer. Hand-written, deliberately small, and
**early — names may still change.** This documents what exists today on
the unix port; the esp32p4 port will expose the same API.

## Running

```sh
make mpy      # builds MicroPython (MPY_DIR ?= ~/micropython) with the surfer module
~/micropython/ports/unix/build-standard/micropython bindings/surfer/tulip.py
```

`tulip.py` boots "tulip mode": an on-screen REPL with `surfer` and
`screen` already in scope. Everything below can be typed at it live.

## The module

```python
import surfer

surfer.init(w=1024, h=600)   # open the display; call once, first
surfer.tick()                # pump input, run animations, compose, present
                             # → False when the window wants to close (Esc)
surfer.keys()                # drain pending key events → [(kind, text, shift), ...]
surfer.keys_held()           # keys DOWN right now → ((kind, text), ...) — for games
surfer.screen()              # the root Node
surfer.rgb(r, g, b)          # 0-255 each → packed RGB565 color int
surfer.screenshot(path)      # dump the framebuffer as binary PPM → bool
                             # (desktop/web; on the P4 use fb_read + Python IO)
surfer.fb_read(x, y, w, h)   # framebuffer region → RGB888 bytes, every port
surfer._touch(x, y, phase)   # inject a synthetic touch (tests / on-screen keyboards)
```

The app owns the loop:

```python
while surfer.tick():
    for kind, text, shift in surfer.keys():
        ...
```

Widget callbacks fire from inside `surfer.tick()`, on the same thread —
no locks, no marshaling.

**Except on the web** (`sys.platform == "webassembly"`), where a
blocking loop would freeze the tab: the browser drives one frame per
`requestAnimationFrame`, and an app registers its per-frame work as a
hook instead of looping:

```python
import sys, tulip
if sys.platform == "webassembly":
    tulip.app_frame = my_step      # called once per frame after the REPL;
else:                              # return False to unhook
    while surfer.tick():
        if my_step() is False:
            break
```

See [examples/space.py](../bindings/surfer/examples/space.py) and
[examples/gamma9001.py](../bindings/surfer/examples/gamma9001.py) for
the full pattern.

## Nodes

Nodes are the scene graph: cheap, pooled, retained. All factory
arguments are positional (the `name=` forms below just show defaults):

| factory | notes |
|---|---|
| `surfer.group(x, y)` | container; children render at its offset |
| `surfer.rect(x, y, w, h, color=grey)` | solid fill |
| `surfer.label(text, x, y, color=white, font=FONT_UI16)` | proportional text |
| `surfer.textgrid(cols, rows, fg=..., bg=..., font=FONT_MONO16)` | fast fixed-width text (terminals, editors) |
| `surfer.scrollview(x, y, w, h)` | clipped viewport; drag/flick scrolls its children |
| `surfer.sprite(image, x, y)` | a runtime image on screen — see [Images & sprites](#images--sprites) |

Every node has:

```python
n.x_pos, n.y_pos     # position in the parent (read/write; writes damage + repaint)
n.w, n.h             # size (read-only)
n.hidden = True      # hide/show the subtree (write)
n.add(child)         # parent a node or widget under this node
n.detach()           # remove from the tree, keep all state — the app-switcher primitive
n.destroy()          # detach + free the subtree
```

Type-specific methods (error on the wrong node type):

```python
label.set_text("new text")
label.set_wrap(120)                     # wrap box width (0 = single line)
label.set_align(surfer.ALIGN_CENTER)    # ALIGN_LEFT / CENTER / RIGHT
rect.set_color(surfer.rgb(230, 150, 60))

grid.set_row(row, "text")               # fill a row, space-padded, default colors
grid.set_cell(col, row, "A", fg, bg)    # one cell; char or codepoint int
grid.grid_scroll(rows)                  # +n scrolls content up, blanks exposed rows

sv.scroll_to(x, y)                      # programmatic scroll (clamped)
```

`detach()` + `screen().add(...)` round-trips losslessly — an app's whole
UI is one group that a switcher detaches and reattaches.

**Custom widgets** hang off any node's touch handler:

```python
pad = surfer.rect(x, y, 34, 44, color)
pad.on_touch = lambda phase, tx, ty: ...   # TOUCH_DOWN / MOVE / UP, screen coords
```

The node captures the gesture DOWN→UP like any widget; an enclosing
scrollview can still steal it after 8 px of travel (act on UP near the
DOWN point for tap semantics — see the step pads in
[examples/gamma9001.py](../bindings/surfer/examples/gamma9001.py)).

## Images & sprites

Load a PNG at runtime (any size, alpha channel respected) and put it on
screen as a sprite:

```python
img = surfer.image(open("ship.png", "rb").read())   # bytes in → Image
img.w, img.h          # decoded size (read-only)
img.destroy()         # free the pixels — only after every sprite using it is gone

# Amiga-style color cycling — a one-entry hardware palette. a8=True keeps
# only the PNG's alpha channel; the mask draws in .tint (rgb565), which
# the P4's PPA applies at blend time. Retint + damage() per frame costs
# the same as drawing the sprite once — no pixels are recomputed.
# `import pulse` is the demo: 22 masks cycling at a locked 62 fps.
mask = surfer.image(png_bytes, True)
mask.tint = surfer.rgb(255, 40, 200)
spr = surfer.sprite(mask, x, y)
spr.damage()          # content changed in place: force a repaint

s = surfer.sprite(img, x, y)
screen.add(s)
s.x_pos = 300         # move it: the compositor repaints whatever it uncovers —
                      # nothing under the sprite needs manual redrawing
s.scale = 1.5         # uniform scale, 1/16 .. 16 (float; the PPA SRM range)
s.rot = 90            # rotation in degrees CCW — multiples of 90 only
                      # (the ESP32-P4's SRM engine rotates in quarter turns)
s.mirror_x = True     # flip horizontally (walk left with right-facing art);
s.mirror_y = True     # flips apply to the source before rotation
s.w, s.h              # the transformed on-screen footprint

s.set_src(x, y, w, h) # the camera primitive: show a window of a big image
s.fast_scroll(True)   # ...and pan it as one DMA band shift per frame —
                      # bake a whole game world with image_new/blit, then
                      # pan a screen-sized src over it (call set_src every
                      # frame; overlay sprites must be later siblings).
                      # Measured on the P4: forest walking went 20 -> 240
                      # fps compute rate this way.
```

`img.blit(src, x, y, rot=0)` also takes quarter-turn rotations, so
rotated props (a fallen tree is a standing one at rot 90) bake into the
world at load time and the frame path stays untransformed.

Taking bytes (not a path) is deliberate: the same code works from a
unix file, the P4's flash VFS, or assets frozen into a web build (see
`tools/pngwrap.py`, which bakes PNGs into an importable module).

A PNG with no transparent pixels draws on the fast opaque path
automatically. At `scale == 1.0, rot == 0` a sprite composites exactly
like any other node; transformed sprites go through the hal's
scale/rotate op (PPA SRM + blend on device, ~200–400 µs per sprite per
damaged frame — dozens per frame are fine).

Sprites keep a reference to their `Image`, so the GC won't collect
pixels that are still on screen. Sprite-sheet sub-rect animation
isn’t exposed yet.

The full demo is [examples/space.py](../bindings/surfer/examples/space.py)
(`import space` from tulip mode): draggable ship, tumbling meteors at
mixed scales, autofiring lasers — Kenney CC0 art from
[assets/kenney/](../assets/kenney/).

## Layers (scrolling backgrounds & tile maps)

For parallax backgrounds and tile maps, bake your tiles into ONE wide
strip per layer at load time, then scroll the strip as a `layer` node —
the frame path pays one blit per layer instead of one per tile:

```python
strip = surfer.image_new(2048, 128)          # opaque; alpha=True for ARGB
strip.fill(surfer.rgb(92, 148, 218))         # also: fill(c, x, y, w, h)
### Shapes — draw the asset, then sprite it

All shape calls rasterize INTO an image at load time (anti-aliased,
never per frame). Paints are an rgb565 int, `(color, alpha)`, or a
linear gradient `((x0, y0, c0[, a0]), (x1, y1, c1[, a1]))` between two
stops. `surfer.image_new(w, h, surfer.A8)` makes a tintable mask —
shapes drawn there recolor by `.tint` (see color cycling above).

```python
g = surfer.image_new(512, 300)                       # opaque 565 canvas
g.poly([(0,300), (256,20), (512,300)],               # gradient triangle
       ((0,300, surfer.rgb(78,54,38)), (256,20, surfer.rgb(172,124,74))))
g.line(10, 10, 500, 40, color, 3)                    # width 3, round caps
g.lines([(0,0), (40,80), (90,20)], color, 5)         # polyline, round joins
g.circle(60, 60, 25, color)                          # filled
g.circle(60, 60, 25, color, 4)                       # 4px outline
g.ellipse(cx, cy, rx, ry, color[, width])
g.bezier([(0,90), (50,0), (100,90)], color, 4)       # 3 pts quadratic
g.bezier([p0, c0, c1, p1], color, 4)                 # 4 pts cubic
```

Order matters when overlays move over a fast-scrolling layer: call the
layer's `set_offset` FIRST, then write the overlay positions. The layer
heals the smear under each overlay where it sits at `set_offset` time —
move-then-shift leaves a trail of ghost slivers behind fast movers.

`import parallax` shows the combination: every mountain is gradient
polys (no art), and the volcanoes' lava veins are bezier strokes in A8
masks with cycling tints. Keep tint-cycled overlay masks CROPPED to
their ink when they ride a fast-scrolling layer — a moving overlay
re-blends its whole bbox every frame (DESIGN.md §5).

tile = surfer.image(open("grass.png", "rb").read())
for x in range(0, 2048, 64):
    strip.blit(tile, x, 0)                   # load-time composition (CPU)
tile.destroy()

l = surfer.layer(strip, 0, y, 1024)          # strip, x, y, on-screen width
screen.add(l)
l.fast_scroll(True)
l.set_offset(px)                             # float pixels; wraps at strip.w
```

`set_offset` wraps automatically — no two-sprite tricks. With
`fast_scroll(True)` (needs an opaque strip; on the P4, triple-buffer
mode) per-frame motion becomes one DMA band copy plus a sliver repaint
instead of a full recompose: measured on the P4, a full-screen
three-layer parallax scene is **19 fps naive vs 63–65 fps with fast
layers**. Rules: fast layers must not overlap each other (stack them in
disjoint horizontal bands), and anything drawn on top of a fast layer
(the player sprite) must be a LATER SIBLING in the same parent — the
layer damages overlays as it shifts. Sub-pixel offsets are free; a
layer that stops moving repaints its band once.

The full demo is
[examples/parallax.py](../bindings/surfer/examples/parallax.py)
(`import parallax` from tulip mode): sky, mountains and ground bands at
0.1x/0.5x/1x with a bobbing ship, printing fps once a second.

## Widgets

Prebuilt controls with the default baked theme. Factories (also available
capitalized: `surfer.Slider` is `surfer.slider`):

| factory | value type |
|---|---|
| `surfer.slider(x, y, w=48, h=330)` | `float` 0.0–1.0 |
| `surfer.knob(x, y, size=64)` | `float` 0.0–1.0 (vertical drag, DAW-style; `size < 52` picks the small 40px style) |
| `surfer.checkbox(x, y)` | `bool` |
| `surfer.dropdown(x, y, w, ["a", "b", ...])` | `int` selected index |
| `surfer.button(x, y, w, h, label="")` | none — `.callback` fires on release inside; `.label = "..."` relabels |

Every widget has the node position attributes plus:

```python
w.value              # read/write, in the natural type above
w.callback = fn      # called on user interaction with the new value
                     # (slider/knob: float, checkbox: bool, dropdown: int)
w.node               # the widget's root Node, for tree operations
w.detach()
```

Setting `.value` programmatically does **not** fire the callback — only
user interaction does. A widget must be parented (`screen.add(w)` /
`node.add(w)`) before it renders.

```python
s = surfer.slider(700, 140)
screen.add(s)
s.callback = lambda v: synth.set("cutoff", v)
```

Inside a `scrollview`, slider and knob drags always win; taps on
checkboxes and dropdowns yield to scrolling after 8 px of travel.

## Constants

```python
surfer.FONT_UI16   surfer.FONT_UI28   surfer.FONT_MONO16

surfer.KEY_TEXT    surfer.KEY_LEFT    surfer.KEY_RIGHT   surfer.KEY_UP
surfer.KEY_DOWN    surfer.KEY_PGUP    surfer.KEY_PGDN    surfer.KEY_HOME
surfer.KEY_END     surfer.KEY_BACKSPACE  surfer.KEY_DELETE  surfer.KEY_ENTER

surfer.TOUCH_DOWN  surfer.TOUCH_MOVE  surfer.TOUCH_UP
```

`surfer.frame_rate(fps)` is game mode: it locks `surfer.tick()` to the
nearest divisor of the panel's measured refresh rate and returns the
actual locked fps — early frames wait for the vsync boundary, late
frames slip whole refresh periods, so motion stays quantized to the
panel instead of wobbling with scene load. A steady half-rate plays
better than a 45-70 swing; pick the rate your worst frame always fits.
`frame_rate(0)` (the default) uncaps and returns the panel rate. Note
the P4 bench panel actually refreshes at 69.7 Hz — `frame_rate(30)`
locks 34.8 there; scale per-frame speeds by the return value if world
speed matters. Locked demos: forest (full rate), parallax (half rate).

`a.hits(b)` tests whether two nodes' on-screen footprints overlap
(axis-aligned box test on absolute position and w/h; transformed
sprites use their transformed footprint). It's the collision primitive
— cheap enough to test every bullet against every enemy every frame.
Returns False if either node is hidden or detached. parallax uses it
for shot-vs-UFO: a hit spawns a scaling fireball (an ARGB explosion
image animated by `.scale` — color art scales in hardware) and hides
both nodes.

`surfer.cpu()` returns busy-percent per core since the last call — two
entries on the P4 (MicroPython runs on core 1), one process-wide entry
on desktop, empty on web. Poll it about once a second next to an fps
meter; parallax draws both: `34 fps (35)` (measured + lock target) with
`cpu 41% / 12%` beneath.

`surfer.keys_held()` returns the keys currently held down — state, not
events, up to 8 at once. Poll it every frame for game controls: it has
no repeat delay, and it's what lets a ship thrust and fire at the same
time (the parallax flight model — velocity + drag from held arrows —
is the reference). Use `keys()` for typing, `keys_held()` for driving;
call `keys()` once per frame anyway to drain the event queue.

Key events from `surfer.keys()` are `(kind, text, shift)`; `text` holds
the typed characters when `kind == KEY_TEXT`, else `""`.

## tulip.py helpers

`bindings/surfer/tulip.py` layers a tulipcc-style shell on top:

- **`UIScreen`** — holds live UI objects. `screen.add(el, x=None, y=None)`
  positions and parents in one call and returns `el`; `screen.remove(el)`;
  `screen.hide()` / `screen.present()` detach/reattach the whole layer.
- **`Console`** — a terminal-lite on a textgrid: `write(str)` with wrap
  and scroll.
- **`Repl`** — line editing, history (↑/↓), `:`-block continuation,
  tracebacks rendered on screen. `repl.feed("code\n")` scripts input.

## A complete program

```python
import surfer

surfer.init(800, 480)
root = surfer.screen()

panel = surfer.group(20, 20)
root.add(panel)
panel.add(surfer.label("mixer", 0, 0, surfer.rgb(240, 242, 248), surfer.FONT_UI28))

sliders = []
for i, name in enumerate(["cutoff", "res", "env", "lfo"]):
    s = surfer.slider(i * 110, 60)
    s.callback = lambda v, n=name: print(n, "=", v)
    panel.add(s)
    sliders.append(s)

while surfer.tick():
    pass
```
