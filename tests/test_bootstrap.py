"""Checks the bootstrap's codec contract and driver transport separately.

Run with ``mise run test``: the task selects each built extension and
interpreter. These tests must pass even while the later M1 suite fails
for missing host methods. Fixed wire bytes detect paired encoder/decoder
mistakes that a round trip alone could hide; subprocess tests verify that
adapter errors remain valid frames and clean EOF terminates the process.
"""

import math
from concurrent.futures import ThreadPoolExecutor
import subprocess
import sys
import unittest

import tinyactors


class CodecTests(unittest.TestCase):
    """Exercise the public codec without private core or adapter calls."""

    def test_known_wire_values(self):
        """Check independent wire expectations for the supported subset."""
        cases = [
            (None, "f6"), (False, "f4"), (True, "f5"),
            (0, "00"), (24, "1818"), (-1, "20"), (-25, "3818"),
            (2**64 - 1, "1bffffffffffffffff"),
            (-2**64, "3bffffffffffffffff"),
            (1.5, "f93e00"), ("é", "62c3a9"), (b"\x00\xff", "4200ff"),
            ([1, None], "8201f6"), ({"id": 1}, "a162696401"),
        ]
        for value, wire in cases:
            with self.subTest(value=value):
                encoded = bytes.fromhex(wire)
                self.assertEqual(tinyactors.encode(value), encoded)
                self.assertEqual(tinyactors.decode(encoded), value)

    def test_growing_writer_and_shared_subtrees(self):
        """Exercise writer growth and repeated references without cycles."""
        shared = ["value" * 1000, b"bytes" * 1000]
        value = {"params": [shared, shared], "float": float("inf")}
        self.assertEqual(tinyactors.decode(tinyactors.encode(value)), value)
        self.assertTrue(math.isnan(tinyactors.decode(tinyactors.encode(float("nan")))))

    def test_rejected_values(self):
        """Unsupported objects must raise instead of changing wire meaning."""
        for value in [object(), (1,), {1: "value"}, 2**64, -2**64 - 1, "\ud800"]:
            with self.subTest(value=repr(value)):
                with self.assertRaises((TypeError, OverflowError, UnicodeError)):
                    tinyactors.encode(value)
        cycle = []
        cycle.append(cycle)
        with self.assertRaises(ValueError):
            tinyactors.encode(cycle)

    def test_nesting_limit(self):
        """Exercise core's bound with nonempty arrays and cyclic-depth guards."""
        value = 0
        for _ in range(64):
            value = [value]
        self.assertEqual(tinyactors.decode(tinyactors.encode(value)), value)
        with self.assertRaises(ValueError):
            tinyactors.encode([value])
        with self.assertRaises(ValueError):
            tinyactors.decode(b"\x81" * 65 + b"\x00")

    def test_invalid_and_unsupported_wire_values(self):
        """Reject incomplete, trailing and currently unmapped wire values."""
        for wire in ["", "1818ff", "81", "a16161", "63eda080", "9fff",
                     "d820d8206161", "a10101"]:
            with self.subTest(wire=wire):
                with self.assertRaises(ValueError):
                    tinyactors.decode(bytes.fromhex(wire))

    def test_decode_requires_immutable_bytes(self):
        """Keep the first decoder's immutable-input contract explicit."""
        for value in [bytearray(b"\x01"), memoryview(b"\x01"), "01"]:
            with self.assertRaises(TypeError):
                tinyactors.decode(value)

    def test_independent_concurrent_calls(self):
        """Exercise call-local state; this does not test shared input mutation."""
        def roundtrip(index):
            """Convert one worker-owned value and compare its decoded result."""
            value = {"id": index, "payload": [b"data" * 1000, True, None,
                                               tinyactors.UNDEFINED, tinyactors.Address("peer")]}
            return tinyactors.decode(tinyactors.encode(value)) == value

        with ThreadPoolExecutor(max_workers=4) as executor:
            self.assertTrue(all(executor.map(roundtrip, range(32))))


class AdapterTests(unittest.TestCase):
    """Check the real command through stdin/stdout, using public codec calls."""

    def test_multiple_calls_and_clean_eof(self):
        """Match reply ids, frame each error, and exit after the final frame."""
        frames = []
        for call_id, method in [(1, "no.such.method"), (2, "host.create")]:
            message = tinyactors.encode({"id": call_id, "method": method, "params": {}})
            frames.append(len(message).to_bytes(4, "big") + message)
        result = subprocess.run(
            [sys.executable, "-m", "conform"], input=b"".join(frames),
            capture_output=True, timeout=10, check=True,
        )
        self.assertEqual(result.stderr, b"")
        output = result.stdout
        for call_id, code, reason in [(1, -32601, "unknown"), (2, -32602, "missing")]:
            size = int.from_bytes(output[:4], "big")
            reply = tinyactors.decode(output[4:4 + size])
            self.assertEqual(reply["id"], call_id)
            self.assertEqual(reply["error"]["code"], code)
            self.assertEqual(reply["error"]["data"]["reason"], reason)
            self.assertNotIn("result", reply)
            output = output[4 + size:]
        self.assertEqual(output, b"")

    def test_truncated_frame_is_an_error(self):
        """Distinguish an incomplete header from normal EOF between frames."""
        result = subprocess.run(
            [sys.executable, "-m", "conform"], input=b"\x00\x00",
            capture_output=True, timeout=10,
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(result.stdout, b"")
        self.assertIn(b"incomplete driver frame", result.stderr)
