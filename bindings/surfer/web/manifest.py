include("$(PORT_DIR)/variants/manifest.py")
# tulip mode, frozen in: `import tulip` starts it (same as SURFER_P4)
module("tulip.py", base_path="..")
# the Gamma 9001 drum machine UI: `import gamma9001`
module("gamma9001.py", base_path="../examples")
# the sprite demo (Kenney CC0 art): `import space`
module("space.py", base_path="../examples")
module("space_assets.py", base_path="../examples")
# the parallax scrolling benchmark: `import parallax`
module("parallax.py", base_path="../examples")
module("parallax_assets.py", base_path="../examples")
# the forest game: `import forest` (touch-drag the elf; the web REPL
# owns the arrow keys)
module("forest.py", base_path="../examples")
module("forest_assets.py", base_path="../examples")

module("pulse.py", base_path="../examples")

module("padtest.py", base_path="../examples")
