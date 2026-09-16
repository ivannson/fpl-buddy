"""Watch what updates during a LIVE gameweek, and how quickly. Run while matches are being played.

  python3 live_watch.py [entry_id] [interval_s=60] [duration_min=150]

Every interval it records (compact, no big payloads kept) to outputs/live_watch/<start>.jsonl:
  - entry/{id}/            summary_event_points, summary_event_rank, summary_overall_rank, leagues entry_rank
  - picks                  entry_history.points / rank / overall_rank, automatic_subs
  - event/{gw}/live/       sum of squad live points (what firmware computes), any 'modified' flags
  - event-status/          bonus_added / points state / leagues
  - private league tables  own event_total / rank / last_updated_data
It prints a line only when something changed, so you can see which endpoints are truly live.
"""
import json
import sys
import time
from datetime import datetime, timezone

from fpl import OUTPUTS, entry_id_from_argv, get


def sample(entry_id):
    entry = get(f"entry/{entry_id}/", quiet=True)
    gw = entry["current_event"]
    picks = get(f"entry/{entry_id}/event/{gw}/picks/", quiet=True)
    live = get(f"event/{gw}/live/", quiet=True)
    status = get("event-status/", quiet=True)

    by_id = {e["id"]: e for e in live["elements"]}
    squad_live = sum(by_id[p["element"]]["stats"]["total_points"] * p["multiplier"] for p in picks["picks"])
    s = {
        "gw": gw,
        "entry.summary_event_points": entry["summary_event_points"],
        "entry.summary_event_rank": entry["summary_event_rank"],
        "entry.summary_overall_rank": entry["summary_overall_rank"],
        "picks.points": picks["entry_history"]["points"],
        "picks.rank": picks["entry_history"]["rank"],
        "picks.overall_rank": picks["entry_history"]["overall_rank"],
        "picks.automatic_subs": len(picks["automatic_subs"]),
        "live.squad_points_x_mult": squad_live,
        "live.modified_count": sum(e.get("modified", False) for e in live["elements"]),
        "event_status": json.dumps(status, sort_keys=True),
    }
    for lg in entry["leagues"]["classic"]:
        if lg["league_type"] != "x":
            continue
        s[f"entry.league.{lg['id']}.entry_rank"] = lg["entry_rank"]
        st = get(f"leagues-classic/{lg['id']}/standings/", quiet=True)
        me = next((r for r in st["standings"]["results"] if r["entry"] == entry_id), None)
        s[f"standings.{lg['id']}.last_updated_data"] = st["last_updated_data"]
        if me:
            s[f"standings.{lg['id']}.rank"] = me["rank"]
            s[f"standings.{lg['id']}.event_total"] = me["event_total"]
    return s


def main():
    entry_id = entry_id_from_argv(sys.argv)
    interval = int(sys.argv[2]) if len(sys.argv) > 2 else 60
    duration = float(sys.argv[3]) if len(sys.argv) > 3 else 150
    out_dir = OUTPUTS / "live_watch"
    out_dir.mkdir(parents=True, exist_ok=True)
    start = datetime.now(timezone.utc)
    path = out_dir / f"{start.strftime('%Y%m%dT%H%M%S')}.jsonl"
    print(f"logging to {path}")

    prev = {}
    end = time.time() + duration * 60
    with path.open("a") as f:
        while time.time() < end:
            ts = datetime.now(timezone.utc).isoformat(timespec="seconds")
            try:
                s = sample(entry_id)
            except Exception as ex:  # keep watching through transient API errors
                print(f"{ts} error: {ex}")
                time.sleep(interval)
                continue
            f.write(json.dumps({"t": ts, **s}) + "\n")
            f.flush()
            changes = {k: (prev.get(k), v) for k, v in s.items() if prev.get(k) != v}
            if prev and changes:
                print(ts)
                for k, (a, b) in changes.items():
                    print(f"   {k}: {a} -> {b}")
            elif not prev:
                print(ts, json.dumps(s, indent=1))
            prev = s
            time.sleep(interval)


if __name__ == "__main__":
    main()
