"""Tag versioning used by native and Android release builds (no SDK required)."""
import argparse
import importlib.util
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location('thor_dev', Path(__file__).parents[1] / 'thor_dev.py')
thor = importlib.util.module_from_spec(spec)
spec.loader.exec_module(thor)


class ReleaseVersionTest(unittest.TestCase):
    def test_display_and_upgrade_codes(self):
        self.assertEqual(thor.release_version('v0.4.1')[0], '0.4.1')
        versions = ['0.4.0', '0.4.1-beta.1', '0.4.1', '0.5.0', '1.0.0']
        codes = [thor.release_version(v)[1] for v in versions]
        self.assertGreater(codes[0], 400)  # Existing Thor APKs use code 400.
        self.assertEqual(sorted(set(codes)), codes)

    def test_invalid_and_overflowing_tags(self):
        for value in ['main', 'v1.2', 'v01.2.3', 'v1.100.0', 'v1.0.1000', 'v211.0.0', 'v1.2.3;echo bad']:
            with self.subTest(value=value), self.assertRaises(argparse.ArgumentTypeError):
                thor.release_version(value)


if __name__ == '__main__':
    unittest.main()
