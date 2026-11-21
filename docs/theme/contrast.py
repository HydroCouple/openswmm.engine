#!/usr/bin/env python3
"""Measure WCAG contrast for the HydroCouple documentation tokens in both modes.

Reads hc-tokens.css, resolves each (foreground, background) pair below for the light and
dark blocks, and fails (exit 1) when a pair is under its threshold: 4.5:1 for text,
3:1 for large text and meaningful graphics.

    python3 contrast.py [path/to/hc-tokens.css]
"""
import re
import sys
from pathlib import Path

PAIRS = [  # (label, foreground, background, minimum)
    ("Body text on page", "text", "bg", 4.5),
    ("Body text on surface", "text", "surface", 4.5),
    ("Muted text on page", "text-muted", "bg", 4.5),
    ("Muted text on raised surface", "text-muted", "raised", 4.5),
    ("Link on page", "link", "bg", 4.5),
    ("Link on raised surface (sidebar)", "link", "raised", 4.5),
    ("Link hover on page", "link-hover", "bg", 4.5),
    ("Active navigation on selection", "on-accent-soft", "accent-soft", 4.5),
    ("Button text on primary", "on-primary", "primary", 4.5),
    ("Code text on code block", "code-text", "code-bg", 4.5),
    ("Inline code on its chip", "inline-code", "inline-code-bg", 4.5),
    ("Caution text on caution surface", "on-flag-soft", "flag-soft", 4.5),
    ("Success text on success surface", "on-ok-soft", "ok-soft", 4.5),
    ("Planned text on planned surface", "on-planned-soft", "planned-soft", 4.5),
    ("Danger text on danger surface", "on-danger-soft", "danger-soft", 4.5),
    ("Hero title on hero start", "on-hero", "hero-from", 4.5),
    ("Hero title on hero end (large)", "on-hero", "hero-to", 3.0),
    ("Hero lede on hero middle", "hero-lede", "hero-mid", 4.5),
    ("Diagram label on node", "text", "diagram-node", 4.5),
    ("Diagram edge on page", "diagram-edge", "bg", 3.0),
    ("Primary accent mark on page", "primary", "bg", 3.0),
    ("Control border on page", "line-strong", "bg", 1.0),
    ("Footer text on footer", "footer-text", "footer-bg", 4.5),
    ("Footer link on footer", "footer-link", "footer-bg", 4.5),
    ("Footer note on footer", "footer-muted", "footer-bg", 4.5),
]


def inherit(modes):
    """Dark blocks omit tokens that do not change between modes."""
    for token, value in modes["light"].items():
        modes["dark"].setdefault(token, value)
    return modes


def luminance(hex_color):
    hex_color = hex_color.lstrip("#")
    channels = [int(hex_color[i:i + 2], 16) / 255 for i in (0, 2, 4)]
    linear = [c / 12.92 if c <= 0.04045 else ((c + 0.055) / 1.055) ** 2.4 for c in channels]
    return 0.2126 * linear[0] + 0.7152 * linear[1] + 0.0722 * linear[2]


def ratio(a, b):
    la, lb = sorted((luminance(a), luminance(b)), reverse=True)
    return (la + 0.05) / (lb + 0.05)


def blocks(css):
    """Return {mode: {token: hex}} from the first light and first dark rule blocks."""
    found = {}
    css = re.sub(r"/\*.*?\*/", "", css, flags=re.S)
    for selector, body in re.findall(r"([^{}]+)\{([^{}]*)\}", css):
        mode = "dark" if "dark" in selector else "light" if ":root" in selector else None
        if not mode or mode in found:
            continue
        found[mode] = dict(re.findall(r"--hc-([a-z-]+):\s*(#[0-9a-fA-F]{6})\s*;", body))
    return found


def main():
    path = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(__file__).resolve().parent / "hc-tokens.css"
    modes = inherit(blocks(path.read_text()))
    failures = 0
    print(f"{'Pair':40} {'Light':>8} {'Dark':>8}  min")
    for label, fg, bg, minimum in PAIRS:
        cells = []
        for mode in ("light", "dark"):
            value = ratio(modes[mode][fg], modes[mode][bg])
            flag = "" if value >= minimum else "!"
            failures += bool(flag)
            cells.append(f"{value:6.2f}{flag:1}")
        print(f"{label:40} {cells[0]:>8} {cells[1]:>8}  {minimum}")
    print("FAIL" if failures else "OK", f"({failures} pairs under threshold)")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
