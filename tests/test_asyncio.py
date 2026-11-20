"""The asyncio driver: await host.idle(), async def processors, one timer."""

import asyncio
import unittest

import tinyactors
from support import APP, Logs, fixture, processors_of

DOCUMENT = fixture("test_asyncio/echo.python.scxml")


def make(processors=None, **options):
    """A host of system "t" with an I/O processor of type APP (a function
    given for it is its receive), running no image yet."""
    return tinyactors.Host([], system="t", io_processors=processors_of(processors or {APP: lambda send: send.fail()}),
                           **options)


def image():
    with tinyactors.compile_charts([("echo.scxml", DOCUMENT)]) as compiled:
        return bytes(compiled.image)


class Asyncio(unittest.TestCase):
    def test_an_async_processor_gets_a_record_that_answers_after_the_ack(self):
        logs = Logs()

        async def processor(send):
            await asyncio.sleep(0)
            send.answer("ack", {"echo": send.data})

        async def main():
            async with make({APP: processor}, log=logs) as host:
                host.add_image(image())
                host.send(host.spawn(host.chart("echo")), "go", [1])
                await host.idle()
                for _ in range(5):
                    await asyncio.sleep(0)
                await host.idle()

        asyncio.run(main())
        self.assertEqual(logs.labelled("ack"), ["{'echo': [1]}"])

    def test_an_async_processor_that_raises_fails_its_send(self):
        logs = Logs()

        async def processor(send):
            raise ValueError("no")

        async def main():
            async with make({APP: processor}, log=logs) as host:
                host.add_image(image())
                host.send(host.spawn(host.chart("echo")), "go")
                await host.idle()
                for _ in range(5):
                    await asyncio.sleep(0)
                with self.assertRaises(ValueError):
                    await host.idle()
                await host.idle()

        asyncio.run(main())
        self.assertEqual(logs.labelled("failed"), ["yes"])

    def test_an_answer_or_failure_to_an_ended_sender_is_dropped(self):
        """answer() and fail() give True when delivered and False when the
        sender ended meanwhile (the maintainer, 2026-11-18, as Node); a
        closed host still raises."""
        for reply in (lambda send: send.answer("ack", {"late": 1}), lambda send: send.fail()):
            logs = Logs()
            gate = asyncio.Event()
            got, kept = [], []

            async def processor(send):
                kept.append(send)
                await gate.wait()
                got.append(reply(send))

            async def main():
                async with make({APP: processor}, log=logs) as host:
                    host.add_image(image())
                    session = host.spawn(host.chart("echo"))
                    host.send(session, "go")
                    await host.idle()
                    self.assertEqual(len(kept), 1)
                    host.terminate(session)
                    await host.idle()
                    gate.set()
                    for _ in range(5):
                        await asyncio.sleep(0)
                    await host.idle()
                with self.assertRaises(tinyactors.TinyactorsError):
                    reply(kept[0])

            asyncio.run(main())
            self.assertEqual(got, [False], "not delivered: the sender ended")
            self.assertEqual((logs.labelled("ack"), logs.labelled("failed")), ([], []), "nobody heard it")

    def test_an_answer_delivered_returns_true(self):
        got = []

        def processor(send):
            got.append(send.answer("ack"))

        async def main():
            async with make({APP: processor}) as host:
                host.add_image(image())
                host.send(host.spawn(host.chart("echo")), "go")
                await host.idle()

        asyncio.run(main())
        self.assertEqual(got, [True])

    def test_the_timer_fires_a_delayed_send(self):
        logs = Logs()

        async def main():
            async with make(log=logs) as host:
                host.add_image(image())
                host.send(host.spawn(host.chart("echo")), "later")
                await host.idle()
                self.assertEqual(logs.labelled("tick"), [])
                await asyncio.sleep(0.1)
                await host.idle()

        asyncio.run(main())
        self.assertEqual(logs.labelled("tick"), ["yes"])

    def test_a_busy_host_lets_other_tasks_run(self):
        """A turn is bounded: a ping-pong between a handler and its actor
        (each answer makes the next send) yields to the loop."""
        pong = fixture("test_asyncio/pong.python.scxml")
        ticks = []

        async def ticker():
            while True:
                ticks.append(1)
                await asyncio.sleep(0)

        async def main():
            async with make({APP: lambda send: send.answer("ack")}) as host:
                with tinyactors.compile_charts([("pong.scxml", pong)]) as compiled:
                    host.add_image(compiled.image)
                task = asyncio.ensure_future(ticker())
                await asyncio.sleep(0)
                before = len(ticks)
                host.send(host.spawn(host.chart("pong")), "go")
                await host.idle()
                task.cancel()
                return len(ticks) - before

        self.assertGreater(asyncio.run(main()), 10)

    def test_typed_methods_under_asyncio(self):
        got = []

        async def main():
            async with make() as host:
                host.host_stats(lambda result: got.append(result["host"]["segments"]))
                await host.idle()

        asyncio.run(main())
        self.assertEqual(got, [0])

    def test_a_typed_method_given_no_handler_is_awaited(self):
        """It resolves with the result, raises the reply's error as its class, and keeps the shape with a handler."""
        async def main():
            async with make() as host:
                host.add_image(image())
                session = host.spawn(host.chart("echo"))
                stats = await host.host_stats()
                state = await host.actor_state(session=session.id)
                with self.assertRaises(tinyactors.StaleError) as raised:
                    await host.actor_state(session=session.id + 1000)
                with self.assertRaises(tinyactors.InvalidError):
                    await host.actor_state(session="not a session")
                with self.assertRaises(TypeError):
                    host.actor_state(on_error=lambda error: None, session=session.id)
                return stats["host"]["segments"], state, raised.exception.code

        segments, state, code = asyncio.run(main())
        self.assertEqual(segments, 1)
        self.assertEqual(state["value"], "s")
        self.assertEqual(code, -32001)

    def test_a_method_pending_at_close_never_settles(self):
        async def main():
            async with make() as host:
                future = host.host_stats()
            await asyncio.sleep(0)
            return future.done()

        self.assertFalse(asyncio.run(main()))


class Synchronous(unittest.TestCase):
    def test_a_typed_method_given_no_handler_needs_asyncio(self):
        with make() as host:
            with self.assertRaises(TypeError) as raised:
                host.host_stats()
        self.assertIn("asyncio", str(raised.exception))

    def test_a_simulated_clock_moves_only_with_increment(self):
        logs = Logs()
        clock = tinyactors.SimulatedClock()
        with make(log=logs, clock=clock) as host:
            host.add_image(image())
            host.send(host.spawn(host.chart("echo")), "later")
            host.run_until_idle()
            self.assertEqual(logs.labelled("tick"), [])
            clock.increment(0.019)
            self.assertEqual(logs.labelled("tick"), [])
            clock.increment(0.001)
            self.assertEqual(logs.labelled("tick"), ["yes"])
            self.assertAlmostEqual(clock.now, 0.02)


if __name__ == "__main__":
    unittest.main()
