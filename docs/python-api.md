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
surfer.screen()              # the root Node
surfer.rgb(r, g, b)          # 0-255 each → packed RGB565 color int
surfer.screenshot(path)      # dump the framebuffer as binary PPM → bool
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
