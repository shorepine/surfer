# SURFER_P4 boots straight into tulip mode on the panel. Ctrl-C on the
# serial console interrupts to the raw REPL (import tulip to come back);
# any import error falls through to the serial REPL instead of wedging.
import tulip
