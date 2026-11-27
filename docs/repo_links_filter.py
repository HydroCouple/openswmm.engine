"""Doxygen input filter for the OpenSWMM Engine's Markdown pages.

The Markdown files are written for GitHub, where ``[vcpkg.json](vcpkg.json)`` opens the file
in the repository. The documentation site hosts only the pages Doxygen builds, so this filter
rewrites relative links to other repository files and folders into links to them on GitHub,
on the branch the manual is built from. Images, links to the Markdown pages Doxygen builds
(``PUBLISHED``), anchors, absolute URLs and targets that are not in the repository are left
for Doxygen. Lines are rewritten in place, so warning line numbers still match.

It follows MeshKit's ``docs/repo_links_filter.py``. Doxygen runs it as
``python3 repo_links_filter.py <file>`` (FILTER_PATTERNS in the Doxyfile) and reads the
filtered text from standard output.
"""
import os
import re
import sys

REPOSITORY = "https://github.com/HydroCouple/openswmm.engine"
#: The branch GitHub Actions builds (a pull request's head branch), else the default branch.
BRANCH = os.environ.get("GITHUB_HEAD_REF") or os.environ.get("GITHUB_REF_NAME") or "HEAD"
#: The Markdown files and folders the Doxyfile's INPUT publishes, from the repository root.
PUBLISHED = ("README.md", "docs/authors.md", "docs/manuals/manuals.md", "docs/manuals/engine",
             "docs/manuals/reference", "docs/manuals/application", "src/legacy/engine",
             "src/legacy/output", "src/engine", "include")
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LINK = re.compile(r"\]\(([^)\s]+)\)")
LEFT_TO_DOXYGEN = re.compile(r"^(?:[a-z][a-z0-9+.-]*:|#)", re.IGNORECASE)


def is_image(line: str, close: int) -> bool:
    """Whether the ``]`` at ``close`` ends the alt text of an image, ``![...]``."""
    depth = 0
    for i in range(close, -1, -1):
        if line[i] == "]":
            depth += 1
        elif line[i] == "[":
            depth -= 1
            if depth == 0:
                return i > 0 and line[i - 1] == "!"
    return False


def published(relative: str) -> bool:
    return relative.endswith(".md") and any(
        relative == p or relative.startswith(p + "/") for p in PUBLISHED)


def rewrite(target: str, folder: str) -> str:
    if LEFT_TO_DOXYGEN.search(target):
        return target
    path, _, anchor = target.partition("#")
    absolute = os.path.normpath(os.path.join(folder, path))
    relative = os.path.relpath(absolute, ROOT).replace(os.sep, "/")
    if relative.startswith("..") or not os.path.exists(absolute) or published(relative):
        return target
    kind = "tree" if os.path.isdir(absolute) else "blob"
    url = f"{REPOSITORY}/{kind}/{BRANCH}/{relative}"
    return url + ("#" + anchor if anchor else "")


def main() -> None:
    source = os.path.abspath(sys.argv[1])
    folder = os.path.dirname(source)
    fenced = False
    with open(source, encoding="utf-8") as stream:
        for line in stream:
            if line.lstrip().startswith(("```", "~~~")):
                fenced = not fenced
            if not fenced:
                line = LINK.sub(lambda m: m.group(0) if is_image(line, m.start())
                                else "](" + rewrite(m.group(1), folder) + ")", line)
            sys.stdout.write(line)


if __name__ == "__main__":
    main()
