import contextlib
import io
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest import mock
import zipfile

import make_release


class ReleasePackTests(unittest.TestCase):
    def setUp(self):
        contexts = contextlib.ExitStack()
        self.addCleanup(contexts.close)
        self.root = Path(contexts.enter_context(tempfile.TemporaryDirectory()))
        self.dist = self.root / 'dist'
        contexts.enter_context(mock.patch.object(make_release, 'ROOT', str(self.root)))
        contexts.enter_context(mock.patch.object(make_release, 'DIST', str(self.dist)))
        contexts.enter_context(mock.patch.object(make_release, 'git', return_value='test-commit'))
        contexts.enter_context(contextlib.redirect_stdout(io.StringIO()))
        for source in make_release.FILES.values():
            path = self.root / source
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(source, encoding='utf-8')
        self.cps = self.root / make_release.CPS_FILE
        self.cps.write_text('test CPS', encoding='utf-8')

    def pack(self):
        return make_release.pack('test', str(self.cps), 'test CPS version')

    def snapshot(self):
        return {path.name: path.read_bytes() for path in self.dist.iterdir()}

    def test_release_checksums_match_the_published_assets(self):
        zip_path, cps_path, timezone_path, sums_path = map(Path, self.pack())
        with zipfile.ZipFile(zip_path) as archive:
            self.assertIsNone(archive.testzip())
            self.assertIn('OpenDMR-test/opendmr_BFUV32.bin', archive.namelist())
        with zipfile.ZipFile(timezone_path) as archive:
            self.assertCountEqual(archive.namelist(), ['tzdb.bin', 'README.md'])
        expected = ['%s  %s' % (make_release.sha256(path), path.name)
                    for path in (zip_path, cps_path, timezone_path)]
        self.assertEqual(sums_path.read_text().splitlines(), expected)

    def test_missing_firmware_keeps_the_previous_release(self):
        self.pack()
        previous = self.snapshot()
        (self.root / make_release.FILES['opendmr_BFUV32.bin']).unlink()
        with self.assertRaisesRegex(SystemExit, 'missing: .*opendmr_BFUV32.bin'):
            self.pack()
        self.assertEqual(self.snapshot(), previous)

    def test_checksum_failure_keeps_the_previous_release(self):
        self.pack()
        previous = self.snapshot()
        self.cps.write_text('updated CPS', encoding='utf-8')
        with mock.patch.object(make_release, 'sha256', side_effect=OSError('checksum read failed')):
            with self.assertRaisesRegex(OSError, 'checksum read failed'):
                self.pack()
        self.assertEqual(self.snapshot(), previous)


class FirmwareBuildTests(unittest.TestCase):
    """build() must not relink the objects of an instrumented build: src/Makefile gates
    -DRX_CAPTURE, -DOPENDMR_DEBUG and -DOPENDMR_DELAY_LOG on make variables, its -MMD
    dependencies do not follow CFLAGS, and make reads those variables from the environment
    this function passes through."""

    def setUp(self):
        contexts = contextlib.ExitStack()
        self.addCleanup(contexts.close)
        self.calls = []

        def run(command, **kwargs):
            self.calls.append((list(command), kwargs.get('cwd'), kwargs))
            return subprocess.CompletedProcess(command, 0, 'the build said something\n', '')

        contexts.enter_context(mock.patch.object(make_release.subprocess, 'run', side_effect=run))
        contexts.enter_context(mock.patch.object(make_release.shutil, 'which',
                                                 side_effect=lambda name, path=None: '/usr/bin/' + name))
        contexts.enter_context(mock.patch.object(make_release, 'git', return_value=''))
        contexts.enter_context(mock.patch.dict(os.environ, {'RX_CAPTURE': '1', 'DEBUG': '1', 'DELAY_LOG': '1'}))
        contexts.enter_context(contextlib.redirect_stdout(io.StringIO()))

    def makes(self):
        src = os.path.join(make_release.ROOT, 'src')
        return [command for command, cwd, _ in self.calls if cwd == src]

    def test_the_firmware_is_built_from_clean(self):
        make_release.build()
        commands = self.makes()
        self.assertTrue(commands, 'build() ran no make in src/')
        self.assertIn('clean', commands[0], 'the first make in src/ must be the clean')

    def test_a_failed_clean_stops_the_release(self):
        def run(command, **kwargs):
            self.calls.append((list(command), kwargs.get('cwd'), kwargs))
            return subprocess.CompletedProcess(command, 1, 'cannot remove build\n', '')

        with mock.patch.object(make_release.subprocess, 'run', side_effect=run):
            with self.assertRaises(SystemExit):
                make_release.build()
        self.assertEqual(len(self.makes()), 1, 'the firmware was built although the clean failed')

    def test_the_instrumentation_variables_are_named_on_the_make_command_line(self):
        make_release.build()
        firmware = [command for command in self.makes() if 'clean' not in command]
        self.assertTrue(firmware, 'build() ran no firmware make in src/')
        # Command-line assignments override the environment, which an exported RX_CAPTURE=1
        # (set in setUp) otherwise instruments even a clean build
        self.assertLessEqual({'RX_CAPTURE=', 'DEBUG=', 'DELAY_LOG=', 'DEBUG_KEEP='}, set(firmware[-1]))


if __name__ == '__main__':
    unittest.main()
