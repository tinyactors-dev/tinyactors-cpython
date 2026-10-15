"""Apply the shared neutral-value specification to the public Python codec.

The suite owns values.txt; these tests read it directly rather than copy
its examples or call private core entry points. Python expressions are
trusted repository test data, evaluated with only their required builtins.
The adapter's M1 host checks remain separate from this host-free codec gate.
"""

import gc
import importlib.util
from pathlib import Path
import subprocess
import sys
import unittest
import weakref

import tinyactors


def value_cases():
    """Read kind/name and field lines using the suite's blank-line format."""
    path = Path(__file__).resolve().parents[2] / "tinyactors-test/suites/protocol/values.txt"
    cases = []
    current = None
    for line in path.read_text(encoding="utf-8").splitlines():
        if line.startswith("#"):
            continue
        if not line.strip():
            current = None
            continue
        key, _, value = line.partition(" ")
        if current is None:
            current = {"kind": key, "name": value, "fields": {}}
            cases.append(current)
        else:
            current["fields"][key] = value
    return cases


class ValueVectorTests(unittest.TestCase):
    """Require every applicable vector family to exercise at least one case."""

    def test_python_expressions(self):
        """Compare encoded bytes to independent vectors; test invalid expressions too."""
        seen = {"encode": 0, "invalid": 0}
        for case in value_cases():
            expression = case["fields"].get("python")
            if expression is None:
                continue
            with self.subTest(case=case["name"]):
                value = eval(expression, {"__builtins__": {}, "float": float, "bytes": bytes})
                if case["kind"] == "invalid":
                    with self.assertRaises((ValueError, OverflowError, UnicodeError, TypeError)):
                        tinyactors.encode(value)
                else:
                    self.assertEqual(case["kind"], "encode")
                    self.assertEqual(tinyactors.encode(value), bytes.fromhex(case["fields"]["cbor"]))
                seen[case["kind"]] += 1
        self.assertGreater(seen["encode"], 0)
        self.assertGreater(seen["invalid"], 0)

    def test_applicable_roundtrips(self):
        """Preserve Python's reference/undefined types and exact neutral values."""
        seen = 0
        for case in value_cases():
            if case["kind"] != "roundtrip" or case["name"].startswith("ECMAScript:"):
                continue
            with self.subTest(case=case["name"]):
                wire = bytes.fromhex(case["fields"]["cbor"])
                back = bytes.fromhex(case["fields"].get("back", case["fields"]["cbor"]))
                self.assertEqual(tinyactors.encode(tinyactors.decode(wire)), back)
                seen += 1
        self.assertGreater(seen, 0)

    def test_malformed_values(self):
        """All malformed neutral-value vectors must be refused before returning a value."""
        seen = 0
        for case in value_cases():
            if case["kind"] != "malformed":
                continue
            with self.subTest(case=case["name"]):
                with self.assertRaises(ValueError):
                    tinyactors.decode(bytes.fromhex(case["fields"]["cbor"]))
                seen += 1
        self.assertGreater(seen, 0)


class ValueTypeTests(unittest.TestCase):
    """Check public identity, immutability and lifetime beyond wire roundtrips."""

    def test_undefined_identity(self):
        """Undefined stays the exported singleton at every supported position."""
        self.assertIs(tinyactors.decode(b"\xf7"), tinyactors.UNDEFINED)
        value = tinyactors.decode(bytes.fromhex("83f7a16161f7f6"))
        self.assertIs(value[0], tinyactors.UNDEFINED)
        self.assertIs(value[1]["a"], tinyactors.UNDEFINED)
        self.assertIsNone(value[2])
        self.assertEqual(repr(tinyactors.UNDEFINED), "UNDEFINED")
        self.assertIsNot(tinyactors.UNDEFINED, None)
        with self.assertRaises(TypeError):
            type(tinyactors.UNDEFINED)()

    def test_address_value_semantics(self):
        """Address is a tagged immutable value rather than a string alias or handle."""
        value = tinyactors.Address("peer")
        self.assertEqual(value.text, "peer")
        self.assertEqual(str(value), "peer")
        self.assertEqual(repr(value), "Address('peer')")
        self.assertEqual(value, tinyactors.Address("peer"))
        self.assertNotEqual(value, tinyactors.Address("other"))
        self.assertNotEqual(value, "peer")
        self.assertEqual(hash(value), hash(tinyactors.Address("peer")))
        self.assertEqual({value: "found"}[tinyactors.Address("peer")], "found")
        decoded = tinyactors.decode(bytes.fromhex("d8206470656572"))
        self.assertIs(type(decoded), tinyactors.Address)
        self.assertEqual(decoded, value)
        with self.assertRaises(TypeError):
            tinyactors.encode({value: 1})
        with self.assertRaises(TypeError):
            value < tinyactors.Address("other")

    def test_address_constructor_and_immutability(self):
        """Refuse conversion hooks, mutable fields, subclassing and type patching."""
        class Text(str):
            """A Unicode subclass must not bypass the codec's exact-type contract."""

        for text in [1, b"peer", Text("peer")]:
            with self.assertRaises(TypeError):
                tinyactors.Address(text)
        with self.assertRaises(UnicodeError):
            tinyactors.Address("\ud800")
        with self.assertRaises(TypeError):
            tinyactors.Address(text="peer")
        with self.assertRaises(AttributeError):
            tinyactors.Address("peer").text = "other"
        for value_type in [tinyactors.Address, type(tinyactors.UNDEFINED)]:
            with self.assertRaises(TypeError):
                type("Subclass", (value_type,), {})
            with self.assertRaises(TypeError):
                value_type.extra = 1

    def test_tag_requires_immediate_text(self):
        """Reject nested tags, truncated references, non-text children and tagged keys."""
        for wire in ["d820", "d82001", "d820d8206161", "d820f7", "a1d820616101"]:
            with self.subTest(wire=wire):
                with self.assertRaises(ValueError):
                    tinyactors.decode(bytes.fromhex(wire))

    def test_tag_counts_toward_depth(self):
        """An Address's tag occupies one level of core's wire nesting budget."""
        value = tinyactors.Address("peer")
        for _ in range(63):
            value = [value]
        wire = tinyactors.encode(value)
        self.assertEqual(tinyactors.encode(tinyactors.decode(wire)), wire)
        with self.assertRaises(ValueError):
            tinyactors.encode([value])

    def test_independent_modules_and_gc(self):
        """Separate module instances own distinct identities and collect their cycles."""
        spec = importlib.util.spec_from_file_location("tinyactors", tinyactors.__file__)
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        self.assertIsNot(module.Address, tinyactors.Address)
        self.assertIsNot(module.UNDEFINED, tinyactors.UNDEFINED)
        address = module.Address("peer")
        self.assertEqual(module.decode(module.encode(address)), address)
        self.assertIs(module.decode(b"\xf7"), module.UNDEFINED)
        with self.assertRaises(TypeError):
            tinyactors.encode(address)
        reference = weakref.ref(module)
        del address, module
        gc.collect()
        self.assertIsNone(reference())

    def test_export_rebinding_keeps_codec_identities(self):
        """Codec behavior is tied to module state, not mutable dictionary exports."""
        spec = importlib.util.spec_from_file_location("tinyactors", tinyactors.__file__)
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        address = module.Address("peer")
        sentinel = module.UNDEFINED
        module.Address = object
        module.UNDEFINED = None
        self.assertEqual(module.decode(module.encode(address)), address)
        self.assertIs(module.decode(module.encode(sentinel)), sentinel)

    def test_codec_refuses_unexecuted_module(self):
        """Before exec_module, methods must raise rather than dereference absent state."""
        code = """
import importlib.util
import tinyactors

spec = importlib.util.spec_from_file_location('tinyactors', tinyactors.__file__)
module = importlib.util.module_from_spec(spec)
for method, argument in [(module.encode, None), (module.decode, b'\\xf7')]:
    try:
        method(argument)
    except RuntimeError as error:
        assert 'not initialized' in str(error)
    else:
        raise AssertionError('an uninitialized codec call succeeded')
"""
        result = subprocess.run([sys.executable, "-c", code], capture_output=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stderr.decode())
        self.assertEqual(result.stdout, b"")
