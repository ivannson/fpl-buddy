"""Price rise/fall data (new for 2026/27 in bootstrap-static elements + game_config).

  python3 price_changes.py snapshot [entry_id]   save a timestamped snapshot + print squad/top movers
  python3 price_changes.py verify               compare the two most recent snapshots: did projections come true?

Fields observed per element:
  price_change_percent       progress toward a change (+ rise / - fall); ~±100 appears to be the trigger
  price_change_hourly_rate   current rate of change of that percent
  price_change_projections   [{offset 0..2, projected_percent, likelihood -5..5}], offset N = game_config.settings.price_change_deadlines[N]
  price_change_locked_until  ISO time; player cannot change price until then (percent/likelihood are 0 while locked)
  price_change_calibrating   bool
Global: game_config.settings.price_change_deadlines[], game_config.status.price_change_last_updated

Snapshot a few hours before a deadline and again after it, then run verify, to confirm what the fields mean.
"""
import json
import sys
from datetime import datetime, timezone

from fpl import OUTPUTS, entry_id_from_argv, get

SNAP_DIR = OUTPUTS / "price_snapshots"
PRICE_KEYS = [
    "now_cost", "cost_change_event", "cost_change_start", "price_change_percent", "price_change_hourly_rate",
    "price_change_projections", "price_change_locked_until", "price_change_calibrating", "web_name",
    "transfers_in_event", "transfers_out_event", "selected_by_percent",
]


def fmt(e):
    proj = " ".join(f"{p['projected_percent']:>6}({p['likelihood']:+d})" for p in e["price_change_projections"])
    lock = f" locked→{e['price_change_locked_until'][:16]}" if e["price_change_locked_until"] else ""
    return (f"{e['web_name']:<16} £{e['now_cost'] / 10:4.1f}  now {e['price_change_percent']:>6}%  "
            f"rate {e['price_change_hourly_rate']:>+5}/h  proj {proj}{lock}")


def snapshot(entry_id):
    boot = get("bootstrap-static/")
    cfg = boot["game_config"]
    snap = {
        "fetched_at": datetime.now(timezone.utc).isoformat(),
        "price_change_deadlines": cfg["settings"]["price_change_deadlines"],
        "price_change_last_updated": cfg["status"]["price_change_last_updated"],
        "elements": {e["id"]: {k: e[k] for k in PRICE_KEYS} for e in boot["elements"]},
    }
    SNAP_DIR.mkdir(parents=True, exist_ok=True)
    path = SNAP_DIR / f"{snap['fetched_at'][:19].replace(':', '')}.json"
    path.write_text(json.dumps(snap, indent=1, ensure_ascii=False))

    print(f"deadlines: {snap['price_change_deadlines']}  last_updated: {snap['price_change_last_updated']}")
    els = list(snap["elements"].values())
    print("\nTop projected risers (next deadline):")
    for e in sorted(els, key=lambda e: -float(e["price_change_projections"][0]["projected_percent"]))[:8]:
        print("  " + fmt(e))
    print("\nTop projected fallers (next deadline):")
    for e in sorted(els, key=lambda e: float(e["price_change_projections"][0]["projected_percent"]))[:8]:
        print("  " + fmt(e))

    entry = get(f"entry/{entry_id}/", quiet=True)
    picks = get(f"entry/{entry_id}/event/{entry['current_event']}/picks/", quiet=True)
    print(f"\nSquad of entry {entry_id}:")
    for p in picks["picks"]:
        print("  " + fmt(snap["elements"][p["element"]]))
    print(f"\nsaved {path}")


def verify():
    snaps = sorted(SNAP_DIR.glob("*.json"))
    if len(snaps) < 2:
        sys.exit("need at least two snapshots")
    old, new = (json.loads(p.read_text()) for p in snaps[-2:])
    print(f"old {old['fetched_at']}  deadlines {old['price_change_deadlines']}")
    print(f"new {new['fetched_at']}  deadlines {new['price_change_deadlines']}\n")

    rows = []
    for eid, o in old["elements"].items():
        n = new["elements"].get(eid)
        if not n:
            continue
        rows.append((n["now_cost"] - o["now_cost"], float(o["price_change_percent"]),
                     float(o["price_change_projections"][0]["projected_percent"]),
                     o["price_change_projections"][0]["likelihood"], o, n))

    changed = [r for r in rows if r[0]]
    print(f"{len(changed)} price changes between snapshots")
    for delta, pct, proj, lik, o, n in sorted(changed, key=lambda r: -abs(r[2])):
        print(f"  {o['web_name']:<16} {delta:+d}  was {pct:6.1f}% proj {proj:6.1f}% lik {lik:+d}  -> now {n['price_change_percent']}%")

    print("\nlikelihood(offset 0) vs outcome:")
    for lik in range(-5, 6):
        group = [r for r in rows if r[3] == lik]
        if group:
            rises = sum(r[0] > 0 for r in group)
            falls = sum(r[0] < 0 for r in group)
            print(f"  {lik:+d}: n={len(group):>3}  rose={rises:>3}  fell={falls:>3}")

    print("\n|projected| >= 100 but no change:")
    for delta, pct, proj, lik, o, n in rows:
        if abs(proj) >= 100 and not delta:
            print(f"  {o['web_name']:<16} proj {proj:6.1f}% lik {lik:+d} locked {o['price_change_locked_until']}")


if __name__ == "__main__":
    if len(sys.argv) > 1 and sys.argv[1] == "verify":
        verify()
    else:
        snapshot(entry_id_from_argv(sys.argv[1:]))
