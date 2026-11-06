"""The probe supervisor must distinguish missing features from crashed tools."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

WRAPPER = Path(os.environ.get('QFUSE_TEST_COMPILER_WRAPPER', str(Path(__file__).resolve().parents[1] / 'scripts/qnx-compiler.py')))


class CompilerSupervisorTest(unittest.TestCase):
    def run_fake(self, failures, crash=True, message=None):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            counter, marker, compiler = root/'counter', root/'failure', root/'compiler.py'
            compiler.write_text(
                'from pathlib import Path\nimport sys\n'
                f'p=Path({str(counter)!r})\nn=int(p.read_text()) if p.exists() else 0\np.write_text(str(n+1))\n'
                f'if n < {failures}:\n'
                f' print({(message or ("cc1 caught signal 11" if crash else "error: size of array is negative"))!r}, file=sys.stderr)\n sys.exit(1)\n'
                'print("compiler result")\n'
            )
            env = dict(os.environ, QFUSE_COMPILER_FAILURE_FILE=str(marker))
            result = subprocess.run([sys.executable, str(WRAPPER), sys.executable, str(compiler)], env=env, capture_output=True, text=True)
            return result, int(counter.read_text()), marker.exists()

    def test_crash_is_recovered_before_a_probe_result_is_returned(self):
        result, attempts, marker = self.run_fake(1)
        self.assertEqual((result.returncode, attempts, marker), (0, 2, False))
        self.assertEqual(result.stdout, 'compiler result\n')

    def test_real_compile_error_is_not_retried_or_reclassified(self):
        result, attempts, marker = self.run_fake(99, crash=False)
        self.assertEqual((result.returncode, attempts, marker), (1, 1, False))
        self.assertIn('size of array is negative', result.stderr)

    def test_driver_reported_collect2_crash_is_also_recovered(self):
        result, attempts, marker = self.run_fake(1, message='ntox86_64-gcc: internal compiler error: Segmentation fault signal terminated program collect2')
        self.assertEqual((result.returncode, attempts, marker), (0, 2, False))

    def test_colored_driver_crash_is_recognized(self):
        result, attempts, marker = self.run_fake(1, message='gcc: \x1b[01;31minternal compiler error: \x1b[0mSegmentation fault signal terminated program as')
        self.assertEqual((result.returncode, attempts, marker), (0, 2, False))

    def test_exhausted_crashes_invalidate_the_entire_configuration(self):
        result, attempts, marker = self.run_fake(99)
        self.assertEqual((result.returncode, attempts, marker), (99, 5, True))

    def test_missing_compiler_invalidates_configuration(self):
        with tempfile.TemporaryDirectory() as directory:
            marker = Path(directory)/'failure'
            result = subprocess.run([sys.executable, str(WRAPPER), str(Path(directory)/'missing-compiler')],
                env=dict(os.environ, QFUSE_COMPILER_FAILURE_FILE=str(marker)), capture_output=True, text=True)
            self.assertEqual(result.returncode, 99)
            self.assertTrue(marker.exists())


if __name__ == '__main__':
    unittest.main()
