#!/usr/bin/env python3
"""One-time migration: split docs/manuals/user/user_manual.md into per-chapter
@page sources under docs/manuals/user/manual/, mirroring the reference-manual
restructure. See plans/DOCUMENTATION_OVERHAUL_PLAN_2026-08-11.md.

Existing {#user_manual_*} heading anchors become the page ids, so any @ref
to them keeps resolving. Deeper headings keep their explicit anchors.
"""
import re
import sys
from pathlib import Path

USER = Path(__file__).resolve().parent.parent / "docs" / "manuals" / "user"

# heading-text prefix -> output filename
FILE_MAP = [
    ("CHAPTER 1", "Chapter1.md"),
    ("CHAPTER 2", "Chapter2.md"),
    ("CHAPTER 3", "Chapter3.md"),
    ("CHAPTER 4", "Chapter4.md"),
    ("CHAPTER 5", "Chapter5.md"),
    ("CHAPTER 6", "Chapter6.md"),
    ("CHAPTER 7", "Chapter7.md"),
    ("CHAPTER 8", "Chapter8.md"),
    ("CHAPTER 9", "Chapter9.md"),
    ("CHAPTER 10", "Chapter10.md"),
    ("CHAPTER 11", "Chapter11.md"),
    ("CHAPTER 12", "Chapter12.md"),
    ("CHAPTER 13", "Chapter13.md"),
    ("APPENDIX A", "AppendixA.md"),
    ("APPENDIX B", "AppendixB.md"),
    ("APPENDIX C", "AppendixC.md"),
    ("APPENDIX D", "AppendixD.md"),
    ("APPENDIX E", "AppendixE.md"),
]

HEAD = re.compile(r"^## +(?!#)(.+?)(?: *\{#([a-z_0-9]+)\})? *$", re.M)


def promote(body):
    out = []
    for ln in body.split("\n")[1:]:
        m = re.match(r"^(#{3,6}) ", ln)
        out.append("#" * (len(m.group(1)) - 1) + ln[len(m.group(1)) :] if m else ln)
    return "\n".join(out).lstrip("\n")


def main():
    mono_path = USER / "user_manual.md"
    text = mono_path.read_text()
    matches = list(HEAD.finditer(text))
    front = text[: matches[0].start()]
    blocks = []
    for i, m in enumerate(matches):
        end = matches[i + 1].start() if i + 1 < len(matches) else len(text)
        blocks.append((m.group(1).strip(), m.group(2), text[m.start() : end]))

    used, pages = set(), []
    for prefix, fname in FILE_MAP:
        hit = next(
            (b for b in blocks if b[0].upper().startswith(prefix) and b[0] not in used),
            None,
        )
        if hit is None:
            print(f"MISSING: {prefix}")
            continue
        used.add(hit[0])
        heading, anchor, body = hit
        page_id = anchor or "user_manual_" + re.sub(r"\W+", "_", prefix.lower())
        content = f"@page {page_id} {heading}\n\n@tableofcontents\n\n{promote(body)}"
        (USER / "manual" / fname).write_text(content + "\n")
        pages.append((page_id, heading))
        print(f"wrote manual/{fname:14s} @page {page_id}")

    leftover = [b[0] for b in blocks if b[0] not in used]
    print(f"front-matter blocks kept in parent: {leftover}")
    kept = "\n".join(b[2].rstrip() + "\n" for b in blocks if b[0] not in used)
    sub = "\n".join(f"- @subpage {pid} — {h}" for pid, h in pages)
    mono_path.write_text(
        front.rstrip() + "\n\n" + kept.rstrip() + "\n\n## Manual Contents\n\n" + sub + "\n"
    )
    print("parent rewritten: user_manual.md")


if __name__ == "__main__":
    sys.exit(main())
