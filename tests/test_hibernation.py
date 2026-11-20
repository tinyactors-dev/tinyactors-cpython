"""Hibernation: the store and the resident bound, and the operator's
sweep, hibernate and wake, on both datamodels."""

import gc
import os
import tempfile
import unittest

import tinyactors
from support import APP, Logs, element, find, fixture, host_with, load, text

COUNTER = {datamodel: fixture("test_hibernation/counter.%s.scxml" % datamodel) for datamodel in ("python", "ecmascript")}


class Store:
    """A store over a dict that records the sessions the host loads."""

    def __init__(self):
        self.blobs = {}
        self.wakes = []

    def save(self, session, blob):
        self.blobs[session] = blob

    def load(self, session):
        self.wakes.append(session)
        return self.blobs[session]

    def delete(self, session):
        self.blobs.pop(session, None)


class Hibernation(unittest.TestCase):
    def roundtrip(self, datamodel):
        logs, store = Logs(), Store()
        with host_with([("counter.scxml", COUNTER[datamodel])], log=logs, store=store) as host:
            session = host.spawn(host.chart("counter"))
            host.send(session, "inc", {"k": 1})
            host.send(session, "inc", "two")
            host.run_until_idle()
            blob = host.hibernate(session)
            self.assertIsInstance(blob, bytes)
            store.save(session.id, blob)
            host.send(session, "report")
            host.run_until_idle()
            self.assertEqual(store.wakes, [session.id])
            self.assertEqual(store.blobs, {}, "the blob is spent once the actor is back")
        return logs.labelled("n")

    def test_a_python_actor_wakes_with_its_variables(self):
        self.assertEqual(self.roundtrip("python"), ["2 [{'k': 1}, 'two'] report"])

    def test_an_ecmascript_actor_wakes_with_its_variables(self):
        self.assertEqual(self.roundtrip("ecmascript"), ['2 [{"k":1},"two"] report'])

    def test_an_actor_wakes_with_its_last_event(self):
        """_event stays bound to the last event across hibernation (SCXML
        5.10): the blob carries it, and the <onexit> of an actor terminated
        at rest reads it."""
        exits = {"python": "f'{_event.name} {_event.data}'",
                 "ecmascript": "_event.name + ' ' + JSON.stringify(_event.data)"}
        for datamodel, expected in (("python", "inc {'k': 7}"), ("ecmascript", 'inc {"k":7}')):
            with self.subTest(datamodel=datamodel):
                logs, store = Logs(), Store()
                document = load("test_hibernation/counter.%s.scxml" % datamodel)
                on_exit = element("onexit", element("log", label="exit", expr=exits[datamodel]))
                find(document, "state[@id='s']").insert(0, on_exit)
                with host_with([("counter.scxml", text(document))], log=logs, store=store) as host:
                    session = host.spawn(host.chart("counter"))
                    host.send(session, "inc", {"k": 7})
                    host.run_until_idle()
                    blob = host.hibernate(session)
                    host.wake(session, blob)
                    host.terminate(session)
                    host.run_until_idle()
                self.assertEqual(logs.labelled("exit"), [expected])

    def test_events_wait_until_the_wake(self):
        """Without a store, an actor hibernated by the operator stays so: its
        events wait, and run in order once the application wakes it."""
        logs = Logs()
        with host_with([("counter.scxml", COUNTER["python"])], log=logs) as host:
            session = host.spawn(host.chart("counter"))
            host.run_until_idle()
            blob = host.hibernate(session)
            for i in range(3):
                host.send(session, "inc", i)
            host.send(session, "report")
            host.run_until_idle()
            self.assertEqual(logs.labelled("n"), [])
            host.wake(session, blob)
            host.run_until_idle()
        self.assertEqual(logs.labelled("n"), ["3 [0, 1, 2] report"])

    def test_the_sweep_returns_actors_that_did_not_run_since_the_last_one(self):
        with host_with([("counter.scxml", COUNTER["python"])]) as host:
            ref = host.chart("counter")
            sessions = [host.spawn(ref) for _ in range(4)]
            host.run_until_idle()
            host.sweep(1024, 16)  # clears every actor's bit
            host.send(sessions[1], "inc", 0)
            host.run_until_idle()
            cold = host.sweep(1024, 16)
            self.assertEqual(sorted(cold), sorted(actor.id for actor in sessions[:1] + sessions[2:]))
            self.assertEqual(len(host.sweep(1024, 2)), 2)

    def test_a_delayed_send_wakes_the_actor_it_was_sent_to(self):
        """A pending delayed send stays in the host while its actor is
        hibernated; when it falls due the host asks for the actor (a WAKE)
        and delivers it."""
        logs, store = Logs(), Store()
        clock = tinyactors.SimulatedClock()
        document = load("test_hibernation/counter.python.scxml")
        later = element("transition", element("log", label="later", expr="str(n)"), event="later")
        find(document, "state[@id='s']").append(later)
        with host_with([("counter.scxml", text(document))], log=logs, store=store, clock=clock) as host:
            session = host.spawn(host.chart("counter"))
            host.send(session, "inc", 0)
            host.send(session, "wait")
            host.run_until_idle()
            store.save(session.id, host.hibernate(session))
            clock.increment(0.999)
            self.assertEqual(store.wakes, [])
            clock.increment(0.001)
            self.assertEqual(store.wakes, [session.id])
        self.assertEqual(logs.labelled("later"), ["1"])

    def test_hibernate_refuses_an_actor_with_events_waiting(self):
        with host_with([("counter.scxml", COUNTER["python"])]) as host:
            session = host.spawn(host.chart("counter"))
            host.run_until_idle()
            host.send(session, "inc", 1)
            with self.assertRaises(tinyactors.TinyactorsError):
                host.hibernate(session)
            host.run_until_idle()
            host.hibernate(session)
            with self.assertRaises(tinyactors.TinyactorsError):
                host.hibernate(session)  # already out of memory

    def test_a_value_outside_the_table_keeps_the_actor_resident(self):
        logs = Logs()
        document = fixture("test_hibernation/odd.python.scxml")
        with host_with([("odd.scxml", document)], log=logs) as host:
            session = host.spawn(host.chart("odd"))
            host.send(session, "odd")
            host.run_until_idle()
            with self.assertRaises(tinyactors.TinyactorsError):
                host.hibernate(session)
            host.send(session, "report")
            host.run_until_idle()
        self.assertEqual(logs.labelled("v"), ["[1, 2]"])

    def test_a_hibernated_python_actor_holds_no_python_object(self):
        """Its instance (of the chart's class, named after the chart) and
        its values go: the blob is all that is left."""
        def instances():
            gc.collect()
            return sum(1 for each in gc.get_objects() if type(each).__name__ == "counter")

        with host_with([("counter.scxml", COUNTER["python"])]) as host:
            ref = host.chart("counter")
            sessions = [host.spawn(ref) for _ in range(3)]
            for session in sessions:
                host.send(session, "inc", ["kept"])
            host.run_until_idle()
            self.assertEqual(instances(), 3)
            for session in sessions[:2]:
                host.hibernate(session)
            self.assertEqual(instances(), 1)

    def test_wake_refuses_an_older_blob(self):
        """A store that fell behind hands back an earlier blob: the wake
        refuses it with TA_ERROR_STALE and the actor stays hibernated, then
        wakes from the latest one with its state."""
        for datamodel in ("python", "ecmascript"):
            with self.subTest(datamodel=datamodel):
                logs = Logs()
                with host_with([("counter.scxml", COUNTER[datamodel])], log=logs) as host:
                    session = host.spawn(host.chart("counter"))
                    host.run_until_idle()
                    older = host.hibernate(session)
                    host.wake(session, older)
                    host.send(session, "inc", 1)
                    host.run_until_idle()
                    latest = host.hibernate(session)
                    with self.assertRaises(tinyactors.TinyactorsError) as raised:
                        host.wake(session, older)
                    self.assertEqual(raised.exception.status, "TA_ERROR_STALE")
                    host.wake(session, latest)
                    host.send(session, "report")
                    host.run_until_idle()
                self.assertEqual(logs.labelled("n")[0].split(" ")[0], "1")

    def test_wake_refuses_a_blob_of_another_host(self):
        """Two hosts of one config and one image, each with a session of the
        same number hibernated: neither wakes the other's blob
        (TA_ERROR_INVALID, the host's identity differs), and each wakes its
        own."""
        source = [("counter.scxml", COUNTER["python"])]
        with host_with(source) as first, host_with(source) as second:
            sessions, blobs = [], []
            for host in (first, second):
                sessions.append(host.spawn(host.chart("counter")))
                host.run_until_idle()
                blobs.append(host.hibernate(sessions[-1]))
            self.assertEqual(sessions[0].id, sessions[1].id)
            for host, other, own in ((first, blobs[1], blobs[0]), (second, blobs[0], blobs[1])):
                with self.assertRaises(tinyactors.TinyactorsError) as raised:
                    host.wake(sessions[0], other)
                self.assertEqual(raised.exception.status, "TA_ERROR_INVALID")
                host.wake(sessions[0], own)

    def test_wake_refuses_a_blob_it_cannot_read(self):
        with host_with([("counter.scxml", COUNTER["python"])]) as host:
            session = host.spawn(host.chart("counter"))
            host.run_until_idle()
            host.hibernate(session)
            with self.assertRaises(tinyactors.TinyactorsError):
                host.wake(session, b"\x00not a blob")

    def test_a_store_that_raises_is_raised_by_the_driver(self):
        class Down(Store):
            def load(self, session):
                raise ValueError("store down")

        with host_with([("counter.scxml", COUNTER["python"])], store=Down()) as host:
            session = host.spawn(host.chart("counter"))
            host.run_until_idle()
            host.hibernate(session)
            host.send(session, "inc", 1)
            with self.assertRaises(ValueError):
                host.run_until_idle()

    def test_hibernate_from_a_nested_call_outside_a_turn_leaves_no_batch_open(self):
        """A send_many iterator is a call inside a call, outside any turn:
        the blob's batch is acknowledged there, and the next run is not
        refused."""
        logs, store = Logs(), Store()
        with host_with([("counter.scxml", COUNTER["python"])], log=logs, store=store) as host:
            a, b = host.spawn(host.chart("counter")), host.spawn(host.chart("counter"))
            host.run_until_idle()

            def items():
                store.save(a.id, host.hibernate(a))
                yield (b, "inc", 1)

            self.assertEqual(host.send_many(items()), 1)
            host.run_until_idle()
            host.send(a, "report")
            host.run_until_idle()
        self.assertEqual(logs.labelled("n"), ["0 [] report"])

    def test_hibernate_between_turns_ends_no_effect_undispatched(self):
        """A turn that stops at its bound of batches leaves none taken: a
        blob pulled before the next turn does not acknowledge effects that
        were never dispatched."""
        replies = [0]

        def again(result):
            replies[0] += 1
            if replies[0] < 200:
                host.host_stats(again)

        def boom(result):
            raise RuntimeError("stop the driver")

        with host_with([("counter.scxml", COUNTER["python"])]) as host:
            session = host.spawn(host.chart("counter"))
            host.run_until_idle()
            host.host_stats(boom)
            host.host_stats(again)
            with self.assertRaises(RuntimeError):
                host.run_until_idle()
            host.hibernate(session)
            host.run_until_idle()
        self.assertEqual(replies[0], 200)

    def test_sweep_and_wake_refuse_numbers_out_of_range(self):
        with host_with([("counter.scxml", COUNTER["python"])]) as host:
            for scan, limit in ((10, -1), (10, 2 ** 32 + 3), (-1, 1)):
                with self.assertRaises(OverflowError):
                    host.sweep(scan, limit)
            with self.assertRaises(OverflowError):
                host.wake(-1, b"")



class Resident(unittest.TestCase):
    """store= and resident=: after a turn that leaves more actors in memory
    than the bound, the host hibernates the surplus into the store, and a
    WAKE loads them back."""

    def resident(self, host):
        got = []
        host.host_stats(lambda result: got.append(result["host"]))
        host.run_until_idle()
        return got[0]["actors"] - got[0]["hibernated"]

    def population(self, datamodel, store):
        logs = Logs()
        with host_with([("counter.scxml", COUNTER[datamodel])], log=logs, store=store, resident=3) as host:
            actors = [host.spawn("counter") for _ in range(10)]
            host.run_until_idle()
            self.assertLessEqual(self.resident(host), 3)
            self.assertEqual(len(store.blobs), 7)
            for round in range(2):
                for index, actor in enumerate(actors):
                    actor.send("inc", index)
                    host.run_until_idle()
                    self.assertLessEqual(self.resident(host), 3)
            for actor in actors:
                actor.send("report")
            host.run_until_idle()
            self.assertLessEqual(self.resident(host), 3)
        return logs.labelled("n")

    def test_at_most_resident_actors_stay_in_memory(self):
        for datamodel, n in (("python", "2 [%d, %d] report"), ("ecmascript", "2 [%d,%d] report")):
            with self.subTest(datamodel=datamodel):
                lines = self.population(datamodel, Store())
                self.assertEqual(sorted(lines), sorted(n % (i, i) for i in range(10)))

    def test_memory_store(self):
        store = tinyactors.MemoryStore()
        self.assertEqual(len(self.population("python", store)), 10)
        store.save(1, b"x")
        self.assertEqual(store.load(1), b"x")
        store.delete(1)
        store.delete(1)
        with self.assertRaises(KeyError):
            store.load(1)

    def test_sqlite_store(self):
        with tempfile.TemporaryDirectory() as directory:
            store = tinyactors.SQLiteStore(os.path.join(directory, "actors.db"))
            store.save(2 ** 64 - 1, b"last")
            self.assertEqual(store.load(2 ** 64 - 1), b"last")
            store.delete(2 ** 64 - 1)

            class Counted:
                def __init__(self, inner):
                    self.inner = inner
                    self.blobs = set()

                def save(self, session, blob):
                    self.blobs.add(session)
                    self.inner.save(session, blob)

                def load(self, session):
                    return self.inner.load(session)

                def delete(self, session):
                    self.blobs.discard(session)
                    self.inner.delete(session)
            self.assertEqual(len(self.population("python", Counted(store))), 10)
            store.close()

    def test_an_actor_terminated_in_the_store_is_deleted_from_it(self):
        store = Store()
        with host_with([("counter.scxml", COUNTER["python"])], store=store) as host:
            actor = host.spawn("counter")
            host.run_until_idle()
            store.save(actor.id, host.hibernate(actor))
            actor.terminate()  # core wakes it to end it: the WAKE loads the blob, then deletes it
            self.assertIn(actor.id, store.blobs)
            host.run_until_idle()
            self.assertEqual(store.blobs, {})

    def test_a_save_that_raises_keeps_the_actor_resident(self):
        class Full(Store):
            def save(self, session, blob):
                raise OSError("disk full")

        logs, errors = Logs(), []
        with host_with([("counter.scxml", COUNTER["python"])], log=logs, store=Full(), resident=1) as host:
            actors = [host.spawn("counter") for _ in range(2)]
            with self.assertRaises(OSError):  # without on_handler_error: raised after the turn
                host.run_until_idle()
        with host_with([("counter.scxml", COUNTER["python"])], log=logs, store=Full(), resident=1,
                       on_handler_error=lambda error, effect: errors.append((type(error), effect))) as host:
            actors = [host.spawn("counter") for _ in range(2)]
            host.run_until_idle()
            self.assertEqual(errors[0], (OSError, None))
            self.assertEqual(self.resident(host), 2)
            for actor in actors:
                actor.send("report")
            host.run_until_idle()
        self.assertEqual(logs.labelled("n"), ["0 [] report", "0 [] report"])

    def test_the_arguments_are_checked(self):
        with self.assertRaises(TypeError):
            host_with([("counter.scxml", COUNTER["python"])], resident=3)  # no store
        with self.assertRaises(ValueError):
            host_with([("counter.scxml", COUNTER["python"])], store=Store(), resident=0)
        with self.assertRaises(TypeError):
            host_with([("counter.scxml", COUNTER["python"])], store=object())


if __name__ == "__main__":
    unittest.main()
