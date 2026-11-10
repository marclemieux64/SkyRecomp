"""Function-boundary regressions; no game files needed: python tools/test_gen.py."""
import unittest
import shutil
import subprocess
import tempfile
from pathlib import Path
from collections import Counter

from gen import Module, SKY_BUILDS, sky_stores_for
from lifter import Lifter
from patches import (BEETLE_TIMER_SITE, DAMAGE_SHIFT_SITE, DASH_CAP_SITE, DASH_START_SITE, DASH_USE_SITE,
                     DASH_GAUGE_SITE, DROP_END_SITE, DROP_KIND_SITES, DROP_THIN_SITE, FLIGHT_SPEED_SITE,
                     GAUGE_PICTURE_SITES, HERO_UNLOCK_SITE, WALK_SPEED_SITE,
                     SKY_PATCHES, STAMINA_STORE_SITE, TREASURE_ROLL_SITES)


class JumpTableBoundaryTest(unittest.TestCase):
    @unittest.skipUnless(shutil.which('clang'), 'clang is required for emitted-C validation')
    def test_interior_generated_c_compiles(self):
        m, _ = self.module(9)
        m.words = [0xd503201f, 0xd65f03c0]
        m.hi = m.lo + 8
        m.is_las = m.is_skyward = False
        m.interior = True
        code = '#include "cpu.h"\n' + '\n'.join(m.emit_function(m.lo, m.hi, {m.lo}, {}, {}))
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / 'interior.c'
            source.write_text(code)
            result = subprocess.run(['clang', '-fsyntax-only', '-Werror=implicit-function-declaration',
                                     '-I' + str(Path(__file__).resolve().parents[1] / 'runtime'), str(source)],
                                    capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)

    def test_interior_entry_preserves_optimized_normal_body(self):
        m, _ = self.module(9)
        m.interior = True
        m.is_las = m.is_skyward = False
        code = '\n'.join(m.emit_function(m.lo, m.hi, set(), {}, {}))
        self.assertIn('c->pc = c->x[31]; c->x[31] = 0;', code)
        self.assertIn('return f_1000_any(c);', code)
        self.assertEqual(code.count('L_dispatch:'), 1)
        self.assertIn('cold, noinline, optnone', code)

    def test_out_of_line_jump_table_adds_external_boundary(self):
        m, target = self.module(9)
        m.words = [0xd61f0160, 0xd65f03c0, 0xd65f03c0]  # br x11; ret; ret
        m.hi = m.lo + 12
        m.jump_table = lambda *args: [m.lo + 4, m.lo + 8]
        labels, external, tables = m.scan(m.lo, m.lo + 8)
        self.assertIn(m.lo + 8, external)
        self.assertIn(m.lo + 4, labels)
        self.assertEqual(tables[m.lo], 'all')

    def test_relocated_vanilla_cheat_and_branch_labels(self):
        base = 0x7100004000
        delta = base - 0x7000000000
        for old, word, expected in [(STAMINA_STORE_SITE, 0xb90002e8, 'rt_cheat_stamina'),
                                    (DROP_THIN_SITE, 0x14000028, f'L_{0x7000b90844 + delta:x}')]:
            m, _ = self.module(9)
            m.base = base
            m.lo = old + delta
            m.words = [word, 0xd65f03c0]
            m.hi = m.lo + 8
            m.is_las = False
            m.is_skyward = True
            m.sky_patch_delta = delta
            m.lifter = Lifter(base, 'c')
            code = '\n'.join(m.emit_function(m.lo, m.hi, set(), {}, {}))
            self.assertIn(expected, code)
            self.assertNotIn('goto L_7000', code)
            m.sky_patch_delta = None  # 1.0.1 cheat addresses have not been verified.
            code = '\n'.join(m.emit_function(m.lo, m.hi, set(), {}, {}))
            self.assertNotIn('extern int rt_', code)

    def module(self, gap, loaded=False):
        m = Module.__new__(Module)
        m.base = m.lo = 0x1000
        # ADR x11, base; ADD x11, x11, x12, LSL #2; ...; BR x11; base:
        target = m.lo + (gap + 3) * 4
        imm = target - m.lo
        adr = 0x1000000B | ((imm & 3) << 29) | ((imm >> 2) << 5)
        m.words = [adr, 0xF940010B if loaded else 0x8B0C096B] + [0xD503201F] * gap + [0xD61F0160, 0xD65F03C0]
        m.hi = m.lo + len(m.words) * 4
        m.lifter = Lifter(m.base, 'c')
        m.cache, m.stats = {}, Counter()
        return m, target

    def test_scheduled_tables_stay_local(self):
        for gap in (8, 9, 32):  # NPC/cave schedules exceed the old seven-instruction lookahead.
            with self.subTest(gap=gap):
                m, target = self.module(gap)
                self.assertTrue(m.jump_table_base(m.lo, target))
                self.assertTrue(m.computed_target(target - 4, m.lo))

    def test_loaded_callback_is_not_a_table(self):
        m, target = self.module(9, loaded=True)
        self.assertFalse(m.jump_table_base(m.lo, target))

    def test_outside_text_is_not_a_table(self):
        m, target = self.module(9)
        self.assertFalse(m.jump_table_base(m.lo, m.hi + 4))

    def test_vector_destination_does_not_overwrite_branch_register(self):
        for simd in ('c', 'asm'):
            with self.subTest(simd=simd):
                m, target = self.module(9)
                m.words[0] = (m.words[0] & ~31) | 9  # ADR x9, base
                m.words[-2] = 0xD61F0120  # BR x9
                m.words[1] = 0x8B0A0929   # ADD x9, x9, x10, LSL #2
                m.words[-3] = 0x4EB01E09  # MOV v9.16b, v16.16b
                m.lifter = Lifter(m.base, simd)
                self.assertTrue(m.computed_target(target - 4, m.lo))
                self.assertTrue(m.jump_table_base(m.lo, target))

    def test_fp_result_in_gpr_does_overwrite_branch_register(self):
        for simd in ('c', 'asm'):
            with self.subTest(simd=simd):
                m, target = self.module(9)
                m.words[-3] = 0x9E66000B  # FMOV x11, d0
                m.lifter = Lifter(m.base, simd)
                self.assertFalse(m.computed_target(target - 4, m.lo))

    def test_sky_timing_hooks_follow_the_build(self):
        base = 0x7100004000  # the main module's base
        builds = {'1.0.0': {base + o: v for o, v in SKY_BUILDS['d824d4b1bbd45d03668380d6d041ba4ca00a64ff'].items()},
                  '1.0.1': {base + o: v for o, v in SKY_BUILDS['5441af3c89f97e1d'].items()}}
        for name, stores in builds.items():
            for simd in ('c', 'asm'):
                for sky in (False, True):
                    with self.subTest(build=name, simd=simd, sky=sky):
                        m, _ = self.module(9)
                        m.lo = m.base = min(stores)
                        m.words = [0xD503201F] * ((max(stores) - m.lo) // 4 + 2)
                        for pc, (_, _, word) in stores.items():
                            m.words[(pc - m.lo) // 4] = word
                        m.words[-1] = 0xD65F03C0
                        m.hi = m.lo + len(m.words) * 4
                        m.is_las = False
                        m.sky_stores = stores if sky else None
                        m.is_skyward = sky
                        m.lifter = Lifter(m.base, simd)
                        code = '\n'.join(m.emit_function(m.lo, m.hi, set(), {}, {}))
                        self.assertEqual(code.count('extern float rt_sky_frame_'), 2 if sky else 0)
                        if sky:
                            self.assertIn('*(f32a *)&c->v[0] = rt_sky_frame_step(*(f32a *)&c->v[0])', code)
                            self.assertIn('*(f32a *)&c->v[1] = rt_sky_frame_ms(*(f32a *)&c->v[1])', code)
                            self.assertIn('ST32(c->x[8] + 0xd38ull', code)
                            self.assertIn('ST32(c->x[8] + 0xd3cull', code)
                            self.assertNotIn('VSF32(', code)  # (simd.h isn't included with --simd asm)

    def test_sky_timing_refuses_a_binary_with_other_instructions(self):
        stores = {0x7100004000 + o: v for o, v in SKY_BUILDS['5441af3c89f97e1d'].items()}
        m, _ = self.module(9)
        m.lo = m.base = min(stores)
        m.words = [0xD503201F] * ((max(stores) - m.lo) // 4 + 2)  # no store at the timing addresses
        m.words[-1] = 0xD65F03C0
        m.hi = m.lo + len(m.words) * 4
        m.is_las = False
        m.sky_stores = stores
        m.is_skyward = True
        m.lifter = Lifter(m.base, 'c')
        with self.assertRaises(AssertionError):
            m.emit_function(m.lo, m.hi, set(), {}, {})

    def test_known_builds_by_build_id_prefix(self):
        self.assertEqual(sky_stores_for('D824D4B1BBD45D03668380D6D041BA4CA00A64FF'),
                         SKY_BUILDS['d824d4b1bbd45d03668380d6d041ba4ca00a64ff'])
        self.assertEqual(sky_stores_for('5441af3c89f97e1d' + '00' * 12), SKY_BUILDS['5441af3c89f97e1d'])
        self.assertIsNone(sky_stores_for('00' * 32))
        self.assertIsNone(sky_stores_for('5441af3c89f97e1e'))  # one byte off: not 1.0.1
        self.assertIsNone(sky_stores_for(''))
        self.assertIsNone(sky_stores_for(None))

    def test_1_0_1_timing_stores_are_the_same_instructions(self):
        # Read from the 1.0.1 main module (decoded text, offsets 0x104a4fc and 0x104a520): str s0,[x8,#0xd38] and
        # str s1,[x8,#0xd3c]. The 1.0.0 offsets hold other code in 1.0.1, so the hooks moved with the build.
        s = SKY_BUILDS['5441af3c89f97e1d']  # (offsets from the main module's base)
        self.assertEqual(s[0x104a4fc], (0, 0xd38, 0xbd0d3900))
        self.assertEqual(s[0x104a520], (1, 0xd3c, 0xbd0d3d01))
        self.assertNotIn(0x104a1ac, s)


    def test_sky_patches_only_for_sky(self):
        words = {STAMINA_STORE_SITE: 0xb90002e8, BEETLE_TIMER_SITE: 0x4b090108, HERO_UNLOCK_SITE: 0x38696908,
                 DAMAGE_SHIFT_SITE: 0x1ac822b5, DASH_USE_SITE: 0x51000529, DASH_START_SITE: 0x52800069,
                 DASH_CAP_SITE: 0x7100091f, FLIGHT_SPEED_SITE: 0x1e272863, DROP_THIN_SITE: 0x14000028,
                 DROP_END_SITE: 0x54000822, DASH_GAUGE_SITE: 0x79e99529,
                 WALK_SPEED_SITE: 0xbd04bac8}
        words.update({a: 0x39400296 for a in DROP_KIND_SITES})
        words.update({a: w for a, (w, _, _) in TREASURE_ROLL_SITES.items()})
        words.update({a: 0x910072a1 for a in GAUGE_PICTURE_SITES})
        self.assertEqual(set(words), set(SKY_PATCHES))
        for pc, word in words.items():
            for sky in (False, True):
                with self.subTest(pc=hex(pc), sky=sky):
                    m, _ = self.module(9)
                    m.lo = m.base = pc
                    m.words = [word, words.get(pc + 4, 0xD503201F), 0xD65F03C0]  # (the next one may be a site too)
                    m.hi = m.lo + 12
                    m.is_las, m.is_skyward = False, sky
                    m.lifter = Lifter(m.base, 'c')
                    code = '\n'.join(m.emit_function(m.lo, m.hi, set(), {}, {}))
                    self.assertEqual(f'/* {pc:#x}: ' in code, sky)


if __name__ == '__main__':
    unittest.main()
