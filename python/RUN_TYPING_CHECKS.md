# Run the mypy typing checks (handoff)

Goal: reproduce CI job **Typing (mypy)** locally and confirm it passes after the
`python_version` fix in `pyproject.toml`. This mirrors `.github/workflows/typing.yml`.

## Background (why this matters)

CI was failing with:

```
numpy/__init__.pyi:737: error: Type statement is only supported in Python 3.12 and greater  [syntax]
Found 1 error in 1 file (errors prevented further checking)
```

Root cause: numpy 2.5 ships `.pyi` stubs that use PEP 695 `type X = ...` statements.
mypy only parses those when the **target** `python_version` is >= 3.12. `[tool.mypy]`
in `pyproject.toml` had `python_version = "3.10"`, so mypy aborted before checking any
project code. The fix bumped it to `"3.12"`. This doc verifies that fix.

## Hard requirement

mypy must **run under Python 3.12 or newer**. This is separate from the
`python_version` config target — the interpreter that executes mypy must itself be
>= 3.12 to parse numpy's `type` statements. Check first:

```bash
python3 --version   # must be 3.12.x or newer
```

If the only available interpreter is < 3.12, the checks cannot run; install Python
3.12+ (e.g. via `uv`, `pyenv`, or `actions/setup-python`) before continuing. Do not
work around this by editing numpy or downgrading the config.

## Setup

```bash
cd python                 # repo's python/ directory — all commands run from here
pip install mypy numpy    # same as CI; no version pins
mypy --version            # sanity check it installed
```

## Run the checks

Two invocations, exactly as the CI matrix runs them:

```bash
# Job 1 — default mode, whole engine package
mypy openswmm/engine

# Job 2 — strict mode, pure-Python surface
mypy --strict tests/typing/test_surface.py
```

(`mypy openswmm/engine` picks up `[tool.mypy]` from `pyproject.toml`, including
`python_version = "3.12"`. `--strict` on job 2 overrides the relaxed defaults.)

## Pass / fail criteria

PASS — each command prints:

```
Success: no issues found in N source files
```

and exits 0.

FAIL conditions to watch for:
- The numpy `Type statement is only supported in Python 3.12` error → the config bump
  didn't take effect, or mypy is running under Python < 3.12. Re-check both.
- `errors prevented further checking` → a fatal/syntax error stopped the run; report
  the file and line verbatim.
- Real type errors in `openswmm/engine` or `tests/typing/` → report the full output;
  these are genuine findings to triage, not the numpy/config problem above.

## What to report back

For each of the two commands: the exact command, its exit code, and the full mypy
output (not a summary). If anything fails, do not attempt fixes beyond confirming the
interpreter version — report the output and stop.
