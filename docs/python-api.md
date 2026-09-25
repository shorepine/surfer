# surfer Python API

The MicroPython binding to surfer. The same module runs on every port:
the desktop (SDL), the ESP32-P4, the web and iOS.

**Arguments are positional.** The `name=default` forms below show what
you may leave off; only `Node.tween` also accepts keywords.

**A method on the wrong kind of node is usually silently ignored**
(`set_color` on a label does nothing). The ones that raise are the
transform, opacity, fade, tween, hitbox and `set_image` calls.

## Running it

```sh
make mpy      # builds MicroPython (MPY_DIR ?= ~/micropython) with the surfer module
~/micropython/ports/unix/build-standard/micropython bindings/surfer/repl.py
```

`repl.py` boots an on-screen REPL with `surfer` and `screen` in scope, so
everything below can be typed live.

## The module

```python
import surfer

surfer.init(1024, 600)       # w, h, single=False. Open the display, first.
                             # Calling it again rebuilds the scene (and resizes)
surfer.tick()                # input, animation, compose, present
                             # -> False when the window is closed
surfer.screen()              # the root Node
surfer.rgb(r, g, b)          # 0-255 each -> a packed RGB565 colour int
surfer.frame_rate(fps)       # lock tick() to a divisor of the refresh; 0 uncaps.
                             # -> the rate actually locked
surfer.cpu()                 # busy percent per core since the last call
```

The app owns the loop:

```python
while surfer.tick():
    for kind, text, shift, ctrl in surfer.keys():
        ...
```

Callbacks fire from inside `surfer.tick()`, on the same thread.

**On the web** a blocking loop would freeze the tab, so the browser
drives one frame per `requestAnimationFrame` and an app registers its
per-frame work as a hook:

```python
import sys, repl
if sys.platform == "webassembly":
    repl.app_frame = my_step       # called once per frame; return False to unhook
else:
    while surfer.tick():
        if my_step() is False:
            break
```

`frame_rate(fps)` is for games: it holds `tick()` to a steady rate the
panel can divide into, so motion doesn't wobble with load. The panel's
refresh differs by board (60.4 Hz on the P4X, 69.7 Hz on the v1.x EV
board), so scale speeds by the value it returns.

## Nodes

Nodes are the scene: pooled, retained, cheap. Add one to a parent and
the compositor repaints only what changes.

| factory | |
|---|---|
| `surfer.group(x, y)` | a container; draws nothing |
| `surfer.rect(x, y, w, h, color=grey)` | a solid, opaque rectangle |
| `surfer.label(text, x, y, color=white, font="ui12")` | proportional text |
| `surfer.textgrid(cols, rows, fg, bg, font="mono16")` | a grid of character cells (needs a monospace font) |
| `surfer.textinput(x, y, w, color=white, font="ui12")` | one editable line |
| `surfer.textarea(x, y, w, rows, color=white, font="ui12")` | editable, wrapped, `rows` lines tall |
| `surfer.scrollview(x, y, w, h)` | a clipped viewport; drag and flick scroll its children |
| `surfer.sprite(img, x, y)` | an image on screen |
| `surfer.filmstrip(img, frame_w, frame_h, x, y)` | an animation |
| `surfer.layer(img, x, y, view_w)` | a wrapping, scrolling window onto a wide image |

A `font` is a name (`"ui28"`, `"mono16"`; `surfer.fonts()` lists them), a
`Font` object, or a registry index. The `FONT_UI16`, `FONT_UI28` and
`FONT_MONO16` constants are those names.

Every node:

```python
n.x_pos, n.y_pos     # position in the parent: integers, read/write
n.w, n.h             # size, read only
n.hidden = True      # hide the subtree (write only)
n.add(child)         # a node or a widget
n.detach()           # out of the tree, all state kept
n.destroy()          # detach and free the subtree
n.damage()           # force a repaint (after you drew into its image)
n.hits(other)        # collision, see below
n.on_touch = fn      # fn(phase, x, y), screen coordinates; phase TOUCH_DOWN/MOVE/UP
```

`detach()` and `add()` round-trip losslessly, which is how a host swaps
whole screens.

Per type:

```python
label.set_text("new text")          # also textinput / textarea
label.set_wrap(120)                 # wrap width in pixels (0 = one line)
label.set_align(surfer.ALIGN_CENTER)   # ALIGN_LEFT / CENTER / RIGHT
rect.set_color(surfer.rgb(230, 150, 60))

group.set_clip(w, h)                # give a group a size: it clips, and can be tapped

grid.set_row(row, "text")           # a whole row, space-padded
grid.set_cell(col, row, "A", fg=0xffff, bg=0)   # one cell; a character or a codepoint
grid.set_cells(col, row, "a run", fg=0xffff, bg=0)
grid.set_colors(fg, bg)             # the default colours
grid.grid_scroll(rows)              # +n scrolls content up
grid.scrollback(mult)               # keep mult screens of history -> bool
grid.view([n]), grid.history()      # how far back the view is / how much there is

sv.scroll_to(x, y)                  # clamped
sv.scroll_offset()                  # -> (x, y)

layer.set_offset(px)                # float pixels; wraps at the image width
n.fast_scroll(True)                 # layer, sprite, scrollview, textgrid: see Layers
```

**A handler goes on the topmost node you tap, then walks up its
parents.** A handler on a background rect never sees a tap that lands on
a label drawn over it; put it on a group containing both. A scrollview
steals a child's gesture after 8 px of travel, so act on TOUCH_UP near
the TOUCH_DOWN point for a tap.

**Collision:** `a.hits(b)` compares the two boxes, then (for sprites and
filmstrips) the pixels: transparent pixels don't collide. Hidden or
detached nodes never hit. If `a` has [hitboxes](#hitboxes), the answer is
a tuple of which of `a`'s boxes hit (empty means no).

## Images and sprites

```python
img = surfer.image(open("ship.png", "rb").read())   # decode PNG bytes
img = surfer.image(png_bytes, True)       # ...as an A8 mask (alpha only)
img = surfer.image_new(w, h)              # blank RGB565, black
img = surfer.image_new(w, h, True)        # blank ARGB, transparent
img = surfer.image_new(w, h, surfer.A8)   # blank mask
img = surfer.image_new(w, h, fmt, True)   # ...in fast internal RAM if there is some
img.w, img.h, img.format, img.stride
img.tint = surfer.rgb(255, 40, 200)       # an A8 image's colour
img.destroy()                             # after every sprite using it is gone
surfer.image_scale(dst, src)              # scale src to fill dst -> True if hardware did it
```

Decode images at load time, not per frame. An image is not freed when
Python drops it: `destroy()` it, and only after the nodes showing it are
gone.

```python
s = surfer.sprite(img, x, y)
screen.add(s)
s.x_pos = 300         # the compositor repaints whatever it uncovers
s.scale = 1.5         # 1/16 .. 16
s.rot = 90            # degrees, multiples of 90 only
s.mirror_x = True     # mirrors apply before rotation
s.mirror_y = True
s.w, s.h              # the transformed footprint
s.set_src(x, y, w, h) # show one window of a big image (a sprite sheet cell, or a camera)
s.set_image(other)    # show a different picture on the same node
```

`scale`, `rot` and the mirrors raise on anything but a sprite or
filmstrip. `set_image` resets the window to the whole new picture and
keeps the node's scale, rotation, mirror and hitboxes.

**A8 masks** are a one-colour silhouette. Change `img.tint` and
`damage()` the sprite: every sprite showing that image recolours, and it
costs one repaint (on the P4 the tint is applied in hardware).

### Filmstrips

An animation is one image of frames, left to right (and row by row for a
grid):

```python
strip = surfer.image(open("walk.png", "rb").read())
f = surfer.filmstrip(strip, 64, 48, x, y)   # frame w, frame h
f.w, f.h              # one frame
f.frame = 2           # pick a frame
f.fps = 12.0          # play at 12 fps; 0 means you set .frame yourself
f.play([fps])         # start (optionally at a new speed)
f.stop()              # freeze, keeping fps
f.playing
```

Late frames are dropped, not replayed. `set_image` keeps the frame size
and recounts the frames.

### Drawing into an image

At load time, not per frame. A **paint** is a colour, `(colour, alpha)`,
or a gradient `((x0, y0, c0[, a0]), (x1, y1, c1[, a1]))`.

```python
g = surfer.image_new(512, 300)
g.fill(color)                                   # the whole image, opaque
g.fill(color, x, y, w, h)                       # a rectangle of it
g.poly([(0, 300), (256, 20), (512, 300)], paint)   # filled, anti-aliased
g.line(x0, y0, x1, y1, paint, width=1)          # round caps
g.lines([(0, 0), (40, 80), (90, 20)], paint, width=1)
g.circle(cx, cy, r, paint, width=0)             # 0: filled
g.ellipse(cx, cy, rx, ry, paint, width=0)
g.bezier([p0, c, p1], paint, width=2)           # 3 points: quadratic
g.bezier([p0, c0, c1, p1], paint, width=2)      # 4 points: cubic
g.blit(src, x, y, rot=0)                        # composite src; rot in quarter turns
```

### Your own pixels

An image supports the buffer protocol:

```python
mv = memoryview(img)          # img.stride bytes a row; format 0 RGB565, 1 ARGB, 2 A8
...                           # write pixels
img.flush()                   # publish them to the hardware (a no-op where the CPU draws)
sprite.damage()               # and repaint what shows them
```

Forget `flush()` and the desktop is perfect while the P4 tears.

### Saving and capturing

```python
open("out.png", "wb").write(surfer.write_png(img))
shot = surfer.fb_image()                  # the screen as an Image (a copy)
shot = surfer.fb_image(0, 0, 320, 240)    # ...a region
surfer.fb_read(x, y, w, h)                # a region as RGB888 bytes
surfer.screenshot(path)                   # a PPM file (desktop and web only)
```

`write_png(fb_image())` is a PNG screenshot on every port. An A8 image
saves as white with its alpha.

## Layers

For scrolling backgrounds, bake the tiles into one wide image at load
time and scroll it as a layer: one blit per frame instead of one per
tile.

```python
strip = surfer.image_new(2048, 128)
strip.fill(surfer.rgb(92, 148, 218))
tile = surfer.image(open("grass.png", "rb").read())
for x in range(0, 2048, 64):
    strip.blit(tile, x, 0)
tile.destroy()

l = surfer.layer(strip, 0, y, 1024)
screen.add(l)
l.fast_scroll(True)
l.set_offset(px)              # wraps at the image width
```

With `fast_scroll(True)` (an opaque image) each frame's motion is one
hardware band shift plus a thin repaint. Fast layers must not overlap
each other, and anything drawn over one should be a later sibling. When
sprites move over a fast layer, call `set_offset` first, then move the
sprites.

`sprite.fast_scroll(True)` does the same for a sprite whose `set_src`
window pans across a big opaque image (a game camera over a baked
world).

## Opacity, fades and tweens

```python
n.opacity = 0.4          # 0..1; sprite, filmstrip, layer, label
n.fade_out(ms=250)
n.fade_in(ms=250)
n.fade_to(0.4, ms=250)
n.fade_cancel()
n.fading

n.tween("x_pos", 300, 1000, ease="out")    # prop, to, ms=250, ease=None, start=None
n.tween_cancel("x_pos")                    # or no argument: all
n.tweening("x_pos")                        # or no argument: any
```

Tweenable: `"x_pos"`, `"y_pos"`, `"scale"`, `"opacity"`. Eases:
`"linear"` (default), `"in"`, `"out"`, `"in_out"`. Several tweens can run
on one node; setting a property directly cancels only that property's
tween. A group, rect or textgrid cannot fade and raises. Opacity is
visual only: taps and `hits()` ignore it.

## Hitboxes

A sprite or filmstrip can carry up to 32 collision boxes, which follow
its scale, mirror and rotation.

```python
hb = s.hitboxes.add(0, 8, 8, 8)       # x, y, w, h in the unrotated picture -> a Hitbox
len(s.hitboxes); s.hitboxes[0]; s.hitboxes.clear()
hb.x, hb.y, hb.w, hb.h                # read/write
hb.index                              # what hits() reports it as
hb.visible = True                     # draw its outline (for debugging)
hb.visible_color = surfer.rgb(0, 255, 0)
hb.remove()                           # later boxes shift down
```

While a node has hitboxes they are its collision shape; the other node's
pixels still count.

## Text

```python
surfer.fonts()                 # every face's name
surfer.fonts(True)             # the monospace ones
f = surfer.font("mono16")      # a name, or TTF-baked bytes
f.cell_w, f.cell_h             # a mono face's cell
f.codepoints()                 # every character it has
surfer.widget_font("ui12")     # the face widgets are built with, from now on
surfer.widget_font()           # -> its name
surfer.emoji("fire")           # the character: it goes in any label or grid
surfer.emoji()                 # every name
img = surfer.text_image(s, color, font="ui12", wrap_w=0)   # text baked into an ARGB image
```

A label can't scale; `text_image` on a sprite can. Emoji are a fallback
face: any codepoint the text face lacks is looked up in the colour emoji
set. An emoji takes two cells in a textgrid (`set_row` allows for that;
`set_cells` doesn't).

### Editing text

A textinput or textarea draws the text and a caret, nothing else, and
**has no keyboard of its own**: feed it keys.

```python
ti = surfer.textinput(x, y, 240)
ti.text, ti.caret            # read/write
ti.focus(True)
ti.mask = "*"                # a password field; None shows the text
for k in surfer.keys():
    if not ti.key(k):        # False when it didn't use the key
        ...                  # Enter (in a one-line field) and hotkeys fall through
ti.insert(s); ti.backspace(); ti.delete(); ti.move(delta, extend=False)
ti.index_from_x(x)

ta = surfer.textarea(x, y, 400, 5)
ta.rows, ta.lines            # lines shown / lines the text takes
ta.scroll_y                  # read/write
```

A tap places the caret and a drag selects. `key()` refuses Tab.

## Widgets

Prebuilt controls. They report through `.callback` and hold `.value`;
**setting `.value` from code does not fire the callback**. A widget must
be added to a node before it draws.

| factory | `.value` | callback gets |
|---|---|---|
| `slider(x, y, w=48, h=330)` | 0.0-1.0 | float |
| `knob(x, y, size=64)` | 0.0-1.0 | float |
| `checkbox(x, y)` | bool | bool |
| `dropdown(x, y, w, ["a", "b"])` | index | int |
| `button(x, y, w, h, label="")` | None | True, on release |
| `selector(x, y, positions)` | index | int |
| `radio(x, y, labels, vertical=True)` | index | int |
| `tabs(x, y, w, h, labels, tab_h=36, face, dim, text, text_active)` | index | int |
| `colorpicker(x, y, size)` | colour | int |
| `scrollbar(x, y, len, vertical=True)` | position | int |
| `led(x, y, color=red)` | brightness 0..1 (or bool) | never called |

- **slider**: wider than tall is horizontal. Under 30 px across it uses
  compact art.
- **knob**: vertical drag. `size` picks between a 40 px knob (size < 52)
  and a 64 px one. `kn.on_tap = fn(where)` handles a tap, 0..1 down the
  knob.
- **selector**: a knob with N detents; a tap advances one.
- **scrollbar**: `sb.set_range(total, visible, pos=0)` in your own unit;
  it hides itself when there is nothing to scroll.
- **tabs**: `h` includes the strip. `t.page(i)` is a group to fill; the
  widget shows one page at a time. `t.set_label(i, s)`,
  `t.set_face(i, c)`, `t.set_dim(i, c)`. Give `face` your page's
  background colour so the tab and page read as one.
- **radio**: `False` for a row.

Every widget:

```python
w.value; w.callback = fn
w.node                 # its root Node, for tree operations
w.x_pos, w.y_pos, w.w, w.h
w.hidden = True
w.detach()
w.color = c            # led, knob, selector, slider cap, tabs
b.label = "new"        # button (write only)
```

Inside a scrollview, slider and knob drags always win; taps on the
others yield to scrolling after 8 px. Capitalised aliases exist for
`Group`, `TextInput`, `Slider`, `Knob`, `Checkbox`, `Dropdown`, `Button`,
`Led`, `Selector`, `ColorPicker`.

## Input

```python
for kind, text, shift, ctrl in surfer.keys():   # drain every frame
    ...
surfer.keys_held()           # ((kind, text), ...): keys down now, up to 8, for games
surfer.wheel()               # [(x, y, dx, dy), ...]: wheel moves no scrollview took
surfer.touches()             # ((id, x, y), ...): every finger, ids stable while held
surfer.has_touch()
surfer.screen_keyboard([show])   # the on-screen keyboard where there is one; None elsewhere
```

`kind` is `KEY_TEXT` (then `text` holds the characters; Tab is `"\t"`)
or one of `KEY_LEFT RIGHT UP DOWN PGUP PGDN HOME END BACKSPACE DELETE
ENTER ESC`.

**ctrl+letter arrives as its control character** in `KEY_TEXT` (`^S` is
`"\x13"`) with `ctrl` False. `^C` is the interrupt and never arrives;
`^A` and `^E` arrive as Home and End. The `ctrl` flag is for keys with no
control character: ctrl+arrows, ctrl+Home/End, ctrl+PgUp/PgDn,
ctrl+Delete.

Use `keys()` for typing and `keys_held()` for steering. `wheel()` is the
desktop's second finger: a trackpad's two-finger gesture arrives as a
wheel, never as touches.

## Controllers

`surfer.pad(n)` is one normalised controller, whatever feeds it. Slots
0..3.

```python
pad = surfer.pad(0)
pad.up, pad.down, pad.left, pad.right
pad.a, pad.b, pad.x, pad.y, pad.l, pad.r, pad.start, pad.select
pad.lx, pad.ly, pad.rx, pad.ry        # sticks, -1.0..1.0
pad.dpad, pad.buttons                 # bitmasks
surfer.pad_keys(0)                    # the slot the keyboard drives; -1: none
```

The keyboard map: arrows or WASD are the d-pad, space or Z is A, X is B,
C is X, V is Y, Q and E are L and R. A gamepad and the keyboard feed the
same slot and merge, so a game works with either.

A driver, an on-screen pad or a test writes the pad:

```python
pad.set_dpad(surfer.DPAD_UP | surfer.DPAD_LEFT)
pad.set_buttons(surfer.BTN_A | surfer.BTN_R)
pad.set_stick(0, 0.2, -0.9)          # stick 0 or 1, x, y
pad.reset()
```

## 3D models

```python
m = surfer.mesh(glb_bytes, tex_png=None, textured=False)
m.render(img, rx, ry, rz, size, cx=img.w/2, cy=img.h/2, cull=None)
m.tris
m.destroy()
```

Renders a low-poly glTF (.glb) into an RGB565 or ARGB image with a
z-buffer and flat shading: `size` is the model's radius in pixels,
`rx/ry/rz` are degrees. Render small and scale the sprite up. `render`
doesn't flush: call `img.flush()` and damage the sprite. `textured=True`
samples the texture per pixel, for models with painted detail.

## Constants

```python
surfer.KEY_TEXT ... surfer.KEY_ESC
surfer.TOUCH_DOWN  surfer.TOUCH_MOVE  surfer.TOUCH_UP
surfer.ALIGN_LEFT  surfer.ALIGN_CENTER  surfer.ALIGN_RIGHT
surfer.A8
surfer.DPAD_UP  DPAD_DOWN  DPAD_LEFT  DPAD_RIGHT
surfer.BTN_A  BTN_B  BTN_X  BTN_Y  BTN_L  BTN_R  BTN_START  BTN_SELECT
surfer.FONT_UI16  FONT_UI28  FONT_MONO16    # the names "ui16", "ui28", "mono16"
```

## Test hooks

```python
surfer._touch(x, y, phase, id=0)          # a synthetic touch
surfer._key(kind, text="", shift=False, ctrl=False)
surfer._wheel(x, y, dx, dy)
```

They go through the same paths real input does.

## repl.py

`bindings/surfer/repl.py` is a REPL shell on top:

- **`UIScreen`**: `screen.add(el, x=None, y=None)` positions and parents
  in one call and returns `el`; `screen.remove(el)`; `screen.hide()` /
  `screen.present()`.
- **`Console`**: text with wrap and scroll on a textgrid.
- **`Repl`**: line editing, history, block continuation, tracebacks on
  screen. `repl.feed("code\n")` scripts input.

## A complete program

```python
import surfer

surfer.init(1024, 600)
root = surfer.screen()

panel = surfer.group(20, 20)
root.add(panel)
panel.add(surfer.label("mixer", 0, 0, surfer.rgb(240, 242, 248), "ui28"))

for i, name in enumerate(["cutoff", "res", "env", "lfo"]):
    s = surfer.slider(i * 110, 60)
    s.callback = lambda v, n=name: print(n, "=", v)
    panel.add(s)

while surfer.tick():
    pass
```

More: [examples/space.py](../bindings/surfer/examples/space.py) (sprites),
[examples/parallax.py](../bindings/surfer/examples/parallax.py) (layers),
[examples/gamma9001.py](../bindings/surfer/examples/gamma9001.py)
(widgets in a scrollview).
