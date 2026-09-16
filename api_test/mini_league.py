"""Mini-league ranks for an entry, and how much data each approach costs on-device.

  python3 mini_league.py [entry_id]

Two sources:
  1. entry/{id}/  -> leagues.classic[]: entry_rank, entry_last_rank, rank_count, entry_percentile_rank,
     active_phases[] (per-phase rank; phase 1 = overall, others = months). ~7 KB, already fetched by firmware.
  2. leagues-classic/{id}/standings/ -> full table (50/page): rank, last_rank, event_total, total, names,
     plus last_updated_data. Needed for "points behind leader / gap to next".
"""
import sys

from fpl import OUTPUTS, entry_id_from_argv, get, save


def main():
    entry_id = entry_id_from_argv(sys.argv)
    entry = get(f"entry/{entry_id}/")
    lines = [f"entry {entry_id} '{entry['name']}' GW{entry['current_event']}"]
    phases = {p["id"]: p["name"] for p in get("bootstrap-static/", quiet=True)["phases"]}

    for lg in entry["leagues"]["classic"]:
        kind = "private" if lg["league_type"] == "x" else "system"
        move = (lg["entry_last_rank"] or 0) - (lg["entry_rank"] or 0)
        phase_str = ", ".join(f"{phases.get(p['phase'], p['phase'])}: {p['rank']}" for p in lg["active_phases"])
        lines.append(f"  [{kind:<7}] {lg['id']:>8} {lg['name'][:24]:<24} rank {lg['entry_rank']}/{lg['rank_count']} "
                     f"(last {lg['entry_last_rank']}, {move:+d})  pct {lg['entry_percentile_rank']}  | {phase_str}")

    for lg in entry["leagues"]["classic"]:
        if lg["league_type"] != "x":
            continue
        st = get(f"leagues-classic/{lg['id']}/standings/")
        save(f"league_classic_{lg['id']}", st)
        results = st["standings"]["results"]
        leader = results[0]["total"] if results else 0
        lines.append(f"\n{lg['name']} (updated {st['last_updated_data']}, has_next={st['standings']['has_next']})")
        for i, r in enumerate(results[:20]):
            me = "  <-" if r["entry"] == entry_id else ""
            gap = results[i - 1]["total"] - r["total"] if i else 0
            lines.append(f"  {r['rank']:>3} (last {r['last_rank']:>3}) {r['entry_name'][:20]:<20} "
                         f"GW {r['event_total']:>3}  total {r['total']:>4}  -{leader - r['total']:<3} gap-above {gap}{me}")

    out = "\n".join(lines)
    (OUTPUTS / "mini_league.txt").write_text(out)
    print(out)


if __name__ == "__main__":
    main()
