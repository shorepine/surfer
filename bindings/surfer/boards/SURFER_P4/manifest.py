include("$(PORT_DIR)/boards/manifest.py")
# tulip mode, frozen in: `import tulip` at the REPL starts it
module("tulip.py", base_path="$(BOARD_DIR)/../..")
# the Gamma 9001 drum machine UI: `import gamma9001`
module("gamma9001.py", base_path="$(BOARD_DIR)/../../examples")
# the sprite demo (Kenney CC0 art): `import space`
module("space.py", base_path="$(BOARD_DIR)/../../examples")
module("space_assets.py", base_path="$(BOARD_DIR)/../../examples")
