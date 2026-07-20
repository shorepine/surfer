include("$(PORT_DIR)/boards/manifest.py")
# boot straight into tulip mode (frozen main.py runs after boot.py)
module("main.py", base_path="$(BOARD_DIR)")
# tulip mode, frozen in: `import tulip` at the REPL starts it
module("tulip.py", base_path="$(BOARD_DIR)/../..")
# the Gamma 9001 drum machine UI: `import gamma9001`
module("gamma9001.py", base_path="$(BOARD_DIR)/../../examples")
# the sprite demo (Kenney CC0 art): `import space`
module("space.py", base_path="$(BOARD_DIR)/../../examples")
module("space_assets.py", base_path="$(BOARD_DIR)/../../examples")
# the parallax scrolling benchmark: `import parallax`
module("parallax.py", base_path="$(BOARD_DIR)/../../examples")
module("parallax_assets.py", base_path="$(BOARD_DIR)/../../examples")
# the forest game (arrow keys / touch-drag the elf): `import forest`
module("forest.py", base_path="$(BOARD_DIR)/../../examples")
module("forest_assets.py", base_path="$(BOARD_DIR)/../../examples")
# A8 tint color cycling (hardware one-entry palette): `import pulse`
module("pulse.py", base_path="$(BOARD_DIR)/../../examples")
