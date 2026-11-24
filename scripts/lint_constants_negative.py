#!/usr/bin/env python3
"""Prove that lint_constants.py can fail, and only on code.

A guard that has never been seen to fail guards nothing. This builds a tiny
source tree in a temporary directory and runs the linter on it three times:

1. a literal in code must be reported, naming the constant to use;
2. the same literal in a comment and in a string must not be reported;
3. the same literal in the one file allowed to define it must not be reported.

Exit status 1 if any expectation fails.
"""
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
LINT = HERE / "lint_constants.py"

CASES = [
    # (name, relative file, contents, expected exit status, expected output substring)
    ("code literal is reported",
     "src/engine/hydraulics/Foo.cpp",
     "double g() {\n    return 32.2 * 2.0; // not a comment literal\n}\n",
     1, "Foo.cpp:2: literal 32.2 -> use constants::GRAVITY"),
    ("second family literal is reported with its own name",
     "src/couplers/Bar.cpp",
     "static const double days = seconds / 86400.0;\n",
     1, "Bar.cpp:1: literal 86400.0 -> use constants::SEC_PER_DAY"),
    ("comments and strings are not code",
     "src/engine/hydrology/Baz.cpp",
     "/* g = 32.2 ft/s^2\n   and 86400 s per day */\n"
     "// pi is 3.14159\n"
     'const char* s = "0.3048 m per ft";\n'
     "double x = 1.0; // 28.317 L per ft3\n",
     0, "0 literal copies"),
    ("the defining header is allowed",
     "src/engine/core/Constants.hpp",
     "constexpr double GRAVITY = 32.2;\nconstexpr double SEC_PER_DAY = 86400.0;\n",
     0, "0 literal copies"),
]


def run_case(name, rel, contents, want_status, want_text):
    with tempfile.TemporaryDirectory(prefix="lint_constants_") as tmp:
        path = Path(tmp) / rel
        path.parent.mkdir(parents=True)
        path.write_text(contents, encoding="utf-8")
        proc = subprocess.run([sys.executable, str(LINT), "--root", tmp],
                              capture_output=True, text=True)
    ok = proc.returncode == want_status and want_text in proc.stdout
    print(f"{'ok  ' if ok else 'FAIL'} {name}")
    if not ok:
        print(f"      expected exit {want_status} and {want_text!r}; got exit {proc.returncode}:")
        print("      " + proc.stdout.strip().replace("\n", "\n      "))
        if proc.stderr.strip():
            print("      " + proc.stderr.strip().replace("\n", "\n      "))
    return ok


def main():
    results = [run_case(*case) for case in CASES]
    print(f"lint_constants negative: {sum(results)}/{len(results)} cases behave")
    return 0 if all(results) else 1


if __name__ == "__main__":
    sys.exit(main())
