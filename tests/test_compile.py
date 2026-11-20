"""compile_charts, Compiled and the resolver."""

import os
import pathlib
import tempfile
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

    def test_image_is_bytes_copied_once_and_a_first_read_after_close_is_stale(self):
        compiled = tinyactors.compile_charts([("c.scxml", GOOD)])
        image = compiled.image
        self.assertIsInstance(image, bytes)
        self.assertIs(compiled.image, image)  # copied once
        compiled.close()
        self.assertIs(compiled.image, image)  # what was read stays
        unread = tinyactors.compile_charts([("c.scxml", GOOD)])
        unread.close()
        with self.assertRaises(tinyactors.TinyactorsError) as raised:
            unread.image
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
            with tinyactors.Host([], system="t", log=logs, clock=tinyactors.SimulatedClock()) as host:
                segment = host.add_image(compiled.image)
                with self.assertRaises(tinyactors.StaleError):
                    host.actor("first")
                host.boot(segment)
                host.run_until_idle()
                first, second = host.actor("first"), host.actor("second")
                self.assertNotEqual(first, second)
                self.assertEqual(first, host.actor(first.address))
                self.assertEqual(first, host.actor(first.id))
                self.assertEqual(first, host.actor("tinyactors://t/@first"))
                self.assertEqual(sorted(logs.labelled("boot")), ["first", "second"])
                first.terminate()
                host.run_until_idle()
                with self.assertRaises(tinyactors.StaleError):
                    host.actor("first")
                with self.assertRaises(tinyactors.StaleError):
                    host.actor(first.id)
                with self.assertRaises(TypeError):
                    host.actor(1.5)
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


class Directory(unittest.TestCase):
    """compile_dir: every *.scxml under a directory, its src files read from
    under it and nowhere else."""

    def tree(self, files):
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        root = pathlib.Path(directory.name, "charts")
        for name, text in files.items():
            (root / name).parent.mkdir(parents=True, exist_ok=True)
            (root / name).write_text(text, encoding="utf-8")
        return root

    def test_a_directory_compiles_into_one_image_its_src_read_under_it(self):
        script = fixture("test_compile/script-src.python.scxml")
        root = self.tree({"parent.scxml": PARENT, "child.scxml": fixture("test_compile/child.python.scxml"),
                          "orders/script.scxml": script.replace('name="c"', 'name="script"'),
                          "orders/reader.py": fixture("test_compile/reader.py"),
                          "notes.txt": "not a chart"})
        logs = Logs()
        with tinyactors.compile_dir(root) as compiled:
            host = tinyactors.Host(compiled, system="t", log=logs)
        with host:
            for chart in ("parent", "child", "script"):
                host.spawn(chart)
            host.run_until_idle()
        with tinyactors.compile_dir(str(root), resolver=Resolver({"reader.py": "def f(x):\n    return x\n"})):
            pass

    def test_a_src_that_leaves_the_directory_is_refused(self):
        script = fixture("test_compile/script-src.python.scxml")
        for src in ("../outside.py", "/etc/hostname", "link.py"):
            with self.subTest(src=src):
                root = self.tree({"c.scxml": script.replace("reader.py", src)})
                (root.parent / "outside.py").write_text("def f(x):\n    return x\n")
                os.symlink(root.parent / "outside.py", root / "link.py")
                with self.assertRaises(tinyactors.CompileError) as raised:
                    tinyactors.compile_dir(root)
                self.assertIn("leaves", " ".join(d.message for d in raised.exception.diagnostics))

    def test_a_missing_file_and_an_empty_directory(self):
        script = fixture("test_compile/script-src.python.scxml")
        with self.assertRaises(tinyactors.CompileError):
            tinyactors.compile_dir(self.tree({"c.scxml": script}))
        with self.assertRaises(ValueError):
            tinyactors.compile_dir(self.tree({"notes.txt": ""}))
        with self.assertRaises(FileNotFoundError):
            tinyactors.compile_dir("/nonexistent/charts")


if __name__ == "__main__":
    unittest.main()
