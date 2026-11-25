#!/usr/bin/env python3
"""The version CI builds the Python packages at, and stamping it into the checkout.

    python scripts/python_release_version.py resolve --ref "$GITHUB_REF" [--dev-run N]
    python scripts/python_release_version.py stamp 6.0.0a4.dev355

resolve prints the version to stamp, or nothing when the committed version stands:

    refs/tags/v<X.Y.Z[-phase.N]>  an engine release. The tag must equal CMakeLists.txt's
                                  version; prints its PEP 440 form (v6.0.0-alpha.4 -> 6.0.0a4).
    refs/tags/py-v<version>       the Python packages alone at that engine version: its PEP 440
                                  form, optionally with .postN and/or .devN (py-v6.0.0a4,
                                  py-v6.0.0a4.post1).
    --dev-run N                   development wheels: the PEP 440 form plus .devN.

It exits 1, naming the problem, for a tag that does not fit. stamp writes a version of that
shape into python/pyproject.toml, packages/gpu-omp/pyproject.toml and the companion's
openswmm == pin. It is for CI's checkout: bump_version.py sets the committed version.
"""
from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

import check_version_sync as sync

SUFFIX = r"(\.post[0-9]+)?(\.dev[0-9]+)?"
PYTHON_DECLARATIONS = [d for d in sync.CORE if d[2] == "python"]


def engine_release(root: Path) -> dict[str, str]:
    """The release CMakeLists.txt declares, in each spelling."""
    return sync.release_forms(*sync.from_cmake(root / "CMakeLists.txt"))


def resolve(ref: str, dev_run: int | None = None, root: Path | None = None) -> str:
    """The version to stamp for this git ref, or '' to keep the committed one."""
    release = engine_release(sync.ROOT if root is None else root)
    if ref.startswith("refs/tags/py-v"):
        version = ref[len("refs/tags/py-v"):]
        if re.fullmatch(re.escape(release["pep440"]) + SUFFIX, version) is None:
            raise ValueError(
                f"tag py-v{version} does not match CMakeLists.txt's version: use "
                f"py-v{release['pep440']}, optionally with .postN and/or .devN "
                "(another version needs scripts/bump_version.py first)")
        return version
    if ref.startswith("refs/tags/v"):
        tag = ref[len("refs/tags/v"):]
        if tag != release["semver"]:
            raise ValueError(f"tag v{tag} does not match CMakeLists.txt's version "
                             f"{release['semver']} (set it with scripts/bump_version.py)")
        return release["pep440"]
    if dev_run is not None:
        return f"{release['pep440']}.dev{dev_run}"
    return ""


def stamp(version: str, root: Path | None = None) -> None:
    """Write version into both Python packages and the companion's pin."""
    root = sync.ROOT if root is None else root
    release = engine_release(root)["pep440"]
    if re.fullmatch(re.escape(release) + SUFFIX, version) is None:
        raise ValueError(f"{version!r} is not {release} with an optional .postN and .devN")
    texts: dict[str, str] = {}
    for name, pattern, _ in PYTHON_DECLARATIONS:
        if name not in texts:
            texts[name] = sync.read(root / name)
        texts[name] = sync.replace(texts[name], name, pattern, version)
    for name, text in texts.items():
        sync.write(root / name, text)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    sub = parser.add_subparsers(dest="command", required=True)
    r = sub.add_parser("resolve", help="print the version to stamp for a git ref")
    r.add_argument("--ref", required=True, help="the git ref, as in GITHUB_REF")
    r.add_argument("--dev-run", type=int, help="build development wheels numbered N")
    s = sub.add_parser("stamp", help="write a version into the Python packages")
    s.add_argument("version")
    args = parser.parse_args(argv)
    try:
        if args.command == "resolve":
            print(resolve(args.ref, args.dev_run))
        else:
            stamp(args.version)
            print(f"Python packages stamped {args.version}")
    except (OSError, ValueError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
