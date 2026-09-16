"""Fetch every FPL endpoint the firmware uses (plus candidates) and save raw JSON to outputs/raw/.

Usage: python3 fetch_all.py [entry_id]
"""
import sys

from fpl import entry_id_from_argv, get, save


def main():
    entry_id = entry_id_from_argv(sys.argv)
    save("bootstrap-static", get("bootstrap-static/"))
    entry = get(f"entry/{entry_id}/")
    if entry is None:
        sys.exit(f"entry {entry_id} not found")
    save("entry", entry)
    gw = entry["current_event"]

    for name, path in [
        # Used by firmware
        ("history", f"entry/{entry_id}/history/"),
        ("picks", f"entry/{entry_id}/event/{gw}/picks/"),
        ("live", f"event/{gw}/live/"),
        # Candidates for new features
        ("fixtures_gw", f"fixtures/?event={gw}"),
        ("event_status", "event-status/"),
        ("transfers", f"entry/{entry_id}/transfers/"),
        ("dream_team", f"dream-team/{gw}/"),
        ("element_summary_1", "element-summary/1/"),
    ]:
        save(name, get(path))

    # Private classic leagues only (league_type "x"); "s" are system leagues (overall, country, club).
    for league in entry["leagues"]["classic"]:
        if league["league_type"] == "x":
            save(f"league_classic_{league['id']}", get(f"leagues-classic/{league['id']}/standings/"))
    for league in entry["leagues"]["h2h"]:
        save(f"league_h2h_{league['id']}", get(f"leagues-h2h/{league['id']}/standings/"))


if __name__ == "__main__":
    main()
