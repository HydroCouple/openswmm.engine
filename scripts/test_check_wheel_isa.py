"""Regression checks for the instruction gate's GNU and LLVM disassembly readers."""
from pathlib import Path
import subprocess
import unittest
from unittest.mock import patch

import check_wheel_isa as isa


class WheelIsaTests(unittest.TestCase):
    def scan(self, machine, instruction, separator="\t"):
        output = f"0000000000000000 <probe>:\n       0:{separator}{instruction}\n"
        with patch.object(isa.subprocess, "run", return_value=subprocess.CompletedProcess(
            [], 0, stdout=output, stderr=""
        )):
            return isa.scan(Path("probe.so"), machine, "objdump")

    def test_sve_and_sme_in_both_disassembly_formats(self):
        for separator in ("\t", "      \t"):
            for instruction in ("ptrue p0.b", "fadd z0.s, z1.s, z2.s", "smstart za"):
                with self.subTest(separator=separator, instruction=instruction):
                    count, functions = self.scan(isa.EM_AARCH64, instruction, separator)
                    self.assertEqual(count, 1)
                    self.assertEqual(functions["probe"], 1)

    def test_avx512_in_both_disassembly_formats(self):
        for separator in ("\t", "      \t"):
            for instruction in ("vaddps %zmm0, %zmm1, %zmm2", "kmovw %eax, %k1",
                                "vaddps %xmm16, %xmm17, %xmm18"):
                with self.subTest(separator=separator, instruction=instruction):
                    self.assertEqual(self.scan(isa.EM_X86_64, instruction, separator)[0], 1)

    def test_neon_and_avx2_are_allowed(self):
        self.assertEqual(self.scan(isa.EM_AARCH64, "add v0.4s, v0.4s, v1.4s")[0], 0)
        self.assertEqual(self.scan(isa.EM_X86_64, "vaddps %ymm0, %ymm1, %ymm2")[0], 0)

    def test_symbol_names_and_comments_are_not_instructions(self):
        self.assertEqual(self.scan(isa.EM_AARCH64, "bl 10 <ptrue>")[0], 0)
        self.assertEqual(self.scan(isa.EM_AARCH64, "ret // ptrue p0.b")[0], 0)
        self.assertEqual(self.scan(isa.EM_X86_64, "ret # %zmm0")[0], 0)

    def test_disassembly_failure_refuses_the_file(self):
        with patch.object(isa.subprocess, "run", return_value=subprocess.CompletedProcess(
            [], 1, stdout="", stderr="unsupported architecture"
        )):
            with self.assertRaisesRegex(RuntimeError, "unsupported architecture"):
                isa.scan(Path("probe.so"), isa.EM_AARCH64, "objdump")


if __name__ == "__main__":
    unittest.main()
