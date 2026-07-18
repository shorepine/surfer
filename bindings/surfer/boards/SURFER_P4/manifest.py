include("$(PORT_DIR)/boards/manifest.py")
# tulip mode, frozen in: `import tulip` at the REPL starts it
module("tulip.py", base_path="$(BOARD_DIR)/../..")
# the Gamma 9001 drum machine UI: `import gamma9001`
module("gamma9001.py", base_path="$(BOARD_DIR)/../../examples")
