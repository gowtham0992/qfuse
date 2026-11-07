"""Native QNX make configuration must preserve private staging across sub-makes."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
PORT = ROOT/'integration/qnx-ports/qfuse'

class NativeMakeStaging(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='qfuse-make.')
        self.addCleanup(self.temp.cleanup)
        self.folder = Path(self.temp.name)
        # Model the SDK's default assignment, not its proprietary build rules.
        (self.folder/'qconfig.mk').write_text('INSTALL_ROOT_nto := /pretend-sdk/target/qnx\nPREFIX := /sdk-default\n')
        (self.folder/'recurse.mk').write_text('all:\n\t@$(MAKE) -s -f "$(QFUSE_TEST_PROBE)" QCONFIG="$(QFUSE_TEST_CONFIG)" probe\n')
        (self.folder/'probe.mk').write_text('include '+str(PORT/'common.mk')+'\nprobe:\n\t@printf "stage=%s\\nprefix=%s\\n" "$(INSTALL_ROOT_nto)" "$(PREFIX)"\n')
        self.env = dict(os.environ)
        for key in ('CPU','MAKEFLAGS','MFLAGS','MAKEOVERRIDES','QNX_TARGET','INSTALL_ROOT_nto','PREFIX','QFUSE_NATIVE_STAGE','QFUSE_NATIVE_PREFIX','QFUSE_PORT_DIR'):
            self.env.pop(key,None)
        self.env.update(QFUSE_TEST_PROBE=str(self.folder/'probe.mk'),QFUSE_TEST_CONFIG=str(self.folder/'qconfig.mk'))

    def run_make(self, args=(), extra=None):
        env = dict(self.env, **(extra or {}))
        return subprocess.run(['make','-s','-C',str(PORT),'-I',str(self.folder),*args,'all'],env=env,capture_output=True,text=True)

    def test_environment_stage_survives_sdk_defaults(self):
        stage=str(self.folder/'private-stage')
        result=self.run_make(extra={'INSTALL_ROOT_nto':stage,'PREFIX':'/opt/qfuse'})
        self.assertEqual(result.returncode,0,result.stderr)
        self.assertIn('stage='+stage,result.stdout)
        self.assertIn('prefix=/opt/qfuse',result.stdout)

    def test_command_line_stage_survives_sdk_defaults(self):
        stage=str(self.folder/'command-stage')
        result=self.run_make(['INSTALL_ROOT_nto='+stage,'PREFIX=/opt/qfuse'])
        self.assertEqual(result.returncode,0,result.stderr)
        self.assertIn('stage='+stage,result.stdout)
        self.assertIn('prefix=/opt/qfuse',result.stdout)

    def test_default_stage_stays_in_recipe(self):
        result=self.run_make()
        self.assertEqual(result.returncode,0,result.stderr)
        self.assertIn('stage='+str(PORT/'stage'),result.stdout)
        self.assertIn('prefix=/usr/local',result.stdout)

    def test_explicit_sdk_stage_rejected(self):
        result=self.run_make(['INSTALL_ROOT_nto=/pretend-sdk/target/qnx'],{'QNX_TARGET':'/pretend-sdk/target/qnx'})
        self.assertNotEqual(result.returncode,0)
        self.assertIn('outside the licensed SDP',result.stderr)

if __name__=='__main__':
    unittest.main()
