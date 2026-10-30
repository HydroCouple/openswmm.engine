#!/usr/bin/env python3
"""Rerun failed CTest commands under a debugger with their original directory."""

import argparse
import json
from pathlib import Path
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("build", type=Path)
    parser.add_argument("debugger", choices=("gdb", "lldb"))
    args = parser.parse_args()
    build = args.build.resolve()
    logs = list((build / "Testing/Temporary").glob("LastTestsFailed*.log"))
    if not logs:
        print(f"No failed-test log found under {build}")
        return
    failed = max(logs, key=lambda p: p.stat().st_mtime)
    names = {
        line.split(":", 1)[1] for line in failed.read_text().splitlines() if ":" in line
    }
    metadata = json.loads(
        subprocess.check_output(
            [
                "ctest",
                "--test-dir",
                str(build),
                "-C",
                "Debug",
                "--show-only=json-v1",
            ]
        )
    )
    for test in metadata["tests"]:
        if test["name"] not in names:
            continue
        command = test.get("command")
        if not command:
            print(f"No executable command registered for {test['name']}")
            continue
        properties = {p["name"]: p["value"] for p in test.get("properties", [])}
        cwd = properties.get("WORKING_DIRECTORY", str(build))
        print(f"Debugger rerun: {test['name']} ({cwd})", flush=True)
        if args.debugger == "gdb":
            debugger = [
                "gdb",
                "-batch",
                "-ex",
                "set pagination off",
                "-ex",
                "set confirm off",
                "-ex",
                "handle SIGSEGV stop print nopass",
                "-ex",
                "run",
                "-ex",
                "thread apply all bt full",
                "-ex",
                "quit",
                "--args",
            ]
        else:
            debugger = [
                "lldb",
                "--batch",
                "-o",
                "run",
                "-o",
                "thread backtrace all",
                "-o",
                "quit",
                "--",
            ]
        # A debugger must not read the failed-test list or consume later names
        # as interactive commands. Keep CTest's arguments and working directory.
        try:
            result = subprocess.run(
                debugger + command,
                cwd=cwd,
                stdin=subprocess.DEVNULL,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
                errors="replace",
                timeout=300,
            )
            output = result.stdout
        except subprocess.TimeoutExpired as error:
            output = error.stdout or b""
            if isinstance(output, bytes):
                output = output.decode(errors="replace")
            output += "\nDebugger rerun exceeded 300 seconds."
        print("\n".join(output.splitlines()[:400]))


if __name__ == "__main__":
    main()
