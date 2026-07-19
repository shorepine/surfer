# tulip mode: an on-screen MicroPython REPL rendered into a surfer
# textgrid, plus a tulipcc-flavored UIScreen. Run with the unix port:
#
#   ~/micropython/ports/unix/build-standard/micropython bindings/micropython/tulip.py
#
# Then, live on the glass:
#   >>> s = surfer.slider(700, 120)
#   >>> screen.add(s)
#   >>> s.callback = lambda v: print("slider:", v)
#   >>> s.y_pos = 200
import io
import sys
import surfer

W, H = 1024, 600


class UIScreen:
    """Holds the live UI objects, tulipcc-style. add() parents anything
    (widget or node) into this screen's group; the group detaches whole
    for app switching."""

    def __init__(self):
        self.elements = []
        self.group = surfer.group(0, 0)
        surfer.screen().add(self.group)

    def add(self, el, x=None, y=None):
        if x is not None:
            el.x_pos = x
        if y is not None:
            el.y_pos = y
        self.group.add(el)
        self.elements.append(el)
        return el

    def remove(self, el):
        el.detach()
        if el in self.elements:
            self.elements.remove(el)

    def hide(self):
        self.group.detach()

    def present(self):
        surfer.screen().add(self.group)


class Console:
    """A terminal-lite on a textgrid: sequential write() with wrap and
    scroll, plus an edited input line with an inverse-video cursor."""

    def __init__(self, cols, rows, x=0, y=0):
        self.cols, self.rows = cols, rows
        self.fg = surfer.rgb(200, 205, 215)
        self.bg = surfer.rgb(18, 20, 25)
        self.grid = surfer.textgrid(cols, rows, self.fg, self.bg)
        self.grid.x_pos = x
        self.grid.y_pos = y
        surfer.screen().add(self.grid)
        self.row = 0
        self.col = 0

    def _newline(self):
        self.col = 0
        if self.row >= self.rows - 1:
            self.grid.grid_scroll(1)
        else:
            self.row += 1

    def write(self, s):
        for ch in s:
            if ch == "\n":
                self._newline()
            else:
                if self.col >= self.cols:
                    self._newline()
                self.grid.set_cell(self.col, self.row, ch, self.fg, self.bg)
                self.col += 1

    def show_input(self, prompt, line, cursor):
        text = prompt + line
        self.grid.set_row(self.row, text)
        ccol = len(prompt) + cursor
        if ccol < self.cols:
            under = text[ccol] if ccol < len(text) else " "
            self.grid.set_cell(ccol, self.row, under, self.bg, self.fg)

    def commit_input(self, prompt, line):
        self.grid.set_row(self.row, prompt + line)
        self._newline()


class Repl:
    def __init__(self, console, namespace):
        self.c = console
        self.g = namespace
        self.g["print"] = self._print
        self.line = ""
        self.cursor = 0
        self.history = []
        self.hist_i = 0
        self.pending = []  # continuation lines
        self.c.write("surfer %dx%d — tulip mode. objects: surfer, screen\n" % (W, H))
        self._prompt()

    def _print(self, *args, **kw):
        sep = kw.get("sep", " ")
        end = kw.get("end", "\n")
        self.c.write(sep.join(str(a) for a in args) + end)

    def _prompt(self):
        self.p = "... " if self.pending else ">>> "
        self.c.show_input(self.p, self.line, self.cursor)

    def _run(self, src):
        try:
            try:
                r = eval(src, self.g)
                if r is not None:
                    self.c.write(repr(r) + "\n")
            except SyntaxError:
                exec(src, self.g)
        except Exception as e:
            # sys.print_exception needs a native stream on the unix port
            buf = io.StringIO()
            sys.print_exception(e, buf)
            self.c.write(buf.getvalue())

    def key(self, kind, text, shift):
        if kind == surfer.KEY_TEXT:
            self.line = self.line[: self.cursor] + text + self.line[self.cursor :]
            self.cursor += len(text)
        elif kind == surfer.KEY_BACKSPACE and self.cursor > 0:
            self.line = self.line[: self.cursor - 1] + self.line[self.cursor :]
            self.cursor -= 1
        elif kind == surfer.KEY_DELETE:
            self.line = self.line[: self.cursor] + self.line[self.cursor + 1 :]
        elif kind == surfer.KEY_LEFT and self.cursor > 0:
            self.cursor -= 1
        elif kind == surfer.KEY_RIGHT and self.cursor < len(self.line):
            self.cursor += 1
        elif kind == surfer.KEY_HOME:
            self.cursor = 0
        elif kind == surfer.KEY_END:
            self.cursor = len(self.line)
        elif kind == surfer.KEY_UP and self.history:
            self.hist_i = max(0, self.hist_i - 1)
            self.line = self.history[self.hist_i]
            self.cursor = len(self.line)
        elif kind == surfer.KEY_DOWN and self.history:
            self.hist_i = min(len(self.history), self.hist_i + 1)
            self.line = (
                self.history[self.hist_i] if self.hist_i < len(self.history) else ""
            )
            self.cursor = len(self.line)
        elif kind == surfer.KEY_ENTER:
            self.c.commit_input(self.p, self.line)
            src = self.line
            if src.strip():
                self.history.append(src)
            self.hist_i = len(self.history)
            if self.pending or src.rstrip().endswith(":"):
                if src.strip() == "" and self.pending:
                    self._run("\n".join(self.pending))
                    self.pending = []
                else:
                    self.pending.append(src)
            elif src.strip():
                self._run(src)
            self.line = ""
            self.cursor = 0
        self._prompt()

    def feed(self, s):
        """Scripted input for demos/tests: printable chars + newlines."""
        for ch in s:
            if ch == "\n":
                self.key(surfer.KEY_ENTER, "", False)
            else:
                self.key(surfer.KEY_TEXT, ch, False)


def main():
    surfer.init(W, H)

    cell = surfer.textgrid(1, 1)  # probe cell metrics
    cols, rows = W // max(1, cell.w), H // max(1, cell.h)
    cell.destroy()

    # module globals: app/hosts (and the web shell) reach these as
    # tulip.console / tulip.screen / tulip.repl
    global console, screen, repl
    console = Console(cols, rows)

    # UI layer sits in front of the console text, tulip-style
    screen = UIScreen()

    ns = {"surfer": surfer, "screen": screen, "console": console}
    repl = Repl(console, ns)
    ns["repl"] = repl

    import os
    feed = os.getenv("SURF_REPL_FEED") if hasattr(os, "getenv") else None
    shot = os.getenv("SURF_SHOT") if hasattr(os, "getenv") else None
    state = {"frames": 0}

    def _frame():
        """One tick of tulip mode; False when the host wants to quit."""
        if not surfer.tick():
            return False
        for kind, text, shift in surfer.keys():
            repl.key(kind, text, shift)
        state["frames"] += 1
        if feed and state["frames"] == 5:
            repl.feed(feed)
        if shot and state["frames"] == 10:
            surfer.screenshot(shot)
            if feed:
                return False
        return True

    global frame
    frame = _frame

    import sys
    if sys.platform == "webassembly":
        # the browser owns the loop: index.html calls tulip.frame() per
        # requestAnimationFrame. Looping here would suspend the VM inside
        # this module's import, which wedges ASYNCIFY (see surfer's
        # bindings/surfer/web/hal_sdl_web.c).
        return

    while frame():
        pass


main()
