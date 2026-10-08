import importlib.util, pathlib, unittest

spec = importlib.util.spec_from_file_location(
    "gen", pathlib.Path(__file__).with_name("gen_ov5640_regs.py"))
gen = importlib.util.module_from_spec(spec); spec.loader.exec_module(gen)

REGS_H = """
#define SYSTEM_CTROL0 0x3008
"""
SETTINGS_H = """
#define REG_DLY 0xffff
#define REGLIST_TAIL 0x0000
static const ratio_settings_t ratio_table[] = {
    { 2560, 1920,   0,   0, 2623, 1951, 32, 16, 2844, 1968 }, //4x3
};
static const DRAM_ATTR uint16_t sensor_default_regs[][2] = {
    {SYSTEM_CTROL0, 0x82}, // software reset
    {REG_DLY, 10},
    {0x3103, 0x13},
    {REGLIST_TAIL, 0x00},
};
static const DRAM_ATTR uint16_t sensor_fmt_jpeg[][2] = {
    {0x4300, 0x30},
    {REGLIST_TAIL, 0x00},
};
"""

class GenTest(unittest.TestCase):
    def test_parses_tables_with_names_delays_and_tail(self):
        tables = gen.parse(REGS_H, SETTINGS_H)
        self.assertEqual(list(tables), ["sensor_default_regs", "sensor_fmt_jpeg"])
        self.assertEqual(tables["sensor_default_regs"],
                         [("W", 0x3008, 0x82), ("D", 10, 0), ("W", 0x3103, 0x13)])
        self.assertEqual(tables["sensor_fmt_jpeg"], [("W", 0x4300, 0x30)])

    def test_emits_rust(self):
        rs = gen.emit({"sensor_fmt_jpeg": [("W", 0x4300, 0x30), ("D", 5, 0)]})
        self.assertIn("pub const SENSOR_FMT_JPEG: &[Op] = &[", rs)
        self.assertIn("Op::Write(0x4300, 0x30),", rs)
        self.assertIn("Op::DelayMs(5),", rs)

    def test_unknown_symbol_fails_loudly(self):
        with self.assertRaises(KeyError):
            gen.parse(REGS_H, "#define REGLIST_TAIL 0x0000\nstatic const uint16_t t[][2] = { {NOPE, 1}, {REGLIST_TAIL, 0} };")

    def test_literal_tail_value_ends_the_list(self):
        h = "#define REG_DLY 0xffff\n#define REGLIST_TAIL 0x0000\nstatic const uint16_t t[][2] = { {0x3103, 1}, {0x0000, 0}, {0x3104, 2} };"
        self.assertEqual(gen.parse(REGS_H, h)["t"], [("W", 0x3103, 1)])

    def test_literal_delay_value_becomes_a_delay(self):
        h = "#define REG_DLY 0xffff\n#define REGLIST_TAIL 0x0000\nstatic const uint16_t t[][2] = { {0xffff, 20}, {REGLIST_TAIL, 0} };"
        self.assertEqual(gen.parse(REGS_H, h)["t"], [("D", 20, 0)])

    def test_unparseable_row_fails_loudly(self):
        h = "#define REG_DLY 0xffff\n#define REGLIST_TAIL 0x0000\nstatic const uint16_t t[][2] = { {0x3800, (x>>8)}, {REGLIST_TAIL, 0} };"
        with self.assertRaises(ValueError) as cm:
            gen.parse(REGS_H, h)
        self.assertIn("t", str(cm.exception))

    def test_missing_sentinels_raise_keyerror(self):
        with self.assertRaises(KeyError):
            gen.parse(REGS_H, "static const uint16_t t[][2] = { {0x3103, 1} };")

    def test_real_vendor_files_parse(self):
        root = pathlib.Path(__file__).resolve().parent.parent / "rust" / "ov5640" / "vendor"
        tables = gen.parse((root / "ov5640_regs.h").read_text(),
                           (root / "ov5640_settings.h").read_text())
        self.assertGreater(len(tables["sensor_default_regs"]), 50)
        self.assertIn("sensor_fmt_jpeg", tables)
        first = tables["sensor_default_regs"][0]
        self.assertEqual(first, ("W", 0x3008, 0x82))  # software reset

if __name__ == "__main__":
    unittest.main()
