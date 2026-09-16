"""Shared helpers for the FPL API test scripts."""
import json
import os
import re
import time
from pathlib import Path

import requests

BASE = "https://fantasy.premierleague.com/api"
ROOT = Path(__file__).parent
OUTPUTS = ROOT / "outputs"
RAW = OUTPUTS / "raw"
HEADERS = {"User-Agent": "fpl-buddy/1.0", "Accept": "application/json"}


def _config_entry_id():
    """Read FPL_ENTRY_ID from include/fpl_config.h so scripts test the same team as the firmware."""
    m = re.search(r"#define\s+FPL_ENTRY_ID\s+(\d+)", (ROOT.parent / "include" / "fpl_config.h").read_text())
    return m.group(1) if m else "0"


# Override with FPL_ENTRY_ID env var or argv.
DEFAULT_ENTRY_ID = int(os.environ.get("FPL_ENTRY_ID") or _config_entry_id())


def get(path, quiet=False):
    """GET an API path. Returns parsed JSON or None on non-200."""
    url = f"{BASE}/{path}"
    t0 = time.time()
    r = requests.get(url, headers=HEADERS, timeout=30)
    if not quiet:
        print(f"{r.status_code} {len(r.content):>9,} B {time.time() - t0:5.2f}s  {path}")
    if r.status_code != 200:
        return None
    try:
        return r.json()
    except ValueError:
        return None


def save(name, data, subdir=RAW):
    subdir.mkdir(parents=True, exist_ok=True)
    path = subdir / f"{name}.json"
    path.write_text(json.dumps(data, indent=2, ensure_ascii=False))
    return path


def load_raw(name):
    """Load a saved raw response, fetching nothing. Run fetch_all.py first."""
    return json.loads((RAW / f"{name}.json").read_text())


def entry_id_from_argv(argv):
    return int(argv[1]) if len(argv) > 1 else DEFAULT_ENTRY_ID
