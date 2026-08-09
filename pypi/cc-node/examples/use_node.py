"""Repo entrypoint — same file ships inside the pip package at cc_node/examples/."""
from pathlib import Path
import runpy
runpy.run_path(str(Path(__file__).resolve().parents[1] / "cc_node" / "examples" / "use_node.py"),
               run_name="__main__")
