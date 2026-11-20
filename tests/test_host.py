"""The Host, the loop and the effects: the rules the suite cannot see
(AGENTS.md, "Rules the suite cannot see")."""

import asyncio
import gc
import sys
import threading
import unittest
import weakref

import tinyactors
from support import APP, Logs, Processor, find, fixture, host_with, load, text

# A chart that sends `out` with its event's data to the application's
# processor, and logs what the answer brings.
ECHO = fixture("test_host/echo.python.scxml")


def spawn(host, name="echo", **options):
    return host.spawn(host.chart(name), **options)


class Views(unittest.TestCase):
    def test_a_view_is_stale_after_its_ack(self):
        kept = []

        def processor(send):
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

        def processor(send):
            host.host_stats(lambda result: seen.append(("stats", result["host"]["actors"])))
            host.send(send.session, "ack", {"later": True})
            seen.append(("view", send.event, send.data, send.session))

        logs = Logs()
        host = host_with([("echo.scxml", ECHO)], {APP: processor}, log=logs)
        session = spawn(host)
        host.send(session, "go", [1, 2])
        host.run_until_idle()
        self.assertEqual(seen, [("view", "out", [1, 2], session.id), ("stats", 1)])
        self.assertEqual(logs.labelled("ack"), ["{'later': True}"])
        host.close()

    def test_answer_and_its_data(self):
        def processor(send):
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

        def processor(send):
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
        host = host_with([("c.scxml", doc)], log=lambda entry: entries.append((entry, entry.label)))
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

        def processor(send):
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
            host = host_with([("echo.scxml", ECHO)], log=lambda e, marker=marker: marker)
            spawn(host)
            host.run_until_idle()
            del marker, host
            self.assertIsNone(reference())
        finally:
            gc.enable()

    def test_send_many_in_slices(self):
        counter = fixture("test_host/counter.python.scxml")
        logs = Logs()
        with tinyactors.compile_charts([("counter.scxml", counter)]) as compiled:
            host = tinyactors.Host(compiled, system="t", config={"system": {"maxEvents": 4096}}, log=logs)
        session = host.spawn("counter")
        self.assertEqual(host.send_many([(session, "inc")] * 1000 + [(session, "show")]), 1001)
        host.run_until_idle()
        self.assertEqual(logs.labelled("n"), ["1000"])
        with self.assertRaises(TypeError):
            host.send_many([(session, "inc")] * 300 + [("bad",)])
        host.close()


class Processors(unittest.TestCase):
    def test_a_processor_that_raises_fails_its_send(self):
        def processor(send):
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
        async def processor(send):
            pass

        host = host_with([("echo.scxml", ECHO)], {APP: processor})
        with self.assertRaises(TypeError) as raised:
            with host:
                pass
        self.assertIn(APP, str(raised.exception))
        with self.assertRaises(TypeError):
            host.run_until_idle()
        host.close()

    def test_a_processor_is_an_object_with_receive(self):
        with self.assertRaises(TypeError) as raised:
            tinyactors.Host([], system="t", io_processors={"urn:fn": print})
        self.assertIn("urn:fn", str(raised.exception))

    def test_the_config_takes_neither_the_system_id_nor_the_processors(self):
        for config in ({"systemID": "t"}, {"ioProcessors": []}):
            with self.subTest(config=config), self.assertRaises(TypeError):
                tinyactors.Host([], system="t", config=config)
        with self.assertRaises(TypeError):
            tinyactors.Host([])  # no system
        with self.assertRaises(ValueError):
            tinyactors.Host([], system="tinyactors-ask")  # ask()'s replies

    def test_processors_make_the_configs_io_processors_in_order(self):
        class Db:
            location = "postgres://db"

            def receive(self, send):
                pass

        got = []
        with tinyactors.Host([], system="t", io_processors={"db": Db(), "ws": Processor(print)}) as host:
            host.host_config(lambda result: got.append(result["ioProcessors"]))
            host.run_until_idle()
        self.assertEqual(got, [[{"type": "db", "location": "postgres://db"}, {"type": "ws", "location": ""}]])


class Methods(unittest.TestCase):
    def test_an_error_goes_to_on_error_or_to_the_driver(self):
        errors = []
        host = host_with([("echo.scxml", ECHO)])
        host.actor_get(print, lambda error: errors.append((error.code, error.reason, error.path)), session=99)
        host.run_until_idle()
        self.assertEqual(errors, [(-32001, "stale", "params.session")])
        host.actor_get(print, session=99)
        with self.assertRaises(tinyactors.TinyactorsError) as raised:
            host.run_until_idle()
        self.assertEqual(raised.exception.reason, "stale")
        host.close()

    def test_actor_get_lists_delayed_sends_due_in_seconds(self):
        """actor.get's timers are due in seconds of the host's clock, the
        API's unit of time; actor.state's history says where h returns."""
        results = []
        host = host_with([("timed.scxml", fixture("test_host/timed.python.scxml"))])
        session = spawn(host, "timed")
        host.run_until_idle()
        host.actor_get(lambda result: results.append(result), session=session.id)
        host.run_until_idle()
        self.assertEqual(results[0]["timers"], [{"sendid": None, "event": "tick", "due": 1.5},
                                                {"sendid": "sla", "event": "breach", "due": 60.0}])
        self.assertEqual(results[0]["timersPending"], 2)
        host.send(session, "next")
        host.send(session, "pause")
        host.run_until_idle()  # a typed method reads the host at its call
        host.actor_state(lambda result: results.append(result), session=session.id)
        host.run_until_idle()
        self.assertEqual(results[1], {"value": "paused", "history": {"h": {"work": {"a": "a2"}}}})
        host.close()

    def test_on_error_by_keyword_is_the_error_handler_not_a_param(self):
        errors = []
        host = host_with([("echo.scxml", ECHO)])
        host.actor_get(print, on_error=lambda error: errors.append(error.reason), session=99)
        host.run_until_idle()
        self.assertEqual(errors, ["stale"])
        with self.assertRaises(TypeError):
            host.actor_get(print, print, on_error=print, session=99)
        host.close()

    def test_params_pass_through_to_core(self):
        errors = []
        host = host_with([("echo.scxml", ECHO)])
        host.actor_list(print, lambda error: errors.append(error.path), x=1)
        host.actor_list(print, lambda error: errors.append(error.path), limit="x")
        host.run_until_idle()
        self.assertEqual(errors, ["params.x", "params.limit"])
        host.close()

    def test_pending_handlers_are_dropped_at_close(self):
        called = []
        host = host_with([("echo.scxml", ECHO)])
        host.host_stats(lambda result: called.append(result))
        host.close()
        self.assertEqual(called, [])
        with self.assertRaises(tinyactors.TinyactorsError):
            host.host_stats(print)


class Lifetime(unittest.TestCase):
    def test_handler_cycles_are_collected(self):
        logs_ref = []

        def build():
            holder = {}
            host = host_with([("echo.scxml", ECHO)], log=lambda e: holder.get("host"))
            holder["host"] = host  # host -> log handler -> holder -> host
            marker = type("Marker", (), {})()
            holder["marker"] = marker
            logs_ref.append(weakref.ref(marker))

        build()
        gc.collect()
        self.assertIsNone(logs_ref[0]())

    def test_close_from_a_handler_waits_for_the_loop(self):
        def processor(send):
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
        def processor(send):
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
            tinyactors.Host([], system="t", config={"x": 1})
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
        def processor(send):
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
        config = {"system": {"ticksPerSecond": 1000}}
        with tinyactors.Host([], system="t", config=config, log=logs, clock=clock) as host:
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
        host.actor_list(lambda result: got.append(result["sessions"]))
        host.run_until_idle()
        self.assertEqual(got, [[]])
        host.close()


FINISHING = fixture("test_host/finishing.python.scxml")


class Actors(unittest.TestCase):
    """Actor: what spawn and actor() return, send.sender, on_done, done."""

    def test_an_actor_is_a_value_of_its_session(self):
        host = host_with([("finishing.scxml", FINISHING)])
        actor = host.spawn("finishing", name="f1")
        self.assertIsInstance(actor, tinyactors.Actor)
        self.assertEqual(actor, host.actor(actor.id))
        self.assertEqual(hash(actor), hash(host.actor("f1")))
        self.assertEqual(len({actor, host.actor("f1"), host.actor(actor.address)}), 1)
        self.assertEqual(actor.address, tinyactors.Address("tinyactors://t/#_scxml_%d" % actor.id))
        self.assertEqual(host.system, "t")
        self.assertIs(actor.host, host)
        with self.assertRaises(AttributeError):
            actor.id = 2
        self.assertEqual(host.spawn(host.chart("finishing")).id, actor.id + 1)  # a ChartRef too
        with self.assertRaises(tinyactors.TinyactorsError):
            host.spawn("nothing")  # no such chart, as Host.chart
        with self.assertRaises(TypeError):
            host.spawn(1)
        host.close()

    def test_on_done_gets_every_top_level_actor_that_ends(self):
        done, sent = [], []
        host = host_with([("finishing.scxml", FINISHING)], {APP: lambda send: sent.append((send.sender, send.host))},
                         on_done=lambda actor, data: done.append((actor, data)))
        actor = host.spawn("finishing")
        actor.send("ping", [1])
        host.run_until_idle()
        self.assertEqual(sent, [(actor, host)])
        actor.send("finish", {"total": 3})
        host.run_until_idle()
        self.assertEqual(done, [(actor, {"total": 3})])
        with self.assertRaises(tinyactors.StaleError):
            host.actor(actor.id)
        host.close()

    def test_actor_awaitables_under_asyncio(self):
        async def main():
            with tinyactors.compile_charts([("finishing.scxml", FINISHING)]) as compiled:
                image = compiled.image
            async with tinyactors.Host(image, system="t", io_processors={APP: Processor(lambda send: None)}) as host:
                first, second = host.spawn("finishing"), host.spawn("finishing", name="second")
                await host.idle()
                state = await first.state()
                context = await first.context()
                got = await first.get()
                listed = await host.actors()
                named = await host.actors(chart="finishing")
                none = await host.actors(chart="other")
                waiting = first.done
                first.send("finish", "bye")
                return state["value"], context, got["session"], listed, named, none, await waiting, second

        state, context, session, listed, named, none, donedata, second = asyncio.run(main())
        self.assertEqual((state, session, donedata), ("s", 1, "bye"))
        self.assertEqual(listed, named)
        self.assertEqual([actor.id for actor in listed], [1, 2])
        self.assertEqual(none, [])

    def test_actor_awaitables_need_asyncio(self):
        host = host_with([("finishing.scxml", FINISHING)])
        actor = host.spawn("finishing")
        for call in (actor.state, actor.context, actor.get, lambda: actor.done, host.actors):
            with self.assertRaises(TypeError):
                call()
        host.close()


class Ask(unittest.TestCase):
    """ask(): a request and its reply, over a send whose origin is an
    address of the host's own; the reply never reaches the transport."""

    def run_asks(self, datamodel, main):
        document = fixture("test_host/asked.%s.scxml" % datamodel)
        transported = []

        async def run():
            with tinyactors.compile_charts([("asked.scxml", document)]) as compiled:
                image = compiled.image
            async with tinyactors.Host(image, system="t", transport=lambda send: transported.append(send.event)) as host:
                return await main(host)

        result = asyncio.run(run())
        self.assertEqual(transported, [])
        return result

    def test_an_actor_answers_to_its_origin(self):
        async def main(host):
            actor = host.spawn("asked")
            first = await actor.ask("question", {"n": 1})
            both = await asyncio.gather(actor.ask("question", 2), actor.ask("question", "x"))
            return first, both

        for datamodel in ("python", "ecmascript"):
            with self.subTest(datamodel=datamodel):
                first, both = self.run_asks(datamodel, main)
                self.assertEqual(first, ("answer", {"echo": {"n": 1}}))
                self.assertEqual(both, [("answer", {"echo": 2}), ("answer", {"echo": "x"})])

    def test_a_cancelled_ask_drops_its_row_and_a_closed_host_its_asks(self):
        pending = []

        async def main(host):
            actor = host.spawn("asked")
            with self.assertRaises(asyncio.TimeoutError):
                await asyncio.wait_for(actor.ask("silent"), 0.01)
            pending.append(actor.ask("silent"))
            return await actor.ask("question", 3)

        self.assertEqual(self.run_asks("python", main), ("answer", {"echo": 3}))
        self.assertFalse(pending[0].done())  # pending at close: it never completes

    def test_ask_needs_asyncio_and_a_live_actor(self):
        host = host_with([("asked.scxml", fixture("test_host/asked.python.scxml"))])
        actor = host.spawn("asked")
        with self.assertRaises(TypeError):
            actor.ask("question")
        host.close()

        async def main(host):
            actor = host.spawn("asked")
            actor.terminate()
            await host.idle()
            with self.assertRaises(tinyactors.StaleError):
                actor.ask("question")
            return True

        self.assertTrue(self.run_asks("python", main))


class Spawning(unittest.TestCase):
    def test_a_chart_spawns_through_a_spawner(self):
        logs = Logs()
        documents = [("workspace.scxml", fixture("test_host/workspace.python.scxml")),
                     ("room.scxml", fixture("test_host/room.python.scxml"))]
        host = host_with(documents, {"spawn": tinyactors.Spawner()}, log=logs)
        workspace = host.spawn("workspace")
        workspace.send("make", {"chart": "room", "name": "lobby", "params": {"topic": "news"}})
        host.run_until_idle()
        room = host.actor("lobby")
        self.assertEqual(logs.labelled("spawned"), ["s1 lobby %s" % room.address.text])
        self.assertEqual(logs.labelled("room"), ["lobby news"])
        for data in ({"chart": "nothing"}, ["room"], {"name": "x"}):
            workspace.send("make", data)
        host.run_until_idle()
        self.assertEqual(logs.labelled("failed"), ["s1"] * 3)
        host.close()

    def test_a_spawn_after_a_deploy_runs_the_new_version_under_the_name(self):
        logs = Logs()
        documents = [("workspace.scxml", fixture("test_host/workspace.python.scxml")),
                     ("room.scxml", fixture("test_host/room.python.scxml"))]
        host = host_with(documents, {"spawn": tinyactors.Spawner()}, log=logs)
        workspace = host.spawn("workspace")
        lobby = {"chart": "room", "name": "lobby", "params": {"topic": "news"}}
        workspace.send("make", lobby)
        host.run_until_idle()
        old = host.actor("lobby")
        version2 = load("test_host/room.python.scxml")
        find(version2, ".//log").set("label", "room2")
        with tinyactors.compile_charts([("room.scxml", text(version2))]) as compiled:
            host.add_image(compiled.image)  # the deploy
        workspace.send("make", lobby)  # the workspace, the name's one owner, spawns it again
        host.run_until_idle()
        new = host.actor("lobby")  # names are looked up newest version first
        self.assertNotEqual(new, old)
        self.assertEqual(logs.labelled("spawned")[1], "s1 lobby %s" % new.address.text)
        self.assertEqual(logs.labelled("room2"), ["lobby news"])
        old.send("hello")  # the old actor keeps its session and its version
        host.run_until_idle()
        self.assertEqual(logs.labelled("room"), ["lobby news"] * 2)
        host.close()


class HandlerErrors(unittest.TestCase):
    def test_on_handler_error_gets_the_error_and_its_effect(self):
        got, logs = [], Logs()

        def processor(send):
            raise ValueError("no")

        def log(entry):
            raise KeyError(entry.label)

        host = host_with([("echo.scxml", ECHO)], {APP: processor}, log=log,
                         on_handler_error=lambda error, effect: got.append((type(error), type(effect).__name__)))
        host.send(spawn(host), "go")
        host.run_until_idle()  # nothing raised: the hook has it
        self.assertEqual(got, [(ValueError, "Send"), (KeyError, "LogEntry")])  # the send failed: error.communication
        host.close()

    def test_a_hook_that_raises_is_raised_by_the_driver(self):
        def hook(error, effect):
            raise RuntimeError("hook")

        host = host_with([("echo.scxml", ECHO)], {APP: lambda send: 1 / 0}, on_handler_error=hook)
        host.send(spawn(host), "go")
        with self.assertRaises(RuntimeError):
            host.run_until_idle()
        host.close()


class OtherSystems(unittest.TestCase):
    """The host's transport carries sends to other systems, with no config;
    what arrives from one is Host.send by address with its origin."""

    SCXML = "http://www.w3.org/TR/scxml/#SCXMLEventProcessor"

    def host(self, sends, logs, transport=True):
        def handler(send):
            sends.append((send.target, send.event, send.origin, send.data, send.type))
        with tinyactors.compile_charts([("remote.scxml", fixture("test_host/remote.python.scxml"))]) as compiled:
            return tinyactors.Host(compiled, system="a", config={"system": {"ticksPerSecond": 1000}}, log=logs,
                                   clock=tinyactors.SimulatedClock(), transport=handler if transport else None)

    def test_a_send_to_another_system_and_the_reply_to_its_origin(self):
        sends, logs = [], Logs()
        with self.host(sends, logs) as host:
            session = host.spawn(host.chart("talker"), name="talker")
            host.send(session, "call")
            host.run_until_idle()
            self.assertEqual(sends, [(tinyactors.Address("tinyactors://b/#_scxml_5"), "hello",
                                      tinyactors.Address("tinyactors://a/#_scxml_%d" % session.id), {"n": 41}, None)])
            sends.clear()
            host.send("tinyactors://a/@talker", "hello", [1], origin="tinyactors://b/#_scxml_5")
            host.run_until_idle()
            self.assertEqual(logs.labelled("got"), ["tinyactors://b/#_scxml_5 %s [1]" % self.SCXML])
            self.assertEqual([(target.text, event) for target, event, *_ in sends], [("tinyactors://b/#_scxml_5", "thanks")])
            host.send("tinyactors://a/#_scxml_%d" % session.id, "hello", origin="tinyactors://c/@x")
            host.run_until_idle()
            self.assertEqual(logs.labelled("got")[-1], "tinyactors://c/@x %s None" % self.SCXML)

    def test_a_message_crosses_whole_and_a_failure_is_reported_by_fail(self):
        """send.message is what Host.send takes on the other side: every
        field, the sendid included; a failure comes back through Host.fail."""
        messages, logs = [], Logs()

        def transport(send):
            messages.append(tinyactors.decode(tinyactors.encode(send.message)))  # through a wire

        with tinyactors.compile_charts([("remote.scxml", fixture("test_host/remote.python.scxml"))]) as compiled:
            host = tinyactors.Host(compiled, system="a", config={"system": {"ticksPerSecond": 1000}}, log=logs,
                                   clock=tinyactors.SimulatedClock(), transport=transport)
        with host:
            session = host.spawn(host.chart("talker"), name="talker")
            host.send(session, "call")
            host.run_until_idle()
            (message,) = messages
            self.assertEqual(set(message), {"target", "event", "data", "sendid", "origin", "traceparent"})
            self.assertEqual((message["target"].system, message["target"].session), ("b", 5))
            self.assertEqual(message["origin"], tinyactors.Address("tinyactors://a/#_scxml_%d" % session.id))
            self.assertEqual(message["sendid"], "h1")
            # the other side's answer, as a message to our talker: unknown keys are ignored
            host.send({"target": message["origin"], "event": "hello", "data": [2], "sendid": "r1",
                       "origin": message["target"], "a later key": True})
            host.run_until_idle()
            self.assertEqual(logs.labelled("got")[-1], "tinyactors://b/#_scxml_5 %s [2]" % self.SCXML)
            # another binding's absent fields decode as UNDEFINED: absent, as None is
            host.send({"target": message["origin"], "event": "hello", "data": [3], "sendid": tinyactors.UNDEFINED,
                       "origin": tinyactors.UNDEFINED, "traceparent": tinyactors.UNDEFINED})
            host.run_until_idle()
            self.assertEqual(logs.labelled("got")[-1], "None None [3]")
            host.fail(message["origin"], message["sendid"])
            host.run_until_idle()
            self.assertEqual(logs.labelled("failed"), ["error.communication h1"])

    def test_an_address_has_parts(self):
        Address = tinyactors.Address
        self.assertEqual((Address("tinyactors://b/#_scxml_5").system, Address("tinyactors://b/#_scxml_5").session,
                          Address("tinyactors://b/#_scxml_5").name), ("b", 5, None))
        self.assertEqual((Address("tinyactors://tab-7f3/@p%C3%B6ng").session,
                          Address("tinyactors://tab-7f3/@p%C3%B6ng").name), (None, "pöng"))
        self.assertEqual((Address("http://x/").system, Address("http://x/").session, Address("http://x/").name),
                         (None, None, None))

    def test_without_a_transport_the_sender_is_told(self):
        logs = Logs()
        with self.host([], logs, transport=False) as host:
            host.send(host.spawn(host.chart("talker"), name="talker"), "call")
            host.run_until_idle()
            self.assertEqual(logs.labelled("failed"), ["error.communication h1"])

    def test_an_async_transport_needs_async_with(self):
        async def transport(send):
            pass

        host = tinyactors.Host([], system="a", transport=transport)
        with self.assertRaises(TypeError):
            host.__enter__()
        host.close()

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

        def processor(send):
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
