"""Snapshots: Host.snapshot and Host.restore, on both datamodels."""

import unittest

import tinyactors
from support import Logs, element, find, fixture, load, text

CONFIG = {"systemID": "t", "system": {"ticksPerSecond": 1000}}
COUNTER = {datamodel: fixture("test_snapshot/counter.%s.scxml" % datamodel) for datamodel in ("python", "ecmascript")}


def host_of(document, **options):
    options.setdefault("clock", tinyactors.SimulatedClock())
    host = tinyactors.Host(CONFIG, **options)
    with tinyactors.compile_charts([("counter.scxml", document)]) as compiled:
        host.add_image(compiled.image)
    return host


class Snapshot(unittest.TestCase):
    def round_trip(self, datamodel):
        logs, clock = Logs(), tinyactors.SimulatedClock()
        with host_of(COUNTER[datamodel], log=logs, clock=clock) as host:
            session = host.spawn(host.chart("counter"))
            host.send(session, "inc", {"k": 1})
            host.send(session, "inc", "two")
            host.send(session, "wait")
            host.run_until_idle()
            host.send(session, "inc", 3)  # waiting in the mailbox, its payload in the table
            blob = host.snapshot()
        self.assertIsInstance(blob, bytes)
        # a new clock that starts at 0: the restored host goes on from the snapshot's time
        logs, clock = Logs(), tinyactors.SimulatedClock()
        with tinyactors.Host.restore(blob, CONFIG, log=logs, clock=clock) as host:
            host.send(session, "report", str(session))
            host.run_until_idle()
            self.assertEqual(logs.labelled("late"), [])
            clock.increment(0.999)
            self.assertEqual(logs.labelled("late"), [])
            clock.increment(0.001)
            other = host.spawn(host.chart("counter"))
            self.assertNotEqual(other, session)
        return logs

    def test_a_python_host_goes_on_after_a_restore(self):
        logs = self.round_trip("python")
        self.assertEqual(logs.labelled("n"), ["3 [{'k': 1}, 'two', 3] report True"])
        self.assertEqual(logs.labelled("late"), ["{'k': 2}"])

    def test_an_ecmascript_host_goes_on_after_a_restore(self):
        logs = self.round_trip("ecmascript")
        self.assertEqual(logs.labelled("n"), ['3 [{"k":1},"two",3] report true'])
        self.assertEqual(logs.labelled("late"), ['{"k":2}'])

    def test_the_snapshot_of_a_restored_host_is_the_same(self):
        for datamodel in ("python", "ecmascript"):
            with self.subTest(datamodel=datamodel):
                clock = tinyactors.SimulatedClock()
                with host_of(COUNTER[datamodel], clock=clock) as host:
                    for _ in range(5):
                        session = host.spawn(host.chart("counter"))
                        host.send(session, "inc", [session])
                        host.send(session, "wait")
                    host.run_until_idle()
                    blob = host.snapshot()
                with tinyactors.Host.restore(blob, CONFIG, clock=clock) as host:
                    self.assertEqual(host.snapshot(), blob)

    def test_a_hibernated_actor_wakes_after_a_restore(self):
        logs, blobs = Logs(), {}

        def wake(host, session, migration):
            host.wake(session, blobs.pop(session))

        with host_of(COUNTER["python"]) as host:
            session = host.spawn(host.chart("counter"))
            host.send(session, "inc", 1)
            host.run_until_idle()
            blobs[session] = host.hibernate(session)
            blob = host.snapshot()
        with tinyactors.Host.restore(blob, CONFIG, log=logs, wake=wake, clock=tinyactors.SimulatedClock()) as host:
            host.send(session, "report", 0)
            host.run_until_idle()
        self.assertEqual(logs.labelled("n"), ["1 [1] report False"])

    def test_a_value_outside_the_table_refuses_the_snapshot(self):
        with host_of(COUNTER["python"]) as host:
            session = host.spawn(host.chart("counter"))
            host.send(session, "odd")
            host.run_until_idle()
            with self.assertRaises(tinyactors.TinyactorsError) as raised:
                host.snapshot()
            self.assertEqual(raised.exception.status, "TA_ERROR_UNSUPPORTED")
            host.send(session, "inc", 1)  # the host goes on
            host.run_until_idle()

    def test_a_snapshot_from_a_handler_is_refused(self):
        refused = []

        def log(host, entry):
            try:
                host.snapshot()
            except tinyactors.TinyactorsError as error:
                refused.append(error.status)

        with host_of(COUNTER["python"], log=log) as host:
            session = host.spawn(host.chart("counter"))
            host.send(session, "report", 0)
            host.run_until_idle()
        self.assertEqual(refused, ["TA_ERROR_STATE"])

    def test_restore_refuses_what_is_no_snapshot(self):
        with self.assertRaises(tinyactors.TinyactorsError):
            tinyactors.Host.restore(b"\x00" * 64, CONFIG)
        with self.assertRaises(TypeError):
            tinyactors.Host.restore()

    def test_a_snapshot_restores_from_an_unaligned_buffer(self):
        with host_of(COUNTER["python"]) as host:
            host.spawn(host.chart("counter"))
            host.run_until_idle()
            blob = host.snapshot()
        shifted = memoryview(b"x" + blob)[1:]
        with tinyactors.Host.restore(shifted, CONFIG, clock=tinyactors.SimulatedClock()) as host:
            self.assertEqual(host.snapshot(), blob)

    def test_one_image_in_two_segments_keeps_each_sessions_chart(self):
        for datamodel in ("python", "ecmascript"):
            with self.subTest(datamodel=datamodel):
                logs, clock = Logs(), tinyactors.SimulatedClock()
                document = load("test_snapshot/counter.%s.scxml" % datamodel)
                find(document, "state[@id='s']").insert(0, element("transition", event="stop", target="f"))
                document.append(element("final", id="f"))
                with tinyactors.compile_charts([("counter.scxml", text(document))]) as compiled:
                    image = bytes(compiled.image)
                with tinyactors.Host(CONFIG, clock=clock) as host:
                    host.add_image(image)
                    first = host.spawn(host.chart("counter"))
                    host.add_image(image)  # the same image again: another segment
                    second = host.spawn(host.chart("counter"))
                    host.run_until_idle()
                    blob = host.snapshot()
                with tinyactors.Host.restore(blob, CONFIG, log=logs, clock=clock) as host:
                    host.send(first, "stop")
                    host.run_until_idle()
                    host.run_until_idle()  # the first segment retires
                    host.send(second, "inc", 1)
                    host.send(second, "report", 0)
                    host.run_until_idle()
                    host.snapshot()
                self.assertEqual(len(logs.labelled("n")), 1)
                self.assertTrue(logs.labelled("n")[0].startswith("1 "))

    def test_the_real_clock_resumes_at_the_snapshots_time(self):
        """A snapshot taken at time 0 of a simulated clock, restored on the
        real clock (its ticks since boot): the delayed send is still a
        second away."""
        logs = Logs()
        with host_of(COUNTER["python"]) as host:
            session = host.spawn(host.chart("counter"))
            host.send(session, "wait")
            host.run_until_idle()
            blob = host.snapshot()
        with tinyactors.Host.restore(blob, CONFIG, log=logs) as host:
            host.run_until_idle()
            self.assertEqual(logs.labelled("late"), [])
            host.run()  # sleeps to the deadline: about a second
        self.assertEqual(logs.labelled("late"), ["{'k': 0}"])

    def test_a_value_nests_as_deep_in_a_snapshot_as_in_a_send(self):
        deep = 1
        for _ in range(62):
            deep = [deep]
        logs = Logs()
        with host_of(COUNTER["python"]) as host:
            session = host.spawn(host.chart("counter"))
            host.send(session, "inc", deep)
            host.run_until_idle()
            blob = host.snapshot()
        with tinyactors.Host.restore(blob, CONFIG, log=logs, clock=tinyactors.SimulatedClock()) as host:
            host.send(session, "report", 0)
            host.run_until_idle()
        self.assertTrue(logs.labelled("n")[0].startswith("1 [[[["))

    def test_restore_refuses_another_systems_config(self):
        with host_of(COUNTER["python"]) as host:
            blob = host.snapshot()
        with self.assertRaisesRegex(ValueError, "system t"):
            tinyactors.Host.restore(blob, {"systemID": "other"})

    def test_a_snapshot_with_a_reply_pending_is_refused(self):
        with host_of(COUNTER["python"]) as host:
            host.host_stats(lambda host, result: None)
            with self.assertRaises(tinyactors.TinyactorsError) as raised:
                host.snapshot()
            self.assertEqual(raised.exception.status, "TA_ERROR_STATE")
            host.run_until_idle()
            host.snapshot()


if __name__ == "__main__":
    unittest.main()
