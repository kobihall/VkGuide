"""Rewrites manifest.json in its compact form: numeric arrays and short objects on one line.

Run after editing the manifest by script:  python3 tools/optical_models/format_manifest.py
"""

import json
import os
import re

PATH = os.path.join(os.path.dirname(os.path.abspath(__file__)), "manifest.json")


def compact(obj):
    text = json.dumps(obj, indent=1)
    text = re.sub(r"\[\s*([-0-9.eE+null,\s]+?)\s*\]",
                  lambda m: "[" + ", ".join(x.strip() for x in m.group(1).split(",")) + "]", text)
    scalar = r"(?:[-0-9.eE+]+|\"[^\"]*\"|\[[^\[\]{}]*\]|null|true|false|\{[^{}]*\})"
    for _ in range(4):
        text = re.sub(r"\{\s*((?:\"[^\"]+\": " + scalar + r",?\s*)+)\}",
                      lambda m: "{" + " ".join(l.strip() for l in m.group(1).splitlines()) + "}"
                      if len(m.group(0)) < 260 else m.group(0), text)
    return text + "\n"


if __name__ == "__main__":
    with open(PATH) as f:
        manifest = json.load(f)
    with open(PATH, "w") as f:
        f.write(compact(manifest))
