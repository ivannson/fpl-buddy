"""Regression check: does every field src/main.cpp reads still exist, with the expected type?

Also verifies team names still map to kit files in data/kits (slugify + alias logic ported from main.cpp).
Run fetch_all.py first. Writes outputs/firmware_field_check.txt.
"""
import re
from pathlib import Path

from fpl import OUTPUTS, ROOT, load_raw

KITS_DIR = ROOT.parent / "data" / "kits"

# (raw file, path, expected python type). "[]" means "first element of array".
CHECKS = [
    ("entry", "current_event", int),
    ("entry", "summary_overall_rank", int),
    ("entry", "summary_overall_points", int),
    ("history", "current[].event", int),
    ("history", "current[].overall_rank", int),
    ("bootstrap-static", "events[].id", int),
    ("bootstrap-static", "events[].is_current", bool),
    ("bootstrap-static", "events[].is_next", bool),
    ("bootstrap-static", "events[].finished", bool),
    ("bootstrap-static", "events[].deadline_time", str),
    ("bootstrap-static", "events[].deadline_time_epoch", int),
    ("bootstrap-static", "elements[].id", int),
    ("bootstrap-static", "elements[].web_name", str),
    ("bootstrap-static", "elements[].element_type", int),
    ("bootstrap-static", "elements[].team", int),
    ("bootstrap-static", "element_types[].id", int),
    ("bootstrap-static", "element_types[].singular_name_short", str),
    ("bootstrap-static", "teams[].id", int),
    ("bootstrap-static", "teams[].name", str),
    ("picks", "picks[].element", int),
    ("picks", "picks[].position", int),
    ("picks", "picks[].multiplier", int),
    ("picks", "picks[].is_captain", bool),
    ("picks", "picks[].is_vice_captain", bool),
    ("live", "elements[].id", int),
    ("live", "elements[].explain[].stats[].identifier", str),
    ("live", "elements[].explain[].stats[].points", int),
    ("live", "elements[].explain[].stats[].value", int),
] + [
    ("live", f"elements[].stats.{k}", int)
    for k in [
        "total_points", "minutes", "goals_scored", "assists", "clean_sheets", "goals_conceded", "own_goals",
        "penalties_saved", "penalties_missed", "yellow_cards", "red_cards", "saves", "bonus",
        "defensive_contribution",
    ]
]

# Identifiers handled explicitly in addBreakdownPointsByIdentifier(); anything else lands in "other".
FIRMWARE_IDENTIFIERS = {
    "minutes", "goals_scored", "assists", "clean_sheets", "goals_conceded", "own_goals", "penalties_saved",
    "penalties_missed", "yellow_cards", "red_cards", "saves", "bonus", "defensive_contribution",
    "defensive_contributions",
}

# normalizeKitTeamSlug() aliases in main.cpp
KIT_ALIASES = {
    "afc_bournemouth": "bournemouth", "brighton_and_hove_albion": "brighton", "manchester_city": "man_city",
    "manchester_utd": "man_utd", "manchester_united": "man_utd", "newcastle_utd": "newcastle",
    "newcastle_united": "newcastle", "nott_m_forest": "nottingham_forest", "nottm_forest": "nottingham_forest",
    "tottenham_hotspur": "tottenham", "west_ham_united": "west_ham", "wolverhampton_wanderers": "wolves",
}


def resolve(node, parts):
    """Yield all values at a dotted path, expanding every [] over all array items."""
    if not parts:
        yield node
        return
    head, rest = parts[0], parts[1:]
    is_array = head.endswith("[]")
    key = head[:-2] if is_array else head
    if not isinstance(node, dict) or key not in node:
        return
    child = node[key]
    if is_array:
        for item in child or []:
            yield from resolve(item, rest)
    else:
        yield from resolve(child, rest)


def slugify(name):
    return re.sub(r"_+$", "", re.sub(r"[^a-z0-9]+", "_", name.lower()).lstrip("_"))


def main():
    lines = []
    failures = 0
    for raw, path, typ in CHECKS:
        values = list(resolve(load_raw(raw), path.split(".")))
        present = [v for v in values if v is not None]
        bad = [v for v in present if not isinstance(v, typ) or (typ is int and isinstance(v, bool))]
        if not values:
            status, failures = "MISSING", failures + 1
        elif bad:
            status, failures = f"TYPE ({type(bad[0]).__name__})", failures + 1
        else:
            status = "ok" + (f" ({len(values) - len(present)} null)" if len(present) < len(values) else "")
        lines.append(f"{status:<16} {raw}: {path}")

    live = load_raw("live")
    seen = {s["identifier"] for e in live["elements"] for x in e["explain"] for s in x["stats"]}
    unhandled = sorted(seen - FIRMWARE_IDENTIFIERS)
    lines.append(f"\nexplain identifiers seen: {sorted(seen)}")
    lines.append(f"unhandled by firmware (-> brOtherPts): {unhandled or 'none'}")
    failures += bool(unhandled)

    kit_teams = {re.sub(r"_(gk|outfield)_.*$", "", f.name) for f in KITS_DIR.glob("*.rgb565")}
    lines.append("\nteam -> kit slug:")
    api_slugs = set()
    for t in load_raw("bootstrap-static")["teams"]:
        slug = KIT_ALIASES.get(slugify(t["name"]), slugify(t["name"]))
        api_slugs.add(slug)
        ok = slug in kit_teams
        failures += not ok
        lines.append(f"  {'ok     ' if ok else 'NO KIT '} {t['id']:>2} {t['name']:<16} -> {slug}")
    unused = sorted(kit_teams - api_slugs)
    lines.append(f"kit files not matching any current team: {unused or 'none'}")

    lines.append(f"\n{failures} problem(s)")
    out = "\n".join(lines)
    (OUTPUTS / "firmware_field_check.txt").write_text(out)
    print(out)


if __name__ == "__main__":
    main()
