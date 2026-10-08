#!/usr/bin/env python3
"""Summarise a writer round-trip sweep into the audit's evidence tables.

Reads results.jsonl and reports, per section/kind, how many decks are affected
and an example. Deck counts matter more than finding counts: one deck with 241
changed [XSECTIONS] rows is one defect, not 241.

Usage: summarize.py <results.jsonl> [--section XSECTIONS] [--examples N]
"""
from __future__ import annotations

import argparse
import json
from collections import Counter, defaultdict


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("jsonl")
    ap.add_argument("--section", default=None,
                    help="drill into one SECTION or SECTION/kind")
    ap.add_argument("--examples", type=int, default=3)
    a = ap.parse_args()

    recs = []
    with open(a.jsonl) as fh:
        for line in fh:
            line = line.strip()
            if line:
                try:
                    recs.append(json.loads(line))
                except json.JSONDecodeError:
                    pass  # a partial trailing line while the sweep runs

    print(f"decks graded: {len(recs)}")
    for gate in ("status", "g1", "g2", "g3", "g4"):
        c = Counter(r[gate] for r in recs if gate in r)
        if c:
            print(f"  {gate:7s} " + "  ".join(f"{k}={v}" for k, v in c.most_common()))

    decks_by_key: dict[str, set[str]] = defaultdict(set)
    example: dict[str, str] = {}
    for r in recs:
        for f in r.get("g4_findings", []):
            key = f"{f['section']}/{f['kind']}"
            decks_by_key[key].add(r["deck"])
            example.setdefault(key, f["detail"])

    if a.section:
        want = a.section.upper()
        print(f"\n--- {want} ---")
        n = 0
        for r in recs:
            for f in r.get("g4_findings", []):
                k = f"{f['section']}/{f['kind']}"
                if k == want or f["section"] == want:
                    print(f"{r['deck'].split('corpus/')[-1]}\n    {k}\n    {f['detail'][:200]}")
                    n += 1
                    if n >= a.examples:
                        return 0
        return 0

    print(f"\n{'decks':>6}  section/kind")
    for key, decks in sorted(decks_by_key.items(), key=lambda kv: -len(kv[1])):
        print(f"{len(decks):6d}  {key}")
        print(f"        e.g. {example[key][:150]}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
