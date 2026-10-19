"""An ABS constant must not hide a same-named local object's relocation."""
import importlib.util
from pathlib import Path
import sys
import unittest
from unittest.mock import patch

SCRIPTS = Path(__file__).resolve().parents[2] / "3ds_port/scripts"
sys.path.insert(0, str(SCRIPTS))
spec = importlib.util.spec_from_file_location("gamedata_relocations", SCRIPTS / "gen_gamedata_bundle.py")
bundle = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bundle)


class RelocationTests(unittest.TestCase):
    def test_duplicate_local_names_and_separate_symbol_tables(self):
        symbols = """Symbol table '.dynsym' contains 3 entries:
     1: 00000000 0 NOTYPE LOCAL DEFAULT ABS unrelated
Symbol table '.symtab' contains 3 entries:
     1: 00800000 1024 OBJECT LOCAL DEFAULT 22 REG_BASE
     2: 04000000 0 NOTYPE LOCAL DEFAULT ABS REG_BASE
"""
        relocs = """Relocation section '.rel.gamedata' at offset 0:
00400000 00000102 R_ARM_ABS32 00800000 REG_BASE
00400004 00000202 R_ARM_ABS32 04000000 REG_BASE
Relocation section '.rel.text' at offset 0:
00000100 00000102 R_ARM_ABS32 00800000 REG_BASE
"""
        with patch.object(bundle.subprocess, "check_output", side_effect=[relocs, symbols]):
            self.assertEqual(bundle.relocations(Path("fixture.elf"), ".gamedata"),
                             [(0x400000, "R_ARM_ABS32")])


if __name__ == "__main__":
    unittest.main()
