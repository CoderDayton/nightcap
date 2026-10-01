#!/usr/bin/env python3
# Copyright 2026 Mocktail Project Authors
# Licensed under the Apache License, Version 2.0.

"""Exercise the full legacy importer with synthetic APKs and failing renames.

APK/ELF inspection tools are fixtures; hashing, extraction, locking, metadata,
and filesystem operations use the real tools. No installed payload is touched.
"""

import json
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
import zipfile


ROOT = Path(__file__).resolve().parents[1]
COMPONENTS = ('libroblox.so', 'sober_apk', 'assets', 'roblox_payload.json')
OLD_FILES = {
    'libroblox.so': b'OLD-LIBRARY',
    'sober_apk/base.apk': b'OLD-APK',
    'sober_apk/split_config.x86_64.apk': b'OLD-SPLIT',
    'assets/content/data.bin': b'OLD-ASSET',
    'roblox_payload.json': b'{"old_metadata":true}\n',
}

TOOL = r'''
import json
import os
from pathlib import Path
import subprocess
import sys

tool = Path(sys.argv[0]).name
args = sys.argv[1:]
if tool == 'aapt':
    old = Path(args[-1]).read_bytes().startswith(b'OLD-')
    code, name = ('100', '1.0') if old else ('200', '2.0')
    split = " split='config.x86_64'" if 'split_' in args[-1] else ''
    print(f"package: name='com.roblox.client' versionCode='{code}' versionName='{name}'{split}")
elif tool == 'apksigner':
    if '--print-certs' in args:
        print('Signer #1 certificate SHA-256 digest: ' + 'a' * 64)
elif tool == 'file':
    print(args[-1] + ': ELF 64-bit for Android')
elif tool == 'readelf':
    if '-h' in args:
        print('  Machine: Advanced Micro Devices X86-64')
    elif '-n' in args:
        old = Path(args[-1]).read_bytes().startswith(b'OLD-')
        print('Build ID: ' + ('1' if old else '2') * 40)
    else:
        print('JNI_OnLoad')
        for name in ('nativeGameGlobalInit', 'nativeUpdateAdapterInit',
                     'nativeAppBridgeV2InitWithParams', 'nativeAppBridgeStartLuaAppDM',
                     'nativeAppBridgeV2StartAppWithParams',
                     'nativeAppBridgeV2UpdateSurfaceAppWithPlatformParams'):
            print('Java_com_roblox_engine_jni_NativeGLInterface_' + name)
elif tool == 'mv':
    source, destination = map(Path, args[-2:])
    phase = 'other'
    component = source.name
    if destination.parent.name == 'rollback':
        phase = 'backup'
    elif source.parent.name == 'new':
        phase = 'activate'
    elif destination.name.startswith('failed-'):
        phase = 'discard'
        component = 'roblox_payload.json' if destination.name == 'failed-metadata.json' else component
    elif source.parent.name == 'rollback':
        phase = 'restore'
    elif source.name == 'rollback' and destination.parent.name == 'versions':
        phase = 'archive'
    with open(os.environ['NIGHTCAP_TEST_MOVES'], 'a') as log:
        log.write(json.dumps([phase, component]) + '\n')
    failures = json.loads(os.environ['NIGHTCAP_TEST_FAILURES'])
    if phase == 'backup' and ['partial-backup', component] in failures:
        if source.is_dir():
            destination.mkdir()
            (destination / 'partial').write_bytes(b'INCOMPLETE-COPY')
        else:
            destination.write_bytes(b'INCOMPLETE-COPY')
        print(f'injected failure: partial-backup/{component}', file=sys.stderr)
        sys.exit(74)
    if [phase, component] in failures:
        print(f'injected failure: {phase}/{component}', file=sys.stderr)
        sys.exit(74)
    sys.exit(subprocess.call([os.environ['NIGHTCAP_TEST_REAL_MV'], *args]))
else:
    raise AssertionError(tool)
'''


class PayloadImportRollbackTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='nightcap-import-test-')
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.project = self.root / 'project with spaces'
        self.bin = self.root / 'bin'
        self.moves = self.root / 'moves.jsonl'
        for tool in ('bash', 'jq', 'unzip', 'flock', 'mv'):
            self.assertIsNotNone(shutil.which(tool), f'required test tool missing: {tool}')
        self.real_mv = shutil.which('mv')
        (self.project / 'scripts').mkdir(parents=True)
        (self.project / 'config').mkdir()
        self.bin.mkdir()
        for name in ('update_roblox_payload.sh', 'payload_integrity.sh'):
            shutil.copy2(ROOT / 'scripts' / name, self.project / 'scripts' / name)
        for name in ('aapt', 'apksigner', 'readelf', 'file', 'mv'):
            path = self.bin / name
            path.write_text(f'#!{sys.executable}\n' + TOOL)
            path.chmod(0o755)
        (self.project / 'config/roblox_signing_certificates.json').write_text(
            json.dumps({'schema_version': 1, 'package': 'com.roblox.client',
                        'trusted_sha256': ['a' * 64]})
        )
        self.base = self.root / 'base.apk'
        self.split = self.root / 'split_config.x86_64.apk'
        with zipfile.ZipFile(self.base, 'w') as archive:
            archive.writestr('assets/content/data.bin', b'NEW-ASSET')
        with zipfile.ZipFile(self.split, 'w') as archive:
            archive.writestr('lib/x86_64/libroblox.so', b'NEW-LIBRARY')

    def active_path(self, component):
        directory = 'config' if component == 'roblox_payload.json' else 'rbx_bin'
        return self.project / directory / component

    def create_previous(self, components=COMPONENTS):
        shutil.rmtree(self.project / 'rbx_bin', ignore_errors=True)
        self.active_path('roblox_payload.json').unlink(missing_ok=True)
        for relative, content in OLD_FILES.items():
            if relative.split('/')[0] not in components:
                continue
            path = self.active_path(relative.split('/')[0])
            if '/' in relative:
                path = path / relative.split('/', 1)[1]
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(content)

    def contents(self, path):
        if path.is_symlink():
            return ('symlink', os.readlink(path))
        if not path.exists():
            return None
        if path.is_file():
            return path.read_bytes()
        return {str(p.relative_to(path)): p.read_bytes()
                for p in path.rglob('*') if p.is_file()}

    def snapshot(self):
        return {name: self.contents(self.active_path(name)) for name in COMPONENTS}

    def run_import(self, failures=()):
        self.moves.unlink(missing_ok=True)
        env = {key: value for key, value in os.environ.items()
               if not key.startswith(('MOCKTAIL_', 'NIGHTCAP_TEST_'))}
        env.update(PATH=str(self.bin) + os.pathsep + env['PATH'],
                   NIGHTCAP_TEST_REAL_MV=self.real_mv,
                   NIGHTCAP_TEST_FAILURES=json.dumps(failures),
                   NIGHTCAP_TEST_MOVES=str(self.moves))
        return subprocess.run(
            ['bash', str(self.project / 'scripts/update_roblox_payload.sh'),
             '--base', str(self.base), '--x86-64', str(self.split)],
            env=env, capture_output=True, text=True, timeout=20,
        )

    def backups(self):
        return list((self.project / 'rbx_bin').glob('.payload-update.*'))

    def assert_failed_and_restored(self, result, before, phase, component):
        self.assertEqual(result.returncode, 74, result.stderr)
        self.assertIn(f'injected failure: {phase}/{component}', result.stderr)
        self.assertEqual(self.snapshot(), before)
        self.assertEqual(self.backups(), [])

    def test_backup_failures_preserve_every_previous_component(self):
        for component in COMPONENTS:
            with self.subTest(component=component):
                self.create_previous()
                before = self.snapshot()
                result = self.run_import([['backup', component]])
                self.assert_failed_and_restored(result, before, 'backup', component)

    def test_activation_failures_restore_previous_components(self):
        for component in COMPONENTS:
            with self.subTest(component=component):
                self.create_previous()
                before = self.snapshot()
                result = self.run_import([['activate', component]])
                self.assert_failed_and_restored(result, before, 'activate', component)

    def test_failed_first_install_removes_activated_components(self):
        for component in COMPONENTS:
            with self.subTest(component=component):
                self.create_previous(())
                before = self.snapshot()
                result = self.run_import([['activate', component]])
                self.assert_failed_and_restored(result, before, 'activate', component)

    def test_sparse_previous_install_is_restored(self):
        self.create_previous(('assets', 'roblox_payload.json'))
        before = self.snapshot()
        result = self.run_import([['activate', 'roblox_payload.json']])
        self.assert_failed_and_restored(result, before, 'activate', 'roblox_payload.json')

    def test_partial_backup_copy_does_not_replace_untouched_original(self):
        self.create_previous()
        before = self.snapshot()
        result = self.run_import([['partial-backup', 'sober_apk']])
        self.assert_failed_and_restored(result, before, 'partial-backup', 'sober_apk')

    def test_restore_failure_retains_backup_and_restores_other_components(self):
        for component in COMPONENTS:
            with self.subTest(component=component):
                self.create_previous()
                before = self.snapshot()
                result = self.run_import([['archive', 'rollback'], ['restore', component]])
                self.assertEqual(result.returncode, 74, result.stderr)
                self.assertIn(f'injected failure: restore/{component}', result.stderr)
                workspaces = self.backups()
                self.assertEqual(len(workspaces), 1)
                workspace = workspaces[0]
                self.assertIn(str(workspace), result.stderr)
                self.assertEqual(self.contents(workspace / 'rollback' / component), before[component])
                for other in COMPONENTS:
                    if other != component:
                        self.assertEqual(self.contents(self.active_path(other)), before[other])
                # A failed recovery is intentionally retained by the importer.
                shutil.rmtree(workspace)

    def test_discard_failure_does_not_move_backup_into_active_directory(self):
        for component in COMPONENTS:
            with self.subTest(component=component):
                self.create_previous()
                before = self.snapshot()
                result = self.run_import([['archive', 'rollback'], ['discard', component]])
                self.assertEqual(result.returncode, 74, result.stderr)
                self.assertIn(f'injected failure: discard/{component}', result.stderr)
                workspaces = self.backups()
                self.assertEqual(len(workspaces), 1)
                workspace = workspaces[0]
                self.assertIn(str(workspace), result.stderr)
                self.assertEqual(self.contents(workspace / 'rollback' / component), before[component])
                moves = [json.loads(line) for line in self.moves.read_text().splitlines()]
                self.assertNotIn(['restore', component], moves)
                for other in COMPONENTS:
                    if other != component:
                        self.assertEqual(self.contents(self.active_path(other)), before[other])
                shutil.rmtree(workspace)

    def test_successful_import_keeps_previous_archive_and_consistent_metadata(self):
        self.create_previous()
        before = self.snapshot()
        result = self.run_import()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.active_path('libroblox.so').read_bytes(), b'NEW-LIBRARY')
        self.assertEqual((self.active_path('assets') / 'content/data.bin').read_bytes(), b'NEW-ASSET')
        self.assertEqual((self.active_path('sober_apk') / 'base.apk').read_bytes(), self.base.read_bytes())
        metadata = json.loads(self.active_path('roblox_payload.json').read_text())
        self.assertEqual(metadata['version_code'], 200)
        self.assertEqual(metadata['elf_build_id'], '2' * 40)
        self.assertEqual(metadata['compatibility_status'], 'unverified')
        self.assertEqual(metadata['sha256']['libroblox'], hashlib.sha256(b'NEW-LIBRARY').hexdigest())
        self.assertEqual(metadata['sha256']['base_apk'], hashlib.sha256(self.base.read_bytes()).hexdigest())
        self.assertEqual(metadata['sha256']['x86_64_split_apk'], hashlib.sha256(self.split.read_bytes()).hexdigest())
        archive = self.project / 'rbx_bin/versions' / ('1.0-' + '1' * 40)
        for component in COMPONENTS:
            self.assertEqual(self.contents(archive / component), before[component])
        self.assertEqual(self.backups(), [])

    def test_successful_first_install_has_no_previous_archive(self):
        result = self.run_import()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertTrue(all(self.active_path(name).exists() for name in COMPONENTS))
        self.assertEqual(list((self.project / 'rbx_bin/versions').iterdir()), [])
        self.assertEqual(self.backups(), [])

    def test_successful_sparse_install_archives_unversioned_components(self):
        self.create_previous(('assets', 'roblox_payload.json'))
        before = self.snapshot()
        result = self.run_import()
        self.assertEqual(result.returncode, 0, result.stderr)
        archives = list((self.project / 'rbx_bin/versions').glob('unknown-*'))
        self.assertEqual(len(archives), 1)
        for component in COMPONENTS:
            self.assertEqual(self.contents(archives[0] / component), before[component])
        self.assertTrue(all(self.active_path(name).exists() for name in COMPONENTS))
        self.assertEqual(self.backups(), [])

    def test_successful_import_archives_dangling_asset_symlink(self):
        self.create_previous()
        shutil.rmtree(self.active_path('assets'))
        self.active_path('assets').symlink_to('missing-assets')
        result = self.run_import()
        self.assertEqual(result.returncode, 0, result.stderr)
        archive = self.project / 'rbx_bin/versions' / ('1.0-' + '1' * 40)
        self.assertEqual(self.contents(archive / 'assets'), ('symlink', 'missing-assets'))
        self.assertEqual((self.active_path('assets') / 'content/data.bin').read_bytes(), b'NEW-ASSET')
        self.assertEqual(self.backups(), [])


if __name__ == '__main__':
    unittest.main()
