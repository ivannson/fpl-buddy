"""Print the key schema (type + example) of each saved raw JSON, collapsing arrays to their first item.

Usage: python3 schema_dump.py  -> writes outputs/schema.txt
"""
import json

from fpl import OUTPUTS, RAW
OUT = OUTPUTS / "schema.txt"


def walk(node, prefix, lines):
    if isinstance(node, dict):
        for k, v in node.items():
            walk(v, f"{prefix}.{k}" if prefix else k, lines)
    elif isinstance(node, list):
        lines.append(f"{prefix}[]  (len={len(node)})")
        if node:
            walk(node[0], f"{prefix}[0]", lines)
    else:
        ex = json.dumps(node)
        lines.append(f"{prefix}: {type(node).__name__} = {ex[:80]}")


def main():
    lines = []
    for f in sorted(RAW.glob("*.json")):
        lines.append(f"\n===== {f.stem} =====")
        walk(json.loads(f.read_text()), "", lines)
    OUT.write_text("\n".join(lines))
    print("\n".join(lines))


if __name__ == "__main__":
    main()
