"""The Host, the loop and the effects: the rules the suite cannot see
(AGENTS.md, "Rules the suite cannot see")."""

import gc
import sys
import threading
import unittest
import weakref

import tinyactors
from support import APP, Logs, fixture, host_with

# A chart that sends `out` with its event's data to the application's
# processor, and logs what the answer brings.
ECHO = fixture("test_host/echo.python.scxml")


def spawn(host, name="echo", **options):
    return host.spawn(host.chart(name), **options)


class Views(unittest.TestCase):
    def test_a_view_is_stale_after_its_ack(self):
        kept = []

        def processor(host, send):
            kept.append(send)
            self.assertEqual(send.event, "out")

        host = host_with([("echo.scxml", ECHO)], {APP: processor})
        host.send(spawn(host), "go", {"n": 1})
        host.run_until_idle()
        self.assertEqual(kept[0].event, "out")  # cached: read in the handler
        with self.assertRaises(tinyactors.TinyactorsError) as raised:
            kept[0].data  # never read before the ack
        self.assertEqual(raised.exception.status, "TA_ERROR_STALE")
        with self.assertRaises(tinyactors.TinyactorsError):
            kept[0].answer("ack")
        host.close()

    def test_fields_stay_valid_after_later_calls_in_the_batch(self):
        """One drain at a time: a typed method a handler calls is answered in
        the next take of the same turn, and the view stays readable."""
        seen = []

        def processor(host, send):
            host.host_stats(lambda host, result: seen.append(("stats", result["host"]["actors"])))
            host.send(send.session, "ack", {"later": True})
            seen.append(("view", send.event, send.data, send.session))

        logs = Logs()
        host = host_with([("echo.scxml", ECHO)], {APP: processor}, log=logs)
        session = spawn(host)
        host.send(session, "go", [1, 2])
        host.run_until_idle()
        self.assertEqual(seen, [("view", "out", [1, 2], session), ("stats", 1)])
        self.assertEqual(logs.labelled("ack"), ["{'later': True}"])
        host.close()

    def test_answer_and_its_data(self):
        def processor(host, send):
            self.assertEqual((send.type, send.target, send.sendid), (APP, "app", None))
            send.answer("ack", {"echo": send.data})

        logs = Logs()
        host = host_with([("echo.scxml", ECHO)], {APP: processor}, log=logs)
        host.send(spawn(host), "go", {"k": [1]})
        host.run_until_idle()
        self.assertEqual(logs.labelled("ack"), ["{'echo': {'k': [1]}}"])
        host.close()

    def test_send_data_is_the_payload_value_and_dropped_after_the_call(self):
        kept = []

        def processor(host, send):
            kept.append(send.data)
            kept.append(sys.getrefcount(send.data))

        host = host_with([("echo.scxml", ECHO)], {APP: processor})
        host.send(spawn(host), "go", ["value"])
        host.run_until_idle()
        # the row held a reference during the handler; it was dropped after
        # the ack, outside core (the deferred drop)
        self.assertLess(sys.getrefcount(kept[0]), kept[1])
        host.close()

    def test_a_log_entry_is_a_thin_view(self):
        entries = []
        doc = fixture("test_host/log.python.scxml")
        host = host_with([("c.scxml", doc)], log=lambda host, entry: entries.append((entry, entry.label)))
        session = spawn(host, "c")
        host.run_until_idle()
        entry, label = entries[0]
        self.assertEqual(label, "l")
        with self.assertRaises(tinyactors.TinyactorsError):
            entry.text
        self.assertEqual(entry.label, "l")
        self.assertTrue(session)
        host.close()


class Bounds(unittest.TestCase):
    def test_a_stale_view_never_answers_whatever_was_read(self):
        kept = []

        def processor(host, send):
            (send.session, send.sendid, send.type, send.traceparent)  # every field answer() needs
            kept.append(send)

        host = host_with([("echo.scxml", ECHO)], {APP: processor})
        host.send(spawn(host), "go")
        host.run_until_idle()
        with self.assertRaises(tinyactors.TinyactorsError) as raised:
            kept[0].answer("ack")
        self.assertEqual(raised.exception.status, "TA_ERROR_STALE")
        host.close()

    def test_a_dropped_host_is_freed_without_the_collector(self):
        marker = type("Marker", (), {})()
        reference = weakref.ref(marker)
        gc.disable()
        try:
            host = host_with([("echo.scxml", ECHO)], log=lambda h, e, marker=marker: marker)
            spawn(host)
            host.run_until_idle()
            del marker, host
            self.assertIsNone(reference())
        finally:
            gc.enable()

    def test_send_many_in_slices(self):
        counter = fixture("test_host/counter.python.scxml")
        logs = Logs()
        host = tinyactors.Host({"systemID": "t", "system": {"maxEvents": 4096}}, log=logs)
        with tinyactors.compile_charts([("counter.scxml", counter)]) as compiled:
            host.add_image(compiled.image)
        session = host.spawn(host.chart("counter"))
        self.assertEqual(host.send_many([(session, "inc")] * 1000 + [(session, "show")]), 1001)
        host.run_until_idle()
        self.assertEqual(logs.labelled("n"), ["1000"])
        with self.assertRaises(TypeError):
            host.send_many([(session, "inc")] * 300 + [("bad",)])
        host.close()


class Processors(unittest.TestCase):
    def test_a_processor_that_raises_fails_its_send(self):
        def processor(host, send):
            raise ValueError("no")

        logs = Logs()
        host = host_with([("echo.scxml", ECHO)], {APP: processor}, log=logs)
        host.send(spawn(host), "go")
        with self.assertRaises(ValueError):
            host.run_until_idle()
        host.run_until_idle()  # the failure is the sender's error.communication
        self.assertEqual(len(logs.labelled("failed")), 1)
        host.close()

    def test_a_processor_without_a_handler_fails_its_send(self):
        logs = Logs()
        host = host_with([("echo.scxml", ECHO)], {}, log=logs)
        host.send(spawn(host), "go")
        host.run_until_idle()
        self.assertEqual(len(logs.labelled("failed")), 1)
        host.close()

    def test_an_async_processor_is_refused_under_with(self):
        async def processor(host, send):
            pass

        host = host_with([("echo.scxml", ECHO)], {APP: processor})
        with self.assertRaises(TypeError) as raised:
            with host:
                pass
        self.assertIn(APP, str(raised.exception))
        with self.assertRaises(TypeError):
            host.run_until_idle()
        host.close()

    def test_a_handler_for_a_type_the_config_lacks_is_refused(self):
        with self.assertRaises(ValueError):
            tinyactors.Host({"systemID": "t"}, {"urn:nope": print})


class Methods(unittest.TestCase):
    def test_an_error_goes_to_on_error_or_to_the_driver(self):
        errors = []
        host = host_with([("echo.scxml", ECHO)])
        host.actor_get(print, lambda host, error: errors.append((error.code, error.reason, error.path)), session=99)
        host.run_until_idle()
        self.assertEqual(errors, [(-32001, "stale", "params.session")])
        host.actor_get(print, session=99)
        with self.assertRaises(tinyactors.TinyactorsError) as raised:
            host.run_until_idle()
        self.assertEqual(raised.exception.reason, "stale")
        host.close()

    def test_on_error_by_keyword_is_the_error_handler_not_a_param(self):
        errors = []
        host = host_with([("echo.scxml", ECHO)])
        host.actor_get(print, on_error=lambda host, error: errors.append(error.reason), session=99)
        host.run_until_idle()
        self.assertEqual(errors, ["stale"])
        with self.assertRaises(TypeError):
            host.actor_get(print, print, on_error=print, session=99)
        host.close()

    def test_params_pass_through_to_core(self):
        errors = []
        host = host_with([("echo.scxml", ECHO)])
        host.actor_list(print, lambda host, error: errors.append(error.path), x=1)
        host.actor_list(print, lambda host, error: errors.append(error.path), limit="x")
        host.run_until_idle()
        self.assertEqual(errors, ["params.x", "params.limit"])
        host.close()

    def test_pending_handlers_are_dropped_at_close(self):
        called = []
        host = host_with([("echo.scxml", ECHO)])
        host.host_stats(lambda host, result: called.append(result))
        host.close()
        self.assertEqual(called, [])
        with self.assertRaises(tinyactors.TinyactorsError):
            host.host_stats(print)


class Lifetime(unittest.TestCase):
    def test_handler_cycles_are_collected(self):
        logs_ref = []

        def build():
            holder = {}
            host = host_with([("echo.scxml", ECHO)], log=lambda h, e: holder.get("host"))
            holder["host"] = host  # host -> log handler -> holder -> host
            marker = type("Marker", (), {})()
            holder["marker"] = marker
            logs_ref.append(weakref.ref(marker))

        build()
        gc.collect()
        self.assertIsNone(logs_ref[0]())

    def test_close_from_a_handler_waits_for_the_loop(self):
        def processor(host, send):
            host.close()
            self.assertFalse(host.closed)

        host = host_with([("echo.scxml", ECHO)], {APP: processor})
        host.send(spawn(host), "go")
        host.run_until_idle()
        self.assertTrue(host.closed)

    def test_another_thread_is_refused(self):
        host = host_with([("echo.scxml", ECHO)])
        caught = []
        thread = threading.Thread(target=lambda: caught.append(self.assertRaises(tinyactors.TinyactorsError, host.chart, "echo")))
        thread.start()
        thread.join()
        host.close()

    def test_a_nested_call_and_an_exception_leave_the_host_usable(self):
        def processor(host, send):
            with self_test.assertRaises(TypeError):
                host.send(send.session, "ack", object())  # not of the value table
            host.send(send.session, "ack", {"nested": [1, 2, 3]})

        self_test = self
        logs = Logs()
        host = host_with([("echo.scxml", ECHO)], {APP: processor}, log=logs)
        host.send(spawn(host), "go")
        host.run_until_idle()
        self.assertEqual(logs.labelled("ack"), ["{'nested': [1, 2, 3]}"])
        host.close()

    def test_a_refused_config_raises_cores_error(self):
        with self.assertRaises(tinyactors.TinyactorsError) as raised:
            tinyactors.Host({"systemID": "t", "x": 1})
        self.assertEqual((raised.exception.code, raised.exception.reason, raised.exception.path), (-32602, "unknown", "x"))


class Inputs(unittest.TestCase):
    def test_send_many_and_spawn_params(self):
        counter = fixture("test_host/counter-params.python.scxml")
        logs = Logs()
        host = host_with([("counter.scxml", counter)], log=logs)
        session = host.spawn(host.chart("counter"), params={"n": 10})
        self.assertEqual(host.send_many([(session, "inc"), (session, "inc", 5), (session, "show")]), 3)
        host.run_until_idle()
        self.assertEqual(logs.labelled("n"), ["16"])
        host.close()

    def test_a_send_to_no_session_is_refused(self):
        host = host_with([("echo.scxml", ECHO)])
        with self.assertRaises(tinyactors.TinyactorsError):
            host.send(12345, "go", {"x": 1})
        host.close()

    def test_answer_with_origin_and_raw(self):
        def processor(host, send):
            send.answer("ack", origin="urn:test:back", raw="the message")

        logs = Logs()
        host = host_with([("answered.scxml", fixture("test_host/answered.python.scxml"))], {APP: processor}, log=logs)
        spawn(host, "answered")
        host.run_until_idle()
        self.assertEqual(logs.labelled("ack"), ["['urn:test:back', '%s', 'the message']" % APP])
        host.close()

    def test_boot_and_next_deadline(self):
        """boot spawns the image's boot actor (the first chart compiled)
        under its name; next_deadline is its delayed send, which a
        SimulatedClock reaches by that increment."""
        logs, clock = Logs(), tinyactors.SimulatedClock()
        config = {"systemID": "t", "system": {"ticksPerSecond": 1000}}
        with tinyactors.Host(config, log=logs, clock=clock) as host:
            self.assertIsNone(host.next_deadline)
            with tinyactors.compile_charts([("boot.scxml", fixture("test_host/boot.python.scxml"))]) as compiled:
                segment = host.add_image(compiled.image)
            host.boot(segment)
            host.run_until_idle()
            self.assertEqual(logs.labelled("booted"), ["booted"])
            self.assertAlmostEqual(host.next_deadline, 1.5)
            clock.increment(host.next_deadline)
            self.assertEqual(logs.labelled("later"), ["fired"])
            self.assertIsNone(host.next_deadline)
            with self.assertRaises(tinyactors.TinyactorsError):
                host.boot(segment + 1)
            with self.assertRaises(TypeError):
                host.boot("0")

    def test_terminate(self):
        host = host_with([("echo.scxml", ECHO)])
        session = spawn(host)
        host.terminate(session)
        host.run_until_idle()
        got = []
        host.actor_list(lambda host, result: got.append(result["sessions"]))
        host.run_until_idle()
        self.assertEqual(got, [[]])
        host.close()


class OtherSystems(unittest.TestCase):
    """The I/O processor the config marks remote carries sends to other
    systems; what arrives from one is Host.send by address with its origin."""

    REMOTE = "urn:test:remote"
    SCXML = "http://www.w3.org/TR/scxml/#SCXMLEventProcessor"

    def host(self, sends, logs):
        config = {"systemID": "a", "system": {"ticksPerSecond": 1000},
                  "ioProcessors": [{"type": self.REMOTE, "remote": True}]}
        host = tinyactors.Host(config, {self.REMOTE: lambda host, send: sends.append(
            (send.target, send.event, send.origin, send.data, send.type))}, log=logs,
            clock=tinyactors.SimulatedClock())
        with tinyactors.compile_charts([("remote.scxml", fixture("test_host/remote.python.scxml"))]) as compiled:
            host.add_image(compiled.image)
        return host

    def test_a_send_to_another_system_and_the_reply_to_its_origin(self):
        sends, logs = [], Logs()
        with self.host(sends, logs) as host:
            session = host.spawn(host.chart("talker"), name="talker")
            host.send(session, "call")
            host.run_until_idle()
            self.assertEqual(sends, [("tinyactors://b/#_scxml_5", "hello", "tinyactors://a/#_scxml_%d" % session,
                                      {"n": 41}, self.REMOTE)])
            sends.clear()
            host.send("tinyactors://a/@talker", "hello", [1], origin="tinyactors://b/#_scxml_5")
            host.run_until_idle()
            self.assertEqual(logs.labelled("got"), ["tinyactors://b/#_scxml_5 %s [1]" % self.SCXML])
            self.assertEqual([(target, event) for target, event, *_ in sends], [("tinyactors://b/#_scxml_5", "thanks")])
            host.send("tinyactors://a/#_scxml_%d" % session, "hello", origin="tinyactors://c/@x")
            host.run_until_idle()
            self.assertEqual(logs.labelled("got")[-1], "tinyactors://c/@x %s None" % self.SCXML)

    def test_send_is_the_way_in_and_actor_send_is_not_offered(self):
        """A program sends with Host.send; actor.send is for a client outside
        the process, so it has no typed method here."""
        self.assertFalse(hasattr(tinyactors.Host, "actor_send"))

    def test_an_address_that_names_nothing_here_is_refused(self):
        with self.host([], Logs()) as host:
            for address, status in (("tinyactors://b/#_scxml_1", "TA_ERROR_INVALID"),
                                    ("tinyactors://a/@nobody", "TA_ERROR_STALE"),
                                    ("not an address", "TA_ERROR_INVALID")):
                with self.subTest(address=address):
                    with self.assertRaises(tinyactors.TinyactorsError) as raised:
                        host.send(address, "hello")
                    self.assertEqual(raised.exception.status, status)


class Traces(unittest.TestCase):
    TRACEPARENT = "00-0102030405060708090a0b0c0d0e0f10-a1a2a3a4a5a6a7a8-01"

    def test_traces_cross(self):
        relay = fixture("test_host/relay.python.scxml")
        seen = []
        mode = {"keep": True}

        def processor(host, send):
            seen.append((send.event, send.traceparent))
            if send.event == "first":
                if mode["keep"]:
                    send.answer("ack")
                else:
                    send.answer("ack", traceparent=None)

        host = host_with([("relay.scxml", relay)], {APP: processor})
        session = spawn(host, "relay")
        host.send(session, "go", traceparent=self.TRACEPARENT)
        host.run_until_idle()
        trace = self.TRACEPARENT.split("-")[1]
        self.assertEqual([event for event, _ in seen], ["first", "second"])
        self.assertEqual(seen[0][1].split("-")[1], trace)  # the send runs in the trace the input started
        self.assertEqual(seen[1][1].split("-")[1], trace)  # answer() keeps it
        seen.clear()
        mode["keep"] = False
        host.send(session, "go", traceparent=self.TRACEPARENT)
        host.run_until_idle()
        self.assertEqual(seen[0][1].split("-")[1], trace)
        self.assertNotEqual(seen[1][1].split("-")[1], trace)  # traceparent=None: a new trace
        host.close()


class Requests(unittest.TestCase):
    def test_a_fetch_at_run_time_is_refused_in_c(self):
        parent = fixture("test_host/parent.python.scxml")
        logs = Logs()
        host = host_with([("parent.scxml", parent)], log=logs)
        spawn(host, "parent")
        host.run_until_idle()
        self.assertEqual(logs.labelled("refused"), ["yes"])
        host.close()


if __name__ == "__main__":
    unittest.main()
