include("$(PORT_DIR)/variants/manifest.py")
# tulip mode, frozen in: `import tulip` starts it (same as SURFER_P4)
module("tulip.py", base_path="..")
# the Gamma 9001 drum machine UI: `import gamma9001`
module("gamma9001.py", base_path="../examples")
