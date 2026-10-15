"""compile_charts, Compiled and the resolver."""

import unittest

import tinyactors
from support import Logs, fixture

GOOD = fixture("test_compile/good.python.scxml")
PARENT = fixture("test_compile/parent.python.scxml")


class Resolver:
    def __init__(self, files):
        self.files = files
        self.asked = []

    def __call__(self, base, source):
        self.asked.append((base, source))
        if source not in self.files:
            raise tinyactors.ResolveError("no file %s" % source, line=3, column=7)
        return self.files[source]


class Compile(unittest.TestCase):
    def test_an_error_raises_compile_error_with_every_diagnostic(self):
        bad = fixture("test_compile/bad.python.scxml")
        with self.assertRaises(tinyactors.CompileError) as raised:
            tinyactors.compile_charts([("bad.scxml", bad)])
        diagnostics = raised.exception.diagnostics
        self.assertTrue(diagnostics)
        self.assertEqual(diagnostics[0].severity, "error")
        self.assertEqual(diagnostics[0].file, "bad.scxml")
        self.assertIsInstance(raised.exception, tinyactors.TinyactorsError)

    def test_image_is_stale_after_close_and_close_refuses_a_held_buffer(self):
        compiled = tinyactors.compile_charts([("c.scxml", GOOD)])
        view = memoryview(compiled.image)
        self.assertTrue(bytes(view))
        with self.assertRaises(BufferError):
            compiled.close()
        view.release()
        compiled.close()
        with self.assertRaises(tinyactors.TinyactorsError) as raised:
            compiled.image
        self.assertEqual(raised.exception.status, "TA_ERROR_STALE")

    def test_a_resolver_reads_src_and_refuses_at_its_line(self):
        child = fixture("test_compile/child.python.scxml")
        resolver = Resolver({"child.scxml": child})
        with tinyactors.compile_charts([("parent.scxml", PARENT)], resolver) as compiled:
            self.assertTrue(len(compiled.image))
        self.assertEqual(resolver.asked, [("parent.scxml", "child.scxml")])
        with self.assertRaises(tinyactors.CompileError) as raised:
            tinyactors.compile_charts([("parent.scxml", PARENT)], Resolver({}))
        diagnostic = [d for d in raised.exception.diagnostics if d.severity == "error"][0]
        self.assertEqual((diagnostic.line, diagnostic.column), (3, 7))
        self.assertIn("no file child.scxml", diagnostic.message)

    def test_a_py_file_is_checked_as_it_is_read(self):
        document = fixture("test_compile/script-src.python.scxml")
        with self.assertRaises(tinyactors.CompileError) as raised:
            tinyactors.compile_charts([("c.scxml", document)], Resolver({"reader.py": fixture("test_compile/reader-refused.py")}))
        diagnostic = [d for d in raised.exception.diagnostics if d.severity == "error"][0]
        self.assertEqual(diagnostic.line, 3)
        self.assertIn("import", diagnostic.message)
        with tinyactors.compile_charts([("c.scxml", document)], Resolver({"reader.py": fixture("test_compile/reader.py")})):
            pass

    def test_another_exception_of_a_resolver_is_raised(self):
        def broken(base, source):
            raise OSError("disk")

        with self.assertRaises(OSError):
            tinyactors.compile_charts([("parent.scxml", PARENT)], broken)

    def test_a_resolver_is_a_callable(self):
        with self.assertRaises(TypeError):
            tinyactors.compile_charts([("parent.scxml", PARENT)], {"child.scxml": ""})

    def test_a_manifest_compiles_into_an_image_whose_actors_boot_by_name(self):
        resolver = Resolver({"worker.scxml": fixture("test_compile/worker.python.scxml")})
        logs = Logs()
        with tinyactors.compile_system(("system.xml", fixture("test_compile/system.xml")), resolver) as compiled:
            with tinyactors.Host({"systemID": "t"}, log=logs, clock=tinyactors.SimulatedClock()) as host:
                segment = host.add_image(compiled.image)
                self.assertIsNone(host.actor_by_name("first"))
                host.boot(segment)
                host.run_until_idle()
                first, second = host.actor_by_name("first"), host.actor_by_name("second")
                self.assertTrue(first and second and first != second)
                self.assertEqual(sorted(logs.labelled("boot")), ["first", "second"])
                host.terminate(first)
                host.run_until_idle()
                self.assertIsNone(host.actor_by_name("first"))
                with self.assertRaises(TypeError):
                    host.actor_by_name(1)
        self.assertEqual(resolver.asked, [("system.xml", "worker.scxml")])

    def test_a_manifest_with_an_error_raises_compile_error(self):
        with self.assertRaises(tinyactors.CompileError) as raised:
            tinyactors.compile_system(("system.xml", "<charts/>"))
        self.assertIn("<system>", raised.exception.diagnostics[0].message)
        with self.assertRaises(TypeError):
            tinyactors.compile_system([("system.xml", "<system/>")])

    def test_without_a_resolver_src_is_refused(self):
        with self.assertRaises(tinyactors.CompileError):
            tinyactors.compile_charts([("parent.scxml", PARENT)])


if __name__ == "__main__":
    unittest.main()
