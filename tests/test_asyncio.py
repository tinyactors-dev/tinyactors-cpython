"""The asyncio driver: await host.idle(), async def processors, one timer."""

import asyncio
import unittest

import tinyactors
from support import APP, Logs, fixture

DOCUMENT = fixture("test_asyncio/echo.python.scxml")
CONFIG = {"systemID": "t", "ioProcessors": [{"type": APP}]}


def image():
    with tinyactors.compile_charts([("echo.scxml", DOCUMENT)]) as compiled:
        return bytes(compiled.image)


class Asyncio(unittest.TestCase):
    def test_an_async_processor_gets_a_record_that_answers_after_the_ack(self):
        logs = Logs()

        async def processor(host, send):
            await asyncio.sleep(0)
            send.answer("ack", {"echo": send.data})

        async def main():
            async with tinyactors.Host(CONFIG, {APP: processor}, log=logs) as host:
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

        async def processor(host, send):
            raise ValueError("no")

        async def main():
            async with tinyactors.Host(CONFIG, {APP: processor}, log=logs) as host:
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

    def test_the_timer_fires_a_delayed_send(self):
        logs = Logs()

        async def main():
            async with tinyactors.Host(CONFIG, log=logs) as host:
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
            async with tinyactors.Host(CONFIG, {APP: lambda host, send: send.answer("ack")}) as host:
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
            async with tinyactors.Host(CONFIG) as host:
                host.host_stats(lambda host, result: got.append(result["host"]["segments"]))
                await host.idle()

        asyncio.run(main())
        self.assertEqual(got, [0])


class Synchronous(unittest.TestCase):
    def test_run_sleeps_to_the_deadline(self):
        logs = Logs()
        with tinyactors.Host(CONFIG, log=logs) as host:
            host.add_image(image())
            host.send(host.spawn(host.chart("echo")), "later")
            host.run()
        self.assertEqual(logs.labelled("tick"), ["yes"])

    def test_a_simulated_clock_moves_only_with_increment(self):
        logs = Logs()
        clock = tinyactors.SimulatedClock()
        with tinyactors.Host(CONFIG, log=logs, clock=clock) as host:
            host.add_image(image())
            host.send(host.spawn(host.chart("echo")), "later")
            host.run()
            self.assertEqual(logs.labelled("tick"), [])
            clock.increment(0.019)
            self.assertEqual(logs.labelled("tick"), [])
            clock.increment(0.001)
            self.assertEqual(logs.labelled("tick"), ["yes"])
            self.assertAlmostEqual(clock.now, 0.02)


if __name__ == "__main__":
    unittest.main()
