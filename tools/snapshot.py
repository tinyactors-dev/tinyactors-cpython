"""Snapshots under load: a deploy rehearsed on a copy of a live host, most
of whose actors sleep in SQLite and some of whose payments are in flight.

    python tools/snapshot.py [--actors N] [--resident R] [--events E] [--datamodel python|ecmascript]

The application's side of snapshots (README, "Snapshots"), as a program
would write it to try a new chart version on a copy of production before
deploying it:

1. live: N order actors of version 1, at most R resident, the rest
   hibernated into SQLite (tools/stress.py's store); E events to actors
   drawn at random; every resident actor gets a delayed send due in 10 s,
   and half of them start paying: each invokes the payment service
   (<invoke type="urn:example:payment">, its amount as a param), which the
   live host runs and which has not answered yet;
2. snapshot: Host.snapshot() of the live host, written to a file, and a
   copy of its store (a copy writes blobs too: it must not write the live
   one). The live host is not stopped;
3. restore: Host.restore makes the copy from the file, with its own store,
   clock and handlers, and a stand-in payment service: a copy must never
   reach the real one. A snapshot of the copy before it runs must be the
   same bytes. At its first run the copy announces each payment that was
   in flight; the stand-in gets each (invocation.restored, the same amount)
   and declines it;
4. rehearse: the copy adds version 2 and migrates every actor to it
   (Host.migrate, waking the hibernated ones a batch at a time from the
   copy's store, as tools/migration.py does), with E more events, then
   runs its clock past the delayed sends;
5. verify the copy: the migration migrated every actor, none failed; a sample
   (all up to 25,000) reports its count (every event, before and after
   the snapshot), its delayed send, its payment (declined for the payers)
   and version 2; each payment in flight reached the stand-in once, with
   its amount, and was cancelled after its decline (stop);
6. verify the live host: it goes on with E events of its own; its sample
   reports version 1, its own counts, payments still in flight; the real
   service was started once per payer and never stopped, and every blob
   in the live store is the last one the live host wrote for its session
   (the copy wrote into its own). After every eviction, on both hosts, at
   most R actors are resident (plus two batches while migrating).

With R = N every actor stays resident, so the datamodels' own state is most
of the snapshot. Exits with status 1 when a check fails. Prints the times,
the sizes, the process's peak memory and the hosts' regions per actor.
"""

import argparse
import hashlib
import array
import collections
import os
import random
import resource
import sys
import tempfile
import time
import zlib

import tinyactors
from stress import BATCH, DATAMODELS, Store, read_chart

PAYMENT = "urn:example:payment"


def index(session):
    return (session & 0xFFFFFFFF) - 1  # segment 0 numbers its sessions 1, 2, ...


class Population:
    """One host's actors: its store, the resident ones (least recently used
    first), the migration's wake queue, the reports; as tools/migration.py."""

    def __init__(self, name, store, options, failures):
        self.name, self.store, self.options, self.failures = name, store, options, failures
        self.resident = collections.OrderedDict()
        self.moved = bytearray(options.actors)   # 1: migrated (the MIGRATED effect)
        self.queued = bytearray(options.actors)  # 1: the migration asked for it, waiting in `asked`
        self.asked = collections.deque()
        self.reports, self.wakes, self.moves, self.peak = {}, 0, 0, 0
        self.migrating, self.migration_done = False, None
        self.written = array.array("Q", bytes(8 * options.actors))  # crc32 + 1 of the last blob it put

    def wake_now(self, host, session):
        host.wake(session, self.store.get(session))
        self.resident[session] = None
        self.wakes += 1

    def wake(self, host, session, migration):
        if migration:  # the migration asks: in its order, a batch at a time
            if not self.queued[index(session)]:
                self.queued[index(session)] = 1
                self.asked.append(session)
            return
        self.wake_now(host, session)  # an event's or a timer's: at once

    def migrated(self, host, entry):
        if self.moved[index(entry.session)]:
            self.failures.append("%s: actor %d moved twice" % (self.name, entry.session))
        self.moved[index(entry.session)] = 1
        self.moves += 1

    def log(self, host, entry):
        session, n, late, paid, version = entry.text.split()
        self.reports[int(session)] = (int(n), int(late), int(paid), int(version))

    def hibernate(self, host, session):
        try:
            blob = host.hibernate(session)
            self.written[index(session)] = zlib.crc32(blob) + 1
            self.store.put(session, blob)
        except tinyactors.TinyactorsError as error:
            if error.status != "TA_ERROR_STATE":
                raise
            return False  # not at rest, or paying (an invocation keeps it in memory): it stays
        self.resident.pop(session, None)
        return True

    def evict(self, host):
        """Down to R resident: cold actors first, then the least recently
        used; while migrating, only the actors that moved already."""
        def may(session):
            return not self.migrating or self.moved[index(session)]

        while len(self.resident) > self.options.resident:
            cold = [s for s in host.sweep(1 << 20, len(self.resident) - self.options.resident) if may(s)]
            if not sum(self.hibernate(host, session) for session in cold):
                break
        for session in list(self.resident):  # payers stay (an invocation keeps an actor in memory)
            if len(self.resident) <= self.options.resident:
                break
            if may(session):
                self.hibernate(host, session)

    def check_resident(self, host, slack=0):
        got = []
        host.host_stats(lambda host, result: got.append(result["host"]))
        host.run_until_idle()
        stats = got[0]
        live = stats["actors"] - stats["hibernated"]
        self.peak = max(self.peak, live)
        if live > self.options.resident + slack:
            self.failures.append("%s: %d actors resident, more than %d + %d" % (
                self.name, live, self.options.resident, slack))
        return stats


class Payments:
    """A payment service: the live host's real one, which has not answered
    yet, or the copy's stand-in, which declines what it is given. Each
    invocation gets a Payment, the service object the host keeps."""

    def __init__(self, decline):
        self.decline, self.started, self.stopped, self.amounts, self.restored = decline, {}, set(), {}, 0

    def start(self, host, invocation):
        self.started[invocation.session] = self.started.get(invocation.session, 0) + 1
        self.amounts[invocation.session] = invocation.data["amount"]
        self.restored += invocation.restored
        if self.decline:
            invocation.send_back("declined")
        return Payment(self, invocation.session)


class Payment:
    """One payment in flight; stop() is its cancel (the payer left paying)."""

    __slots__ = ("payments", "session")

    def __init__(self, payments, session):
        self.payments, self.session = payments, session

    def stop(self, host):
        if self.session in self.payments.stopped:
            self.payments.stopped.add(-self.session)  # twice: the check below sees it
        self.payments.stopped.add(self.session)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--actors", type=int, default=2_000_000)
    parser.add_argument("--resident", type=int, default=1000)
    parser.add_argument("--events", type=int, default=200_000)
    parser.add_argument("--sample", type=int, default=25_000)
    parser.add_argument("--datamodel", choices=DATAMODELS, default="python")
    parser.add_argument("--seed", type=int, default=1)
    options = parser.parse_args()
    random.seed(options.seed)
    failures = []
    directory = tempfile.TemporaryDirectory()
    v1, v2 = (read_chart(version, options.datamodel) for version in ("order-v1", "order-v2"))

    resident_room = min(options.resident, options.actors) + 4 * BATCH
    # sized as tools/stress.py's (instance 128 B and strings 16 B per resident actor, tracing off);
    # events: a delayed send per resident actor, and after the restore the stand-in's declines
    # (quickjs keeps each actor's last event too)
    config = {"systemID": "orders", "system": {"ticksPerSecond": 1000,
                                               "maxActors": options.actors + 4 * BATCH,
                                               "maxEvents": 4096 + 3 * resident_room, "maxTimers": resident_room,
                                               "maxRequests": resident_room,
                                               "instanceBytes": 128 * resident_room,
                                               "stringBytes": 16384 + 16 * resident_room, "tracing": False}}
    live_store = Store(os.path.join(directory.name, "live.db"))
    live = Population("live", live_store, options, failures)
    real = Payments(decline=False)
    live_clock = tinyactors.SimulatedClock()
    host = tinyactors.Host(config, wake=live.wake, log=live.log, clock=live_clock, services={PAYMENT: real.start})
    with tinyactors.compile_charts([("order.scxml", v1)]) as compiled:
        host.add_image(compiled.image)
    chart = host.chart("order")
    sessions = array.array("Q")
    sent = array.array("I", bytes(4 * options.actors))       # events before the snapshot
    sent_live = array.array("I", bytes(4 * options.actors))  # the live host's after it
    sent_copy = array.array("I", bytes(4 * options.actors))  # the copy's after it

    def traffic(population, host, counts, total):
        for first in range(0, total, BATCH):
            items = []
            for _ in range(min(BATCH, total - first)):
                i = random.randrange(options.actors)
                counts[i] += 1
                items.append((sessions[i], "inc", {"k": i}))
                if sessions[i] in population.resident:
                    population.resident.move_to_end(sessions[i])
            if host.send_many(items) != len(items):
                failures.append("%s: send_many refused events" % population.name)
            host.run_until_idle()
            if not population.migrating:
                population.evict(host)
                population.check_resident(host)

    # 1. live: populate, events, delayed sends, payments in flight
    started = time.perf_counter()
    for i in range(options.actors):
        session = host.spawn(chart)
        sessions.append(session)
        live.resident[session] = None
        if len(live.resident) >= options.resident + BATCH:
            host.run_until_idle()
            live.evict(host)
    host.run_until_idle()
    live.evict(host)
    if sessions and sessions[-1] != sessions[0] + options.actors - 1:
        failures.append("the sessions are not numbered 1..N in segment 0: index() is wrong")
    traffic(live, host, sent, options.events)
    waiting = list(live.resident)
    payers = waiting[::2]
    for first in range(0, len(waiting), BATCH):
        host.send_many([(s, "wait") for s in waiting[first:first + BATCH]])
        host.send_many([(s, "pay") for s in waiting[first:first + BATCH:2]])
        host.run_until_idle()
    live_store.flush()
    stats = live.check_resident(host)
    if len(real.started) != len(payers):
        failures.append("live: %d payments started for %d payers" % (len(real.started), len(payers)))
    amounts = {s: sent[index(s)] for s in payers}
    print("live: %d actors and %d events in %.1f s; %d resident with a delayed send, %d of them paying; %d hibernated" % (
        options.actors, options.events, time.perf_counter() - started, len(waiting), len(payers),
        stats["hibernated"]), flush=True)

    # 2. snapshot, to a file; a copy of the store; the live host goes on
    started = time.perf_counter()
    blob = host.snapshot()
    taken = time.perf_counter() - started
    path = os.path.join(directory.name, "orders.snapshot")
    with open(path, "wb") as file:
        file.write(blob)
    in_memory = list(live.resident)  # resident at the snapshot: resident in the copy too
    copy_store = Store(os.path.join(directory.name, "copy.db"))
    live_store.db.backup(copy_store.db)
    print("snapshot: %.1f MB in %.2f s (%.0f B an actor); the store copied" % (
        len(blob) / 1e6, taken, len(blob) / options.actors), flush=True)
    digest = hashlib.sha256(blob).digest()  # the copy's snapshot is compared with it: one snapshot in memory at a time
    del blob

    # 3. restore the copy, with stand-ins; its snapshot before it runs is the same bytes
    with open(path, "rb") as file:
        blob = file.read()
    copy = Population("copy", copy_store, options, failures)
    stand_in = Payments(decline=True)
    copy_clock = tinyactors.SimulatedClock()  # a clock of its own: the copy's time is not the live host's
    started = time.perf_counter()
    rehearsal = tinyactors.Host.restore(blob, config, wake=copy.wake, log=copy.log, migrated=copy.migrated,
                                        clock=copy_clock, services={PAYMENT: stand_in.start},
                                        migration_done=lambda host, result: setattr(copy, "migration_done", result))
    restored = time.perf_counter() - started
    del blob
    again = rehearsal.snapshot()
    if hashlib.sha256(again).digest() != digest:
        failures.append("the copy's snapshot differs from the one it was restored from")
    del again
    copy.resident.update((s, None) for s in in_memory)
    rehearsal.run_until_idle()  # the payments in flight, announced again: the stand-in declines them
    if stand_in.started != {s: 1 for s in payers} or stand_in.restored != len(payers):
        failures.append("copy: the stand-in started %d payments (%d restored) for %d payers" % (
            len(stand_in.started), stand_in.restored, len(payers)))
    if stand_in.amounts != amounts:
        failures.append("copy: a restored payment lost its amount")
    print("restore: in %.2f s; %d payments in flight started again on the stand-in" % (
        restored, stand_in.restored), flush=True)

    # 4. rehearse the deploy: version 2, migrate everyone with events meanwhile, the clock past the sends
    started = time.perf_counter()
    with tinyactors.compile_charts([("order.scxml", v2)]) as compiled:
        rehearsal.add_image(compiled.image)
    copy.migrating = True
    migration = rehearsal.migrate(wake=True)
    copy_events, stalled, before = 0, 0, -1
    while copy.migration_done is None:
        stalled = stalled + 1 if copy.moves == before and not copy.asked else 0
        before = copy.moves
        if stalled > 100:
            failures.append("copy: the migration stalled with %d actors moved" % before)
            break
        woken = 0
        while copy.asked and woken < BATCH:
            session = copy.asked.popleft()
            copy.queued[index(session)] = 0
            if session not in copy.resident and not copy.moved[index(session)]:
                copy.wake_now(rehearsal, session)
                woken += 1
        if copy_events < options.events:
            n = min(BATCH, options.events - copy_events)
            traffic(copy, rehearsal, sent_copy, n)
            copy_events += n
        rehearsal.run_until_idle()
        copy.evict(rehearsal)
        copy.check_resident(rehearsal, 2 * BATCH)
    copy.migrating = False
    traffic(copy, rehearsal, sent_copy, options.events - copy_events)
    result = copy.migration_done or {"migration": None, "migrated": 0, "failed": 0, "pending": None}
    if result["migration"] != migration or result["migrated"] != options.actors or result["failed"] or result["pending"]:
        failures.append("copy: the migration ended %r, not %d migrated" % (result, options.actors))
    rehearsed = time.perf_counter() - started
    print("rehearse: %d actors to version 2 in %.1f s, %d events meanwhile, %d woken from the copy's store" % (
        copy.moves, rehearsed, options.events, copy.wakes), flush=True)
    return finish(options, failures, directory, sessions, (sent, sent_live, sent_copy), (live, copy),
                  (host, rehearsal), (real, stand_in), (payers, waiting, live_clock, copy_clock), traffic)


def finish(options, failures, directory, sessions, sent_all, populations, hosts, services, groups, traffic):
    sent, sent_live, sent_copy = sent_all
    live, copy = populations
    host, rehearsal = hosts
    real, stand_in = services
    payers, waiting, live_clock, copy_clock = groups
    paying, late = set(payers), set(waiting)

    def report(population, h, sample):
        population.reports.clear()
        for chunk in range(0, len(sample), BATCH):
            h.send_many([(sessions[i], "report") for i in sample[chunk:chunk + BATCH]])
            h.run_until_idle()
            population.evict(h)
            population.check_resident(h)

    sample = range(options.actors) if options.actors <= options.sample else random.sample(range(options.actors), options.sample)
    sample = sorted(set(sample) | {index(s) for s in payers[:50]} | {index(s) for s in waiting[1::2][:50]})

    # 5. the copy: the clock past the delayed sends, then every count, send, payment and version
    copy_clock.increment(10_000)
    copy.evict(rehearsal)
    copy_region = copy.check_resident(rehearsal)["regionBytes"]
    report(copy, rehearsal, sample)
    expected = {i: (sent[i] + sent_copy[i], 1 if sessions[i] in late else 0, 0 if sessions[i] in paying else -1, 2)
                for i in sample}
    wrong = [i for i in sample if copy.reports.get(sessions[i]) != expected[i]]
    if wrong:
        failures.append("copy: %d of %d actors wrong, e.g. actor %d: %r, expected %r" % (
            len(wrong), len(sample), wrong[0], copy.reports.get(sessions[wrong[0]]), expected[wrong[0]]))
    rehearsal.close()

    # 6. the live host goes on: its own events, the clock past its delayed sends; payments still in flight
    traffic(live, host, sent_live, options.events)
    live_clock.increment(10_000)
    live.evict(host)
    report(live, host, sample)
    expected = {i: (sent[i] + sent_live[i], 1 if sessions[i] in late else 0, -1, 1) for i in sample}
    wrong_live = [i for i in sample if live.reports.get(sessions[i]) != expected[i]]
    if wrong_live:
        failures.append("live: %d of %d actors wrong, e.g. actor %d: %r, expected %r" % (
            len(wrong_live), len(sample), wrong_live[0], live.reports.get(sessions[wrong_live[0]]), expected[wrong_live[0]]))
    if real.started != {s: 1 for s in payers} or real.restored or real.stopped:
        failures.append("live: the real payment service was started %d times for %d payers (%d restored), "
                        "stopped %d times" % (sum(real.started.values()), len(payers), real.restored, len(real.stopped)))
    if stand_in.stopped != set(payers):
        failures.append("copy: %d stand-in payments cancelled after their decline, %d payers" % (
            len(stand_in.stopped), len(payers)))
    live_region = live.check_resident(host)["regionBytes"]
    host.close()
    live.store.flush()
    foreign = sum(1 for session, blob in live.store.db.execute("SELECT session, blob FROM blobs")
                  if live.written[index(session)] != zlib.crc32(blob) + 1)
    if foreign:
        failures.append("live: %d blobs in the live store are not the live host's last" % foreign)
    peak_rss = resource.getrusage(resource.RUSAGE_SELF).ru_maxrss * (1 if sys.platform == "darwin" else 1024)
    print("verify: %d actors on each host, %d wrong on the copy, %d on the live host" % (
        len(sample), len(wrong), len(wrong_live)))
    print("memory: peak resident %.0f MB (%.0f B an actor, both hosts); region %.1f MB live, %.1f MB the copy "
          "(%.0f B an actor); at most %d actors resident after an eviction" % (
              peak_rss / 1e6, peak_rss / options.actors, live_region / 1e6, copy_region / 1e6,
              live_region / options.actors, max(live.peak, copy.peak)))
    for failure in failures[:20]:
        print("FAIL " + failure)
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
