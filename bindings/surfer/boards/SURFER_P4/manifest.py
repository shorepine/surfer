include("$(PORT_DIR)/boards/manifest.py")
# tulip mode, frozen in: `import tulip` at the REPL starts it
module("tulip.py", base_path="$(BOARD_DIR)/../..")
