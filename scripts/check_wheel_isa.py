#!/usr/bin/env python3
"""Fail when a Linux wheel ships instructions that only some CPUs of its architecture have.

A wheel tagged aarch64 or x86_64 must run on every CPU with that tag. The 6.0.0a4.dev1
aarch64 wheel did not: the engines were compiled with -mcpu=native on a Neoverse N2 build
runner, so they contained SVE instructions and died with SIGILL on Graviton2, Ampere Altra
and Apple Silicon Linux VMs. A smoke test on the build runner cannot see this, because that
CPU has SVE, so this script reads the instructions instead.

Every ELF library or executable in the given wheels, archives, directories or files is
disassembled with objdump and searched for:

- aarch64: SVE, SVE2 and SME (scalable vector and predicate registers, streaming mode);
- x86_64:  AVX-512 (zmm and mask registers, and the EVEX-only registers xmm16-31/ymm16-31).

The x86_64 baseline the engine targets (x86-64-v3: AVX2 and FMA) is allowed.
AArch64 libgcc_s also contains explicit SME ABI support and SVE unwind queries,
which do not impose a baseline CPU requirement. Only those known runtime cases
are allowed and reported separately; engine/plugin code is always checked.

Exit status: 0 when nothing is found, 1 when a file contains such instructions, 2 when a
file cannot be checked (for example an objdump that cannot read the architecture).
"""

from __future__ import annotations

import argparse
import collections
import hashlib
import os
import re
import shutil
import subprocess
import sys
import tarfile
import zipfile
from pathlib import Path

EM_X86_64 = 62
EM_AARCH64 = 183

CHECKS = {
    EM_AARCH64: (
        "SVE/SME",
        re.compile(
            r"\bz(?:[12]?\d|3[01])\.[bhsdq]\b"  # SVE vector registers
            r"|\bp(?:1[0-5]|\d)(?:/[zm]|\.[bhsdq])"  # SVE predicate registers
            r"|\bza(?:\d+[hv]?(?:\.[bhsdq])?)?\b"  # SME ZA array
            r"|\b(?:ptrues?|pfalse|while(?:l[eot]|h[is]|rw|wr)|rdvl|addvl|addpl"
            r"|cnt[bhwd]|inc[bhwd]|dec[bhwd]|sqinc[bhwd]|sqdec[bhwd]|uqinc[bhwd]|uqdec[bhwd]"
            r"|setffr|rdffrs?|wrffr|smstart|smstop|rdsvl|addsvl|addspl)\b"
        ),
    ),
    EM_X86_64: (
        "AVX-512",
        re.compile(r"%zmm\d+|%[xy]mm(?:1[6-9]|2\d|3[01])\b|%k[0-7]\b"),
    ),
}
ARCH_NAMES = {EM_X86_64: "x86_64", EM_AARCH64: "aarch64"}


LIBGCC_S = re.compile(r"libgcc_s(?:-[0-9a-f]{8,})?\.so(?:\.\d+)*$")
GCC_SME_HELPERS = {"__arm_tpidr2_save", "__arm_tpidr2_restore", "__arm_za_disable"}


def is_libgcc_runtime(path: Path, machine: int) -> bool:
    return machine == EM_AARCH64 and LIBGCC_S.fullmatch(path.name) is not None


def runtime_support_instruction(function: str, insn: str) -> bool:
    """Known optional-ISA support in AArch64 libgcc_s, never arbitrary compute.

    GCC's SME save/disable helpers test __aarch64_have_sme; restore is an SME
    ABI entry point called only with live ZA state. The unwinder evaluates
    CNTD only for the SVE vector-granule pseudo-register (DWARF register 46).
    Stripped libgcc leaves its internal copies labelled as offsets from
    _Unwind_GetTextRelBase, so allow only that exact query in this region.
    Sources: gcc/libgcc/config/aarch64/__arm_{tpidr2_save,tpidr2_restore,za_disable}.S
    and gcc/libgcc/config/aarch64/value-unwind.h (GCC 14).
    """
    name = function.split("@", 1)[0]
    return name in GCC_SME_HELPERS or (
        name in {"_Unwind_GetGR", "_Unwind_GetTextRelBase"}
        and re.fullmatch(r"cntd\s+x0\s*", insn) is not None
    )


def elf_machine(path: Path) -> int | None:
    """The ELF e_machine of a shared object or executable, or None for anything else."""
    try:
        with path.open("rb") as f:
            head = f.read(20)
    except OSError:
        return None
    if len(head) < 20 or head[:4] != b"\x7fELF":
        return None
    order = "little" if head[5] == 1 else "big"
    if int.from_bytes(head[16:18], order) not in (2, 3):  # ET_EXEC, ET_DYN
        return None
    return int.from_bytes(head[18:20], order)


def scan(path: Path, machine: int, objdump: str) -> tuple[int, collections.Counter, collections.Counter]:
    """Count disallowed and known runtime-support instructions per function."""
    _, pattern = CHECKS[machine]
    proc = subprocess.run(
        [objdump, "-d", "-C", "--no-show-raw-insn", str(path)],
        capture_output=True, text=True, errors="replace",
    )
    if proc.returncode != 0:
        raise RuntimeError(proc.stderr.strip() or f"{objdump} exited with {proc.returncode}")
    header = re.compile(r"^[0-9a-f]+ <(.*)>:$")
    instruction = re.compile(r"^\s*[0-9a-fA-F]+:\s+(.+)$")
    function = "?"
    per_function: collections.Counter = collections.Counter()
    allowed: collections.Counter = collections.Counter()
    runtime = is_libgcc_runtime(path, machine)
    for line in proc.stdout.splitlines():
        m = header.match(line)
        if m:
            function = m.group(1)
            continue
        m = instruction.match(line)
        if not m:
            continue
        # Keep the mnemonic and operands; drop symbol annotations and comments.
        insn = m.group(1).split("<", 1)[0].split("//", 1)[0].split("#", 1)[0]
        if pattern.search(insn):
            if runtime and runtime_support_instruction(function, insn.strip()):
                allowed[function] += 1
            else:
                per_function[function] += 1
    return sum(per_function.values()), per_function, allowed


def expand(inputs: list[Path], work_dir: Path) -> list[tuple[str, Path]]:
    """(label, file) for every file in the inputs; archives are unpacked under work_dir."""
    files: list[tuple[str, Path]] = []
    for item in inputs:
        if item.is_dir():
            files += [(str(p), p) for p in sorted(item.rglob("*")) if p.is_file()]
        elif zipfile.is_zipfile(item):
            dest = work_dir / item.name
            shutil.rmtree(dest, ignore_errors=True)
            with zipfile.ZipFile(item) as z:
                z.extractall(dest)
            files += [(f"{item.name}: {p.relative_to(dest)}", p)
                      for p in sorted(dest.rglob("*")) if p.is_file()]
        elif tarfile.is_tarfile(item):
            dest = work_dir / item.name
            shutil.rmtree(dest, ignore_errors=True)
            with tarfile.open(item) as t:
                if hasattr(tarfile, "data_filter"):  # Python 3.12+, and 3.8-3.11 security releases
                    t.extractall(dest, filter="data")
                else:
                    t.extractall(dest)
            files += [(f"{item.name}: {p.relative_to(dest)}", p)
                      for p in sorted(dest.rglob("*")) if p.is_file() and not p.is_symlink()]
        elif item.is_file():
            files.append((str(item), item))
        else:
            raise FileNotFoundError(item)
    return files


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("inputs", nargs="+", type=Path,
                        help="wheels, .tar.gz/.zip packages, directories or ELF files")
    parser.add_argument("--work-dir", type=Path, default=Path("build/isa-check"),
                        help="where archives are unpacked for review (default: build/isa-check)")
    parser.add_argument("--objdump", default=os.environ.get("OBJDUMP", "objdump"))
    args = parser.parse_args(argv)

    args.work_dir.mkdir(parents=True, exist_ok=True)
    cache: dict[tuple[str, bool], tuple[int, collections.Counter, collections.Counter]] = {}
    checked = failed = errors = 0
    for label, path in expand(args.inputs, args.work_dir):
        machine = elf_machine(path)
        if machine is None:
            continue
        if machine not in CHECKS:
            print(f"skip  {label}: ELF machine {machine} is not checked")
            continue
        digest = hashlib.sha256(path.read_bytes()).hexdigest()
        key = (digest, is_libgcc_runtime(path, machine))
        try:
            if key not in cache:
                cache[key] = scan(path, machine, args.objdump)
        except RuntimeError as exc:
            print(f"ERROR {label}: cannot disassemble ({exc}). Run on a {ARCH_NAMES[machine]} "
                  "host, or pass --objdump with a binutils that reads it.")
            errors += 1
            continue
        checked += 1
        count, per_function, allowed = cache[key]
        kind = CHECKS[machine][0]
        if allowed:
            print(f"note  {label}: {sum(allowed.values())} known GCC runtime-support "
                  f"{kind} instructions allowed in {len(allowed)} functions")
        if count:
            failed += 1
            examples = ", ".join(f[:80] for f, _ in per_function.most_common(3))
            print(f"FAIL  {label}: {count} {kind} instructions in {len(per_function)} "
                  f"functions (most in: {examples})")
        else:
            print(f"ok    {label}: no disallowed {kind} instructions")
    print(f"{checked} ELF files checked, {failed} with CPU-specific instructions, "
          f"{errors} could not be checked")
    if errors:
        return 2
    if checked == 0:
        print("ERROR no ELF files found in the inputs")
        return 2
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
