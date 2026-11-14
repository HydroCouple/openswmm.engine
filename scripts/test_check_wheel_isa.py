"""Regression checks for the instruction gate's GNU and LLVM disassembly readers."""
from pathlib import Path
import contextlib
import io
import subprocess
import unittest
from unittest.mock import patch

import check_wheel_isa as isa


class WheelIsaTests(unittest.TestCase):
    def scan(self, machine, instruction, separator="\t", *, library="probe.so", function="probe"):
        output = f"0000000000000000 <{function}>:\n       0:{separator}{instruction}\n"
        with patch.object(isa.subprocess, "run", return_value=subprocess.CompletedProcess(
            [], 0, stdout=output, stderr=""
        )):
            return isa.scan(Path(library), machine, "objdump")

    def test_sve_and_sme_in_both_disassembly_formats(self):
        for separator in ("\t", "      \t"):
            for instruction in ("ptrue p0.b", "fadd z0.s, z1.s, z2.s", "smstart za"):
                with self.subTest(separator=separator, instruction=instruction):
                    count, functions, _ = self.scan(isa.EM_AARCH64, instruction, separator)
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

    def test_libgcc_sme_abi_helpers_are_allowed(self):
        for library in ("libgcc_s.so.1", "libgcc_s-2d945d6c.so.1"):
            for function in ("__arm_tpidr2_save", "__arm_tpidr2_restore", "__arm_za_disable"):
                for separator in ("\t", "      \t"):
                    with self.subTest(library=library, function=function, separator=separator):
                        result = self.scan(isa.EM_AARCH64, "smstop za", separator,
                                           library=library, function=function + "@@GCC_14.0")
                        self.assertEqual(result[0], 0)
                        self.assertEqual(sum(result[2].values()), 1)

    def test_libgcc_unwind_vector_length_queries_are_allowed(self):
        for function in ("_Unwind_GetGR", "_Unwind_GetTextRelBase"):
            result = self.scan(isa.EM_AARCH64, "cntd x0", library="libgcc_s.so.1",
                               function=function + "@@GCC_3.0")
            self.assertEqual(result[0], 0)
            self.assertEqual(sum(result[2].values()), 1)

    def test_runtime_exceptions_do_not_hide_other_cpu_requirements(self):
        cases = [
            ("probe.so", "__arm_tpidr2_save", "smstop za"),
            ("libopenswmm.engine.so", "_Unwind_GetGR", "cntd x0"),
            ("libgomp.so.1", "__arm_tpidr2_restore", "smstop za"),
            ("libgcc_s.so.1", "engine_flux", "ptrue p0.b"),
            ("libgcc_s.so.1", "__arm_tpidr2_save_extra", "smstop za"),
            ("libgcc_s.so.1", "_Unwind_GetGR", "fadd z0.s, z1.s, z2.s"),
            ("libgcc_s.so.1", "_Unwind_GetTextRelBase", "ptrue p0.b"),
        ]
        for library, function, instruction in cases:
            with self.subTest(library=library, function=function, instruction=instruction):
                self.assertEqual(self.scan(isa.EM_AARCH64, instruction, library=library,
                                           function=function)[0], 1)
        self.assertEqual(self.scan(isa.EM_X86_64, "vaddps %zmm0, %zmm1, %zmm2",
                                   library="libgcc_s.so.1", function="__arm_tpidr2_save")[0], 1)

    def test_identical_bytes_in_an_engine_library_are_not_cached_as_runtime(self):
        work = Path(__file__).resolve().parents[1] / "build" / "wheel-isa-tests"
        work.mkdir(parents=True, exist_ok=True)
        header = bytearray(32)
        header[:6] = b"\x7fELF\x02\x01"
        header[16:18] = (3).to_bytes(2, "little")
        header[18:20] = isa.EM_AARCH64.to_bytes(2, "little")
        runtime, engine = work / "libgcc_s.so.1", work / "libopenswmm.engine.so"
        runtime.write_bytes(header)
        engine.write_bytes(header)
        output = "0000000000000000 <__arm_tpidr2_save@@GCC_14.0>:\n  0: smstop za\n"
        stdout = io.StringIO()
        with patch.object(isa.subprocess, "run", return_value=subprocess.CompletedProcess(
            [], 0, stdout=output, stderr=""
        )), contextlib.redirect_stdout(stdout):
            self.assertEqual(isa.main([str(runtime), str(engine), "--work-dir", str(work)]), 1)
        self.assertIn("1 with CPU-specific instructions", stdout.getvalue())
        self.assertIn("FAIL  " + str(engine), stdout.getvalue())

    def test_disassembly_failure_refuses_the_file(self):
        with patch.object(isa.subprocess, "run", return_value=subprocess.CompletedProcess(
            [], 1, stdout="", stderr="unsupported architecture"
        )):
            with self.assertRaisesRegex(RuntimeError, "unsupported architecture"):
                isa.scan(Path("probe.so"), isa.EM_AARCH64, "objdump")


if __name__ == "__main__":
    unittest.main()
