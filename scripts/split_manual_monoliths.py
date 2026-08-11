#!/usr/bin/env python3
"""One-time migration: split monolithic manual .md files into per-chapter
Doxygen @page sources under sections/, making sections the single source.

See plans/DOCUMENTATION_OVERHAUL_PLAN_2026-08-11.md.

Modes:
  --dry-run   split in memory, diff against existing sections/, report deltas
  --write     write chapter files, rewrite parent page, delete redundant files

Heading convention in monoliths: chapters are `## Chapter N: Title`; inside a
chapter, `###` -> `##`, `####` -> `###`, etc. Generated chapter files start
with `@page <id> <title>` followed by @tableofcontents.
"""
import argparse
import difflib
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent / "docs" / "manuals" / "reference"

# monolith heading text (regex) -> (section filename, page-id suffix)
MANUALS = {
    "hydraulics": {
        "parent_page": "hydraulics_reference_manual",
        "title": "OpenSWMM Hydraulics Reference Manual",
        "chapters": [
            (r"Chapter 1", "Chapter1-Overview.md", "ch1_overview"),
            (r"Chapter 2", "Chapter2-HydraulicModel.md", "ch2_hydraulic_model"),
            (r"Chapter 3", "Chapter3-DynamicWave.md", "ch3_dynamic_wave"),
            (r"Chapter 4", "Chapter4-KinematicWave.md", "ch4_kinematic_wave"),
            (r"Chapter 5", "Chapter5-CrossSection.md", "ch5_cross_section"),
            (r"Chapter 6", "Chapter6-PumpsRegulators.md", "ch6_pumps_regulators"),
            (r"Chapter 7", "Chapter7-AdvancedFeatures.md", "ch7_advanced_features"),
            (r"Chapter 8", "Chapter8-FiniteVolume.md", "ch8_finite_volume"),
            (r"Chapter 9", "Chapter9-TwoDimensional.md", "ch9_two_dimensional"),
            (r"References", "References.md", "references"),
            (r"Appendix", "Appendix.md", "appendix"),
        ],
    },
    "hydrology": {
        "parent_page": "hydrology_reference_manual",
        "title": "OpenSWMM Hydrology Reference Manual",
        "chapters": [
            (r"Chapter 1", "Chapter1-Overview.md", "ch1_overview"),
            (r"Chapter 2", "Chapter2-Meteorology.md", "ch2_meteorology"),
            (r"Chapter 3", "Chapter3-SurfaceRunoff.md", "ch3_surface_runoff"),
            (r"Chapter 4", "Chapter4-Infiltration.md", "ch4_infiltration"),
            (r"Chapter 5", "Chapter5-Groundwater.md", "ch5_groundwater"),
            (r"Chapter 6", "Chapter6-Snowmelt.md", "ch6_snowmelt"),
            (r"Chapter 7", "Chapter7-RDII.md", "ch7_rdii"),
            (r"Glossary", "Glossary.md", "glossary"),
            (r"References", "References.md", "references"),
        ],
    },
    "quality": {
        "parent_page": "quality_reference_manual",
        "title": "OpenSWMM Water Quality Reference Manual",
        "chapters": [
            (r"Chapter 1", "Chapter1-Overview.md", "ch1_overview"),
            (r"Chapter 2", "Chapter2-UrbanRunoffQuality.md", "ch2_urban_runoff_quality"),
            (r"Chapter 3", "Chapter3-PollutantBuildup.md", "ch3_pollutant_buildup"),
            (r"Chapter 4", "Chapter4-SurfaceWashoff.md", "ch4_surface_washoff"),
            (r"Chapter 5", "Chapter5-TransportAndTreatment.md", "ch5_transport_treatment"),
            (r"Chapter 6", "Chapter6-LowImpactDevelopmentControls.md", "ch6_lid_controls"),
            (r"Glossary", "Glossary.md", "glossary"),
            (r"References", "References.md", "references"),
        ],
    },
}

H2 = re.compile(r"^## +(?!#)(.+?)\s*$", re.M)


def preprocess(text):
    """Normalize monolith quirks: BOMs, H1 chapter headings (hydrology),
    double-spaced headings, stray empty headings."""
    text = text.replace("﻿", "")
    # hydrology marks chapters/front-matter with H1; normalize to H2
    text = re.sub(r"^# +(?=\S)", "## ", text, flags=re.M)
    text = re.sub(r"^(#+) +", lambda m: m.group(1) + " ", text, flags=re.M)
    # drop empty headings and stray '**\**' artifacts
    text = re.sub(r"^#+ *\n", "", text, flags=re.M)
    text = re.sub(r"^\*\*\\\*\* *\n", "", text, flags=re.M)
    return text


def split_monolith(text):
    """Return (front_matter, [(heading, body)]) split on '## ' headings."""
    matches = list(H2.finditer(text))
    front = text[: matches[0].start()] if matches else text
    blocks = []
    for i, m in enumerate(matches):
        end = matches[i + 1].start() if i + 1 < len(matches) else len(text)
        blocks.append((m.group(1).strip(), text[m.start() : end]))
    return front, blocks


def promote(body):
    """Drop the '## ' chapter heading line and promote deeper headings one level."""
    lines = body.split("\n")
    out = []
    for ln in lines[1:]:  # skip the chapter '## ' heading itself
        m = re.match(r"^(#{3,6}) ", ln)
        out.append("#" * (len(m.group(1)) - 1) + ln[len(m.group(1)) :] if m else ln)
    return "\n".join(out).lstrip("\n")


def build_chapter(page_id, heading, body):
    return f"@page {page_id} {heading}\n\n@tableofcontents\n\n{promote(body)}"


def norm(s):
    return [re.sub(r"\s+", " ", ln).strip() for ln in s.splitlines() if ln.strip()]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--write", action="store_true")
    ap.add_argument("--manual", choices=list(MANUALS), action="append")
    args = ap.parse_args()
    manuals = args.manual or list(MANUALS)

    for name in manuals:
        cfg = MANUALS[name]
        mono_path = ROOT / f"{name}.md"
        sect_dir = ROOT / name / "sections"
        text = preprocess(mono_path.read_text())
        front, blocks = split_monolith(text)
        used = set()
        chapters = []
        for pat, fname, suffix in cfg["chapters"]:
            hit = next(
                (b for b in blocks if re.match(pat, b[0]) and b[0] not in used), None
            )
            if hit is None:
                print(f"[{name}] MISSING in monolith: {pat}")
                continue
            used.add(hit[0])
            page_id = f"{name}_ref_{suffix}"
            chapters.append((fname, page_id, hit[0], build_chapter(page_id, *hit)))

        leftover = [h for h, _ in blocks if h not in used]
        print(f"[{name}] front-matter blocks kept in parent: {leftover}")

        for fname, page_id, heading, content in chapters:
            old = sect_dir / fname
            if old.exists():
                # diff ignoring the header scaffolding we add
                new_body = norm(content)
                old_body = norm(old.read_text())
                only_old = [
                    ln
                    for ln in difflib.unified_diff(new_body, old_body, lineterm="", n=0)
                    if ln.startswith("+") and not ln.startswith("+++")
                ]
                # lines present only in the existing section file = deltas to review
                real = [
                    ln
                    for ln in only_old
                    if not re.match(r"^\+#* ?(Chapter|@page|@tableofcontents)", ln)
                    and len(ln) > 3
                ]
                status = f"{len(real):3d} section-only lines" if real else "in sync"
                print(f"  {fname:44s} {status}")
                if real and not args.write:
                    for ln in real[:6]:
                        print(f"      {ln[:120]}")
            if args.write:
                old.write_text(content + "\n")

        if args.write:
            # parent page: front matter + unmatched blocks (disclaimer,
            # abstract, lists, ...) + subpage list
            kept = "\n".join(b[1].rstrip() + "\n" for b in blocks if b[0] not in used)
            sub = "\n".join(
                f"- @subpage {pid} — {h}" for _, pid, h, _ in chapters
            )
            parent = (
                front.rstrip()
                + "\n\n"
                + kept.rstrip()
                + "\n\n## Manual Contents\n\n"
                + sub
                + "\n"
            )
            mono_path.write_text(parent)
            print(f"[{name}] parent rewritten: {mono_path.name}")


if __name__ == "__main__":
    sys.exit(main())
