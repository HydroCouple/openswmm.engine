#!/usr/bin/env python3
"""Set the engine and Python package version in every declaration, from one argument.

    python scripts/bump_version.py 6.0.0-alpha.5    # or 6.0.0-beta.1, 6.0.0-rc.1, 6.0.1

CMakeLists.txt is the version's source. This writes its project VERSION and
OPENSWMM_PRERELEASE and every declaration that restates them
(check_version_sync.DECLARATIONS), then runs the gate. The Python packages get the
PEP 440 form (6.0.0a5) without a .devN: development builds and Python-only releases add
theirs in CI (python_release_version.py).

OPENSWMM_PRERELEASE is a CACHE variable, so an existing build tree keeps reporting the old
version until it is reconfigured with -DOPENSWMM_PRERELEASE=<new> or a fresh cache.
"""
from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

import check_version_sync as sync

SEMVER = re.compile(r"([0-9]+\.[0-9]+\.[0-9]+)(?:-(alpha|beta|rc)\.([0-9]+))?")


def bump(version: str, root: Path | None = None) -> None:
    """Write version (X.Y.Z or X.Y.Z-alpha|beta|rc.N) into every declaration under root.

    Nothing is written unless every declaration is found exactly once.
    """
    root = sync.ROOT if root is None else root
    m = SEMVER.fullmatch(version)
    if m is None:
        raise ValueError(f"{version!r} is not X.Y.Z or X.Y.Z-alpha.N, -beta.N or -rc.N")
    forms = sync.release_forms(m.group(1), m.group(2) or "", int(m.group(3) or 0))
    forms["python"] = forms["pep440"]
    texts: dict[str, str] = {}
    for name, pattern, form in sync.DECLARATIONS:
        if name not in texts:
            texts[name] = sync.read(root / name)
        texts[name] = sync.replace(texts[name], name, pattern, forms[form])
    for name, text in texts.items():
        sync.write(root / name, text)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("version", help="X.Y.Z or X.Y.Z-alpha.N, -beta.N or -rc.N")
    args = parser.parse_args(argv)
    try:
        bump(args.version)
    except (OSError, ValueError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    print(f"Set {args.version} in {len({d[0] for d in sync.DECLARATIONS})} files. "
          "Reconfigure existing build trees with -DOPENSWMM_PRERELEASE=<new> "
          "(it is a CACHE variable).\n")
    return sync.main()


if __name__ == "__main__":
    sys.exit(main())
