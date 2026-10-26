from pathlib import Path
import importlib.util
import plistlib
import struct
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


def load(name):
    spec = importlib.util.spec_from_file_location(name, ROOT / 'tools' / (name + '.py'))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


seed = load('umi_startup_seed')
policy = load('make_root_volume_policy')


class BootstrapTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name).resolve()
        p = self.root / 'System/Library/CoreServices'
        p.mkdir(parents=True)
        (p / 'SystemVersion.plist').write_bytes(plistlib.dumps(
            {'ProductVersion': '16.5', 'ProductBuildVersion': '20F66'}))

    def tearDown(self):
        self.temp.cleanup()

    def test_fresh_seed_plan(self):
        plan = seed.make_plan(self.root)
        self.assertEqual(len(plan['files']), 4)
        self.assertEqual(len(plan['directories']), 4)
        self.assertFalse(plan['device_key_material_copied'])
        self.assertEqual(plistlib.loads(seed.placeholder()), {'KeyBagKeys': b'SWEET'})

    def test_unknown_existing_keybag_refused(self):
        p = self.root / seed.KEYBAG.lstrip('/')
        p.parent.mkdir(parents=True)
        p.write_bytes(b'unknown state')
        with self.assertRaises(ValueError):
            seed.make_plan(self.root)

    def test_exact_existing_placeholder_preserved(self):
        p = self.root / seed.KEYBAG.lstrip('/')
        p.parent.mkdir(parents=True)
        p.write_bytes(seed.placeholder())
        self.assertTrue(seed.make_plan(self.root)['files'][-1]['already_exact'])

    def test_final_directory_symlink_refused(self):
        p = self.root / 'private/var'
        p.mkdir(parents=True)
        (p / 'other').mkdir()
        (p / 'keybags').symlink_to('/private/var/other')
        with self.assertRaises(ValueError):
            seed.make_plan(self.root)

    def test_wrong_os_refused(self):
        with self.assertRaises(ValueError):
            seed.make_plan(self.root, '16.7.16', '20H392')

    def test_no_host_absolute_symlink_resolution(self):
        p = self.root / 'var'
        p.symlink_to('/private/var')
        self.assertEqual(seed.guest(self.root, '/var/keybags'), self.root / 'private/var/keybags')
        with self.assertRaises(ValueError):
            seed.guest(self.root, '../../etc/passwd')


class VolumePolicyTests(unittest.TestCase):
    def header(self, identity=b'12345678'):
        h = bytearray(512)
        h[:4] = b'HX\x00\x05'
        struct.pack_into('>II', h, 40, 4096, 4194304)
        h[104:112] = identity
        return bytes(h)

    def test_only_policy_constant_changes(self):
        template = 'before\nstatic const uint8_t id[8]={0,0,0,0,0,0,0,0};\nafter\n'
        result = policy.render(template, self.header(), self.header())
        self.assertEqual(result, 'before\nstatic const uint8_t id[8]={0x31,0x32,0x33,0x34,0x35,0x36,0x37,0x38};\nafter\n')

    def test_wrong_alternate_rejected(self):
        with self.assertRaises(ValueError):
            policy.render('static const uint8_t id[8]={0};', self.header(), self.header(b'87654321'))

    def test_bad_geometry_and_truncation_rejected(self):
        for offset, value in ((0, b'H+'), (2, b'\x00\x04'), (40, struct.pack('>I', 512)),
                              (44, struct.pack('>I', 4194303)), (104, bytes(8))):
            h = bytearray(self.header())
            h[offset:offset + len(value)] = value
            with self.assertRaises(ValueError):
                policy.check_header(h)
        with self.assertRaises(ValueError):
            policy.check_header(self.header()[:511])

    def test_ambiguous_template_refused(self):
        for template in ('no policy', 'static const uint8_t id[8]={0};\n' * 2):
            with self.assertRaises(ValueError):
                policy.render(template, self.header(), self.header())


if __name__ == '__main__':
    unittest.main()
