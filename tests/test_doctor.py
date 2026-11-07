"""The prerequisite checker must support SDK hosts without the optional lab."""
import contextlib
import io
from pathlib import Path
import runpy
import subprocess
import sys
import unittest
from unittest.mock import patch

SCRIPT = Path(__file__).resolve().parents[1]/'scripts/doctor.py'

class DoctorProfiles(unittest.TestCase):
    def run_doctor(self, args, missing=()):
        output = io.StringIO()
        def which(command):
            return None if command in missing else '/tools/'+command
        with patch.object(sys, 'argv', [str(SCRIPT), *args]), \
             patch('platform.system', return_value='Linux'), \
             patch('platform.machine', return_value='x86_64'), \
             patch('pathlib.Path.is_file', return_value=True), \
             patch('shutil.which', side_effect=which), \
             patch('subprocess.run', return_value=subprocess.CompletedProcess([], 0, 'linux\n', '')) as run, \
             contextlib.redirect_stdout(output), contextlib.redirect_stderr(output):
            try:
                runpy.run_path(str(SCRIPT), run_name='__main__')
                code = 0
            except SystemExit as error:
                code = error.code
        return code, output.getvalue(), run

    def test_sdk_host_needs_no_docker_or_qemu(self):
        code, text, run = self.run_doctor([], {'docker', 'qemu-system-x86_64', 'qemu-system-aarch64'})
        self.assertEqual(code, 0, text)
        run.assert_not_called()

    def test_host_checks_need_no_sdk_or_lab(self):
        code, text, run = self.run_doctor(['--mode', 'host'])
        self.assertEqual(code, 0, text)
        self.assertNotIn('QNX Linux host compiler', text)
        run.assert_not_called()

    def test_arm_lab_checks_arm_emulator(self):
        code, text, _ = self.run_doctor(['--mode', 'lab', '--arch', 'aarch64le'], {'qemu-system-aarch64'})
        self.assertEqual(code, 1, text)
        self.assertIn('qemu-system-aarch64: missing', text)
        self.assertNotIn('qemu-system-x86_64:', text)

    def test_missing_core_build_tool_fails(self):
        code, text, _ = self.run_doctor([], {'make'})
        self.assertEqual(code, 1, text)
        self.assertIn('make: missing', text)

if __name__ == '__main__':
    unittest.main()
