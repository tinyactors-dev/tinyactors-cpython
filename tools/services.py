"""Invoked services under load: every actor invokes a service the
application runs, talks to it, and ends it one of three ways.

    python tools/services.py [--actors N] [--events E] [--datamodel python|ecmascript]

The application's side of invoked services (README, "Invoked services"),
as a program would write it:

1. start: N actors of a client chart, each invoking a counter service
   (<invoke type="urn:example:counter">) with params; the application
   registered a handler for the type, which makes one Python object per
   invocation;
2. traffic: E times, a client drawn at random is told to ping its service
   (<send target="#_counter">); the service counts and sends back a pong
   with its count (invocation.send_back); the client counts the pongs;
3. end: a third of the clients ask their service to finish, which ends
   the invocation with done data (invocation.done), so the client gets
   done.invoke.counter; a third leave the invoking state, so core cancels
   the invocation and the service is stopped (service.stop); the rest
   keep theirs;
4. verify: every client reports its pongs, the done data it got, and its
   state; every ping was answered once, every finished client's total is
   its pongs, every cancelled service was stopped once and no other, and
   the host holds exactly the services still running (the others' objects
   are collected).

Exits with status 1 when a check fails. Prints the times, the round trips
a second, the process's peak memory and the host's region per actor.
"""

import argparse
import array
import pathlib
import random
import resource
import sys
import time
import gc
import weakref

import tinyactors

BATCH = 256
COUNTER = "urn:example:counter"
# The charts are files in tools/charts/, <name>.<datamodel>.scxml.
CHARTS = pathlib.Path(__file__).resolve().parent / "charts"
DATAMODELS = ("ecmascript", "python")


def read_chart(name, datamodel):
    """The text of the chart file tools/charts/<name>.<datamodel>.scxml."""
    return (CHARTS / ("%s.%s.scxml" % (name, datamodel))).read_text(encoding="utf-8")


class Counter:
    """A service: one object per invocation, counting the pings it gets."""

    __slots__ = ("invocation", "count", "world", "__weakref__")

    def __init__(self, invocation, world):
        self.invocation, self.count, self.world = invocation, 0, world

    def receive(self, host, send):
        if send.event == "ping":
            self.count += 1
            if send.data["n"] > self.count - 1:  # never more pongs counted than sent (some may be on their way)
                self.world.failures.append("session %d pinged with n=%r after %d pongs" % (
                    self.invocation.session, send.data["n"], self.count - 1))
            self.invocation.send_back("pong", {"n": self.count})
        elif send.event == "finish":
            self.invocation.done({"total": self.count})
            self.world.done.add(self.invocation.session)

    def stop(self, host):
        session = self.invocation.session
        if session in self.world.stopped:
            self.world.failures.append("session %d stopped twice" % session)
        self.world.stopped.add(session)


class World:
    def __init__(self):
        self.failures, self.started, self.done, self.stopped, self.reports = [], {}, set(), set(), {}
        self.alive = set()  # weak references to every service: the host holds the running ones only

    def start(self, host, invocation):
        if invocation.session in self.started:
            self.failures.append("session %d invoked twice" % invocation.session)
        if invocation.type != COUNTER or invocation.src != "counter://local" or invocation.invokeid != "counter":
            self.failures.append("an invocation of %r %r %r" % (invocation.type, invocation.src, invocation.invokeid))
        self.started[invocation.session] = invocation.data["seed"]
        service = Counter(invocation, self)
        self.alive.add(weakref.ref(service))
        return service

    def log(self, host, entry):
        session, pongs, total, state = entry.text.split()
        self.reports[int(session)] = (int(pongs), int(total), state)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--actors", type=int, default=200_000)
    parser.add_argument("--events", type=int, default=1_000_000)
    parser.add_argument("--datamodel", choices=DATAMODELS, default="python")
    parser.add_argument("--seed", type=int, default=1)
    options = parser.parse_args()
    random.seed(options.seed)
    world = World()
    room = options.actors + 4 * BATCH
    # Capacity planning: an invocation is a request of core's (maxRequests); quickjs keeps each
    # actor's last event in core's pool (its datamodel does not detach _event), so on quickjs
    # maxEvents counts the actors too; the "python" datamodel lets its events go. The counter's
    # instance fits in 128 B, its strings in 16; tracing off (nobody exports the traces here).
    events = 16384 + (room if options.datamodel == "ecmascript" else 0)
    config = {"systemID": "services", "system": {"maxActors": room, "maxRequests": room, "maxEvents": events,
                                                 "instanceBytes": 128 * room, "stringBytes": 16384 + 16 * room,
                                                 "tracing": False}}
    host = tinyactors.Host(config, log=world.log, services={COUNTER: world.start})
    with tinyactors.compile_charts([("client.scxml", read_chart("client", options.datamodel))]) as compiled:
        host.add_image(compiled.image)
    chart = host.chart("client")

    # 1. start: every actor invokes a counter
    started = time.perf_counter()
    sessions = array.array("Q")
    for i in range(options.actors):
        sessions.append(host.spawn(chart, params={"seed": i}))
        if i % BATCH == BATCH - 1:
            host.run_until_idle()
    host.run_until_idle()
    start_time = time.perf_counter() - started
    if len(world.started) != options.actors:
        world.failures.append("%d invocations started for %d actors" % (len(world.started), options.actors))
    wrong_seed = sum(1 for i, s in enumerate(sessions) if world.started.get(s) != i)
    if wrong_seed:
        world.failures.append("%d invocations got another actor's params" % wrong_seed)
    print("start: %d actors, each with an invoked service, in %.1f s (%.0f a second)" % (
        options.actors, start_time, options.actors / start_time), flush=True)

    # 2. traffic: pings and pongs
    started = time.perf_counter()
    gos = array.array("I", bytes(4 * options.actors))
    for first in range(0, options.events, BATCH):
        items = []
        for _ in range(min(BATCH, options.events - first)):
            i = random.randrange(options.actors)
            gos[i] += 1
            items.append((sessions[i], "go"))
        if host.send_many(items) != len(items):
            world.failures.append("send_many refused events")
        host.run_until_idle()
    traffic = time.perf_counter() - started
    print("traffic: %d round trips (actor -> service -> actor) in %.1f s (%.0f a second)" % (
        options.events, traffic, options.events / traffic), flush=True)

    # 3. end: a third finish (done.invoke), a third leave (cancelled), the rest keep theirs
    started = time.perf_counter()
    finishing = {sessions[i] for i in range(0, options.actors, 3)}
    leaving = {sessions[i] for i in range(1, options.actors, 3)}
    ends = [(s, "end") for s in sorted(finishing)] + [(s, "leave") for s in sorted(leaving)]
    for first in range(0, len(ends), BATCH):
        if host.send_many(ends[first:first + BATCH]) != len(ends[first:first + BATCH]):
            world.failures.append("send_many refused events")
        host.run_until_idle()
    ending = time.perf_counter() - started
    print("end: %d finished with done data, %d cancelled, in %.1f s" % (len(finishing), len(leaving), ending), flush=True)

    # 4. verify
    for first in range(0, options.actors, BATCH):
        host.send_many([(sessions[i], "report") for i in range(first, min(first + BATCH, options.actors))])
        host.run_until_idle()
    wrong = []
    for i, session in enumerate(sessions):
        expected_state = "finished" if session in finishing else "left" if session in leaving else "talking"
        expected = (gos[i], gos[i] if session in finishing else -1, expected_state)
        if world.reports.get(session) != expected:
            wrong.append((i, world.reports.get(session), expected))
    if wrong:
        world.failures.append("%d of %d actors wrong, e.g. actor %d: %r, expected %r" % (
            len(wrong), options.actors, wrong[0][0], wrong[0][1], wrong[0][2]))
    if world.done != finishing:
        world.failures.append("%d services finished, %d asked to" % (len(world.done), len(finishing)))
    if world.stopped != leaving:
        world.failures.append("%d services stopped (%d of them not cancelled), %d cancelled" % (
            len(world.stopped), len(world.stopped - leaving), len(leaving)))
    gc.collect()
    running = sum(1 for ref in world.alive if ref() is not None)
    if running != options.actors - len(finishing) - len(leaving):
        world.failures.append("the host holds %d services, %d still run" % (
            running, options.actors - len(finishing) - len(leaving)))
    got = []
    host.host_stats(lambda host, result: got.append(result["host"]))
    host.run_until_idle()
    region = got[0]["regionBytes"]
    host.close()
    peak_rss = resource.getrusage(resource.RUSAGE_SELF).ru_maxrss * (1 if sys.platform == "darwin" else 1024)
    print("verify: %d actors' pongs, done data and states, %d wrong; %d services still running" % (
        options.actors, len(wrong), running))
    print("memory: peak resident %.0f MB (%.0f B an actor with its service); region %.1f MB (%.0f B an actor)" % (
        peak_rss / 1e6, peak_rss / options.actors, region / 1e6, region / options.actors))
    for failure in world.failures[:20]:
        print("FAIL " + failure)
    sys.exit(1 if world.failures else 0)


if __name__ == "__main__":
    main()
