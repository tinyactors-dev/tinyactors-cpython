"""Live migration under load: every actor moves to a new chart version
while events keep coming, most of them asleep in SQLite.

    python tools/migration.py [--actors N] [--resident R] [--events E] [--datamodel python|ecmascript]

The application's side of migration (bindings.md section 4; README,
"Migration"), as a program would write it:

1. populate: N actors of version 1 of a counter chart, at most R resident,
   the rest hibernated into SQLite (tools/stress.py's store); then E
   events to actors drawn at random;
2. migrate: version 2 of the chart is added, and Host.migrate(wake=True)
   moves every actor of version 1 to it. Version 2 takes the count from
   error.platform.upgrade (version 1's bindings) and knows its version.
   The migration asks for each hibernated actor with a WAKE that names it: the
   program queues those and wakes a batch at a time, so at most R plus
   two batches are resident; the migration takes each woken one in its next
   runs. An event's WAKE (migration None) wakes its actor at once, and the migration
   takes that one too, so its events do not wait for the queue. Meanwhile
   E more events go to random actors, a batch after every wake: the
   migration is live. After each batch the migrated actors at rest are
   hibernated again (one put back before it moved would be asked for
   again: wasted work, not a stall);
3. verify: the migration ended with every actor migrated, none failed, none
   left; a sample of actors (all when N is small) reports its count and
   version, which must be the events it was sent and 2; the old version's
   segment was retired; after every eviction at most R actors were
   resident, plus two batches (the migration's WAKEs woken in a round, and the
   actors events woke, until the migration moves them).

Exits with status 1 when a check fails. Prints the times, the rates, the
store's size, the process's peak memory and the host's region per actor.
"""

import argparse
import array
import collections
import os
import random
import resource
import sys
import tempfile
import time

import tinyactors
from stress import BATCH, DATAMODELS, Store, read_chart

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--actors", type=int, default=1_000_000)
    parser.add_argument("--resident", type=int, default=1000)
    parser.add_argument("--events", type=int, default=100_000)
    parser.add_argument("--sample", type=int, default=25_000)
    parser.add_argument("--datamodel", choices=DATAMODELS, default="python")
    parser.add_argument("--seed", type=int, default=1)
    options = parser.parse_args()
    random.seed(options.seed)
    failures = []

    directory = tempfile.TemporaryDirectory()
    store = Store(os.path.join(directory.name, "actors.db"))
    resident = collections.OrderedDict()  # sessions in memory, least recently used first
    moved = bytearray(options.actors)     # 1: migrated to version 2 (the MIGRATED effect)
    queued = bytearray(options.actors)    # 1: the migration asked for it, waiting in `asked`
    asked = collections.deque()           # the migration's WAKEs, in the order they came
    counts, wakes, moves = {}, [0], [0]
    current = {"result": None, "id": None}  # the migration: its id, then migration_done's result

    def index(session):
        return (session & 0xFFFFFFFF) - 1  # segment 0 numbers its sessions 1, 2, ...

    def wake_now(host, session):
        host.wake(session, store.get(session))
        resident[session] = None
        wakes[0] += 1

    def wake(host, session, migration):
        if migration:  # the migration asks: in its order, a batch at a time
            if not queued[index(session)]:
                queued[index(session)] = 1
                asked.append(session)
            return
        wake_now(host, session)  # an event's or a timer's: at once (a migration waiting for it takes it)

    def migrated(host, entry):
        if moved[index(entry.session)]:
            failures.append("actor %d moved twice" % entry.session)
        moved[index(entry.session)] = 1
        moves[0] += 1

    def done(host, result):
        current["result"] = result

    def log(host, entry):
        session, count, version = entry.text.split()
        counts[int(session)] = (int(count), int(version))

    def hibernate(host, session):
        try:
            store.put(session, host.hibernate(session))
        except tinyactors.TinyactorsError as error:
            if error.status != "TA_ERROR_STATE":
                raise
            return False  # not at rest now (an upgrade or an event waiting): it stays
        resident.pop(session, None)
        return True

    def evict(host, migrating):
        """Down to R resident: cold actors first, then the least recently
        used; while migrating, only the actors that moved already."""
        def may(session):
            return not migrating or moved[index(session)]

        while len(resident) > options.resident:
            cold = [s for s in host.sweep(1 << 20, len(resident) - options.resident) if may(s)]
            if not sum(hibernate(host, session) for session in cold):
                break
        for session in [s for s in resident if may(s)][:max(0, len(resident) - options.resident)]:
            hibernate(host, session)

    peak = [0]

    def check_resident(host, slack):
        got = []
        host.host_stats(lambda host, result: got.append(result["host"]))
        host.run_until_idle()
        stats = got[0]
        live = stats["actors"] - stats["hibernated"]
        peak[0] = max(peak[0], live)
        if live > options.resident + slack:
            failures.append("%d actors resident, more than %d + %d" % (live, options.resident, slack))
        return stats

    resident_room = options.resident + 4 * BATCH
    # sized as tools/stress.py's (instance 128 B and strings 16 B per resident actor, tracing off)
    config = {"systemID": "migration", "system": {"maxActors": options.actors + 4 * BATCH, "maxEvents": 4096,
                                                  "instanceBytes": 128 * resident_room,
                                                  "stringBytes": 16384 + 16 * resident_room, "tracing": False}}
    host = tinyactors.Host(config, wake=wake, log=log, migrated=migrated, migration_done=done)
    v1, v2 = (read_chart(version, options.datamodel) for version in ("counter-v1", "counter-v2"))
    with tinyactors.compile_charts([("counter.scxml", v1)]) as compiled:
        host.add_image(compiled.image)
    chart = host.chart("counter")

    # 1. populate, R resident at most, and events
    started = time.perf_counter()
    sessions = array.array("Q")
    sent = array.array("I", bytes(4 * options.actors))
    for i in range(options.actors):
        session = host.spawn(chart)
        sessions.append(session)
        resident[session] = None
        if len(resident) >= options.resident + BATCH:
            host.run_until_idle()
            evict(host, False)
    host.run_until_idle()
    evict(host, False)
    if sessions and sessions[-1] != sessions[0] + options.actors - 1:
        failures.append("the sessions are not numbered 1..N in segment 0: index() is wrong")

    def traffic(count):
        items = []
        for _ in range(count):
            i = random.randrange(options.actors)
            sent[i] += 1
            items.append((sessions[i], "inc", {"k": i}))
            if sessions[i] in resident:
                resident.move_to_end(sessions[i])
        if host.send_many(items) != len(items):
            failures.append("send_many refused events")

    for first in range(0, options.events, BATCH):
        traffic(min(BATCH, options.events - first))
        host.run_until_idle()
        evict(host, False)
    store.flush()
    stats = check_resident(host, 0)
    populated = time.perf_counter() - started
    print("populate: %d actors of version 1 and %d events in %.1f s, %d resident, %d hibernated" % (
        options.actors, options.events, populated, stats["actors"] - stats["hibernated"], stats["hibernated"]), flush=True)

    # 2. migrate, live
    started = time.perf_counter()
    with tinyactors.compile_charts([("counter.scxml", v2)]) as compiled:
        host.add_image(compiled.image)
    region_both = None
    current["id"] = host.migrate(wake=True)
    live_events, stalled, before = 0, 0, -1
    while current["result"] is None:
        stalled = stalled + 1 if moves[0] == before and not asked else 0
        before = moves[0]
        if stalled > 100:
            failures.append("the migration stalled: nothing moved in 100 rounds, %d actors moved" % before)
            break
        woken = 0
        while asked and woken < BATCH:
            session = asked.popleft()
            queued[index(session)] = 0
            if session not in resident and not moved[index(session)]:  # an event may have woken it
                wake_now(host, session)
                woken += 1
        if live_events < options.events:
            traffic(min(BATCH, options.events - live_events))
            live_events += min(BATCH, options.events - live_events)
        host.run_until_idle()
        evict(host, True)
        # a batch of the migration's WAKEs woken this round, and up to a batch the
        # round's events woke, until the migration moves them (only moved actors go
        # back)
        stats = check_resident(host, 2 * BATCH)
        if region_both is None:
            region_both = stats["regionBytes"]
    while live_events < options.events:  # the rest after the migration, so every run has its E events
        traffic(min(BATCH, options.events - live_events))
        live_events += min(BATCH, options.events - live_events)
        host.run_until_idle()
        evict(host, False)
    evict(host, False)
    store.flush()
    migration = time.perf_counter() - started
    result = current["result"] or {"migration": None, "migrated": 0, "failed": 0, "pending": None}
    if result["migration"] != current["id"] or result["migrated"] != options.actors or result["failed"] or result["pending"]:
        failures.append("the migration ended %r, not %d migrated" % (result, options.actors))
    if moves[0] != options.actors:
        failures.append("%d MIGRATED effects for %d actors" % (moves[0], options.actors))
    stats = check_resident(host, 0)
    print("migrate: %d actors to version 2 in %.1f s (%.0f a second), %d events meanwhile, %d woken from the store" % (
        moves[0], migration, moves[0] / migration, options.events, wakes[0]), flush=True)

    # 3. verify a sample
    started = time.perf_counter()
    sample = range(options.actors) if options.actors <= options.sample else random.sample(range(options.actors), options.sample)
    for chunk in range(0, len(sample), BATCH):
        host.send_many([(sessions[i], "report") for i in sample[chunk:chunk + BATCH]])
        host.run_until_idle()
        evict(host, False)
        check_resident(host, 0)
    wrong = [i for i in sample if counts.get(sessions[i]) != (sent[i], 2)]
    if wrong:
        failures.append("%d of %d actors wrong, e.g. actor %d: %r, sent %d" % (
            len(wrong), len(sample), wrong[0], counts.get(sessions[wrong[0]]), sent[wrong[0]]))
    stats = check_resident(host, 0)
    if stats["segments"] != 1:
        failures.append("%d segments live after the migration: version 1's was not retired" % stats["segments"])
    verified = time.perf_counter() - started
    host.close()
    store.flush()
    size = os.path.getsize(os.path.join(directory.name, "actors.db"))
    peak_rss = resource.getrusage(resource.RUSAGE_SELF).ru_maxrss * (1 if sys.platform == "darwin" else 1024)
    print("verify: %d actors' counts and versions in %.1f s, %d wrong" % (len(sample), verified, len(wrong)))
    print("store: %d puts, %d gets, %.1f MB" % (store.puts, store.gets, size / 1e6))
    print("memory: peak resident %.0f MB (%.0f B an actor); region %.1f MB with both versions, %.1f MB after "
          "(%.0f B an actor); at most %d actors resident after an eviction" % (
              peak_rss / 1e6, peak_rss / options.actors, (region_both or 0) / 1e6, stats["regionBytes"] / 1e6,
              stats["regionBytes"] / options.actors, peak[0]))
    for failure in failures:
        print("FAIL " + failure)
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
