"""Run the real cut-record generator on a tiny terrace, without cartridge art."""
import ast
import collections
from pathlib import Path
from types import SimpleNamespace
import unittest

SOURCE = Path(__file__).resolve().parents[1] / 'scripts/voxel_terraces.py'


class TerraceFootTest(unittest.TestCase):
    def test_cut_flank_meets_lower_terrace(self):
        # Route 114's flank: its lowest rock is at 48px, but the top north
        # of it is at 32px. Its clear background must reach that lower top.
        top, flank = (0, 0), (0, 1)
        mask = [0xff] * 16
        art = SimpleNamespace(w=1, h=2, metatile=lambda x, y: 0x71 if y == 0 else 0x75)
        cells = SimpleNamespace(
            kind={top: 'T', flank: 'R'}, members=[('test', 0, 0)],
            src={top: ('test', 0, 0), flank: ('test', 0, 1)},
            arts={'test': art}, roles_of={'test': None}, rim=set(), bridges=set(),
            cheeks=set(), band={}, keep=set(),
            is_stair=lambda cell: False, muddy=lambda cell: False,
            metatile=lambda cell: art.metatile(*cell), role=lambda cell: 'cliff',
        )
        sol = {'cells': cells, 'grids': {top: [[32]*5 for _ in range(5)],
                                       flank: [[64-4*i for i in range(5)] for _ in range(5)]},
               'flat': {top: 32}}
        gr = SimpleNamespace(
            _cut_mask=lambda art, roles, x, y, rock: mask if y else None,
            _behind=lambda *args, **kwargs: 1,
            _plain_background=lambda *args: True,
        )
        env = dict(collections=collections, GROUND='G', TOP='T', ROCK='R', UNKNOWN='?', N=4,
                   BLOCK_WALLS=4, PLAIN=1, GOES_ON=2, THIN_DECK=8, LEVEL=16,
                   CUT_LIFT=2, FACES=[0x7c], gr=gr,
                   low_side=lambda *args: None, is_face=lambda *args: False,
                   straight_mask=lambda mask: mask,
                   _sample=lambda grid, x, y: 56,
                   behind=lambda *args: 1,
                   edge=lambda g, side: g[0] if side == 'N' else g[-1] if side == 'S'
                   else [row[0 if side == 'W' else -1] for row in g])
        tree = ast.parse(SOURCE.read_text())
        node = next(n for n in tree.body if isinstance(n, ast.FunctionDef) and n.name == 'records')
        exec(compile(ast.Module(body=[node], type_ignores=[]), str(SOURCE), 'exec'), env)
        rec = env['records'](sol)[flank]
        self.assertEqual(rec[1], 32, 'cut background must meet the lower terrace')
        # No nearby flat ground: retain the rock's original foot.
        sol['flat'].clear()
        self.assertEqual(env['records'](sol)[flank][1], 48)


if __name__ == '__main__':
    unittest.main()
