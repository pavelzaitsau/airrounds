from contextlib import redirect_stdout
import io
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

with patch.object(sys, "path", [str(Path(__file__).resolve().parents[2] / "tools/quality"), *sys.path]):
    import memory


def node(name, size=None, qualifier="static"):
    label = f"{name}\\nsrc.cpp:1:1"
    if size is not None:
        label += f"\\n{size} bytes ({qualifier})"
    return f'node: {{ title: "{name}" label: "{label}" }}\n'


def edge(source, target):
    return f'edge: {{ sourcename: "{source}" targetname: "{target}" label: "src.cpp:2:2" }}\n'


APP = "int32_t airrounds_app(void*)"
TICK = "void {anonymous}::tick(void*)"
PARSE = "bool airrounds::uart::readReply(const char*, Reply&)"
REQUEST = "void airrounds::flipper::SessionClient::sendFrame(Request, const char*, const Profile*)"
SEND = "static void {anonymous}::App::send(void*, const char*, size_t)"


class MemoryBudgetTest(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.root = Path(self.directory.name)
        self.manifest = self.root / "application.fam"
        self.manifest.write_text('App(appid="airrounds", stack_size=1000)\n')

    def tearDown(self):
        self.directory.cleanup()

    def stack(self, text):
        path = self.root / "app.ci"
        path.write_text(text)
        with redirect_stdout(io.StringIO()):
            return memory.flipper_stack(path, self.manifest)

    def graph(self, parse_size=100, extra=""):
        return (node(APP, 50) + node(TICK, 200) + node(PARSE, parse_size) + node(REQUEST, 40) +
                node(SEND, 30) + node("__indirect_call") + node("furi_hal_serial_tx") +
                edge(TICK, PARSE) + edge(TICK, REQUEST) + edge(REQUEST, "__indirect_call") +
                edge(SEND, "furi_hal_serial_tx") + extra)

    def test_callback_runs_on_the_app_frame(self):
        self.assertEqual(self.stack(self.graph()), 50 + 200 + 100)

    def test_indirect_call_follows_listed_target(self):
        # tick > sendFrame > App::send outweighs tick > readReply when the parser frame is small.
        self.assertEqual(self.stack(self.graph(parse_size=10)), 50 + 200 + 40 + 30)

    def test_budget_is_half_the_thread_stack(self):
        self.assertEqual(self.stack(self.graph(parse_size=250)), 500)
        with self.assertRaisesRegex(SystemExit, "use 501 bytes; the budget is 500"):
            self.stack(self.graph(parse_size=251))

    def test_unlisted_indirect_call_fails(self):
        other = "void {anonymous}::paint(App&)"
        extra = node(other, 8) + edge(other, "__indirect_call")
        with self.assertRaisesRegex(SystemExit, "Unlisted indirect call"):
            self.stack(self.graph(extra=extra))

    def test_recursion_fails(self):
        with self.assertRaisesRegex(SystemExit, "Recursion makes the stack unbounded"):
            self.stack(self.graph(extra=edge(PARSE, TICK)))

    def test_dynamic_frame_fails(self):
        with self.assertRaisesRegex(SystemExit, "dynamic"):
            self.stack(self.graph() + node("void grow(size_t)", 16, "dynamic"))

    def test_missing_indirect_target_fails(self):
        # A renamed App::send must not silently drop the indirect edge.
        text = self.graph().replace(SEND, "static void {anonymous}::App::transmit(void*)")
        with self.assertRaisesRegex(SystemExit, "Indirect call target App::send not found"):
            self.stack(text)

    def test_missing_entry_fails(self):
        with self.assertRaisesRegex(SystemExit, "airrounds_app not found"):
            self.stack(self.graph().replace(APP, "int32_t other_app(void*)"))

    def test_manifest_without_stack_size_fails(self):
        self.manifest.write_text('App(appid="airrounds")\n')
        with self.assertRaisesRegex(SystemExit, "No stack_size"):
            self.stack(self.graph())


class Esp32MemoryTest(unittest.TestCase):
    def check(self, globals_, frames):
        with tempfile.TemporaryDirectory() as temporary:
            report = Path(temporary) / "sketch.ino.cpp.su"
            report.write_text("".join(f"sketch.ino.cpp:1:1:{name}\t{size}\t{qualifier}\n"
                                      for name, size, qualifier in frames))
            output = f"Global variables use {globals_} bytes (15%) of dynamic memory.\n"
            with redirect_stdout(io.StringIO()):
                return memory.esp32_memory(output, report)

    def test_within_budgets(self):
        frames = [("void loop()", 288, "static"),
                  ("bool airrounds::uart::writeSessionJournal(const char*) [with File = SdFile]",
                   2512, "static")]
        self.assertEqual(self.check(52144, frames), 52144)

    def test_global_ram_budget(self):
        with self.assertRaisesRegex(SystemExit, "globals use 65537 bytes"):
            self.check(64 * 1024 + 1, [("void loop()", 288, "static")])

    def test_frame_limit(self):
        with self.assertRaisesRegex(SystemExit, "1025-byte frame; the limit is 1024"):
            self.check(1000, [("void loop()", 1025, "static")])

    def test_reviewed_frame_has_its_own_cap(self):
        name = "bool airrounds::uart::writeSessionJournal(const char*)"
        with self.assertRaisesRegex(SystemExit, "2561-byte frame; the limit is 2560"):
            self.check(1000, [(name, 2561, "static")])

    def test_dynamic_frame_fails(self):
        with self.assertRaisesRegex(SystemExit, "dynamic,bounded, not static"):
            self.check(1000, [("void grow(size_t)", 64, "dynamic,bounded")])

    def test_budgets_include_their_limits(self):
        frames = [("void loop()", 1024, "static"),
                  ("bool airrounds::uart::writeSessionJournal(const char*)", 2560, "static")]
        self.assertEqual(self.check(64 * 1024, frames), 64 * 1024)

    def test_report_lists_the_largest_frames_first(self):
        with tempfile.TemporaryDirectory() as temporary:
            report = Path(temporary) / "sketch.ino.cpp.su"
            report.write_text("".join(f"sketch.ino.cpp:1:1:void f{size}()\t{size}\tstatic\n"
                                      for size in (16, 512, 64, 256)))
            output = io.StringIO()
            with redirect_stdout(output):
                memory.esp32_memory("Global variables use 1000 bytes", report)
        self.assertIn("largest frames: f512 512 of 1024; f256 256 of 1024; f64 64 of 1024",
                      output.getvalue())

    def test_repeated_function_keeps_its_largest_frame(self):
        frames = [("void loop()", 1025, "static"), ("void loop()", 8, "static")]
        with self.assertRaisesRegex(SystemExit, "1025-byte frame"):
            self.check(1000, frames)

    def test_missing_ram_report_fails(self):
        with self.assertRaisesRegex(SystemExit, "did not report global RAM"):
            memory.esp32_memory("", Path("unused"))


if __name__ == "__main__":
    unittest.main()
