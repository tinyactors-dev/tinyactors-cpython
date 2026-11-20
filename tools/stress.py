"""Hibernation under load: many actors, few resident, the rest in SQLite.

    python tools/stress.py [--actors N] [--resident R] [--events E] [--datamodel python|ecmascript]

The application's side of hibernation (README, "Hibernation"), as a
program would write it: a store and a bound, Host(store=..., resident=R),
and the host does the rest.

1. populate: spawn N actors of a counter chart; after each turn the host
   hibernates the surplus past R into the store (one SQLite row each);
2. traffic: E events to actors drawn at random; an event for a
   hibernated actor makes a WAKE, for which the host loads the blob and
   wakes it, and after the turn the CLOCK sweep's cold actors go back to
   the store until R are resident;
3. verify: a sample of actors (all of them when N is small) reports its
   count, which must be the events it was sent; after every turn at most
   R actors are resident (host_stats).

Exits with status 1 when a check fails. Prints the times, the rates, the
store's size, the process's peak memory and the host's region per actor.
"""

import argparse
import array
import os
import pathlib
import random
import resource
import sqlite3
import sys
import tempfile
import time

import tinyactors

# The charts are files in tools/charts/, <name>.<datamodel>.scxml.
CHARTS = pathlib.Path(__file__).resolve().parent / "charts"
DATAMODELS = ("ecmascript", "python")


def read_chart(name, datamodel):
    """The text of the chart file tools/charts/<name>.<datamodel>.scxml."""
    return (CHARTS / ("%s.%s.scxml" % (name, datamodel))).read_text(encoding="utf-8")


BATCH = 256


class Store:
    """The hibernated actors' blobs, one SQLite row per session: the
    host's store (save, load, delete), its writes batched in transactions
    of 4096 (what tinyactors.SQLiteStore does a row at a time)."""

    def __init__(self, path):
        self.db = sqlite3.connect(path, isolation_level=None)
        self.db.execute("PRAGMA journal_mode=WAL")
        self.db.execute("PRAGMA synchronous=OFF")
        self.db.execute("CREATE TABLE blobs (session INTEGER PRIMARY KEY, blob BLOB NOT NULL)")
        self.pending = {}  # session -> blob, or None for a delete
        self.puts = self.gets = 0

    def save(self, session, blob):
        self.pending[session] = blob
        self.puts += 1
        if len(self.pending) >= 4096:
            self.flush()

    def delete(self, session):
        self.pending[session] = None
        if len(self.pending) >= 4096:
            self.flush()

    def flush(self):
        if self.pending:
            self.db.execute("BEGIN")
            self.db.executemany("INSERT OR REPLACE INTO blobs VALUES (?, ?)",
                                [(session, blob) for session, blob in self.pending.items() if blob is not None])
            self.db.executemany("DELETE FROM blobs WHERE session = ?",
                                [(session,) for session, blob in self.pending.items() if blob is None])
            self.db.execute("COMMIT")
            self.pending.clear()

    def load(self, session):
        self.gets += 1
        if session in self.pending:  # read your own writes
            return self.pending[session]
        return self.db.execute("SELECT blob FROM blobs WHERE session = ?", (session,)).fetchone()[0]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--actors", type=int, default=2_000_000)
    parser.add_argument("--resident", type=int, default=1000)
    parser.add_argument("--events", type=int, default=200_000)
    parser.add_argument("--sample", type=int, default=10_000)
    parser.add_argument("--datamodel", choices=DATAMODELS, default="python")
    parser.add_argument("--seed", type=int, default=1)
    options = parser.parse_args()
    random.seed(options.seed)
    failures = []

    directory = tempfile.TemporaryDirectory()
    store = Store(os.path.join(directory.name, "actors.db"))
    counts = {}

    def log(entry):
        session, count = entry.text.split()
        counts[int(session)] = int(count)

    peak = [0]

    def check_resident(host, slack):
        got = []
        host.host_stats(lambda result: got.append(result["host"]))
        host.run_until_idle()
        stats = got[0]
        live = stats["actors"] - stats["hibernated"]
        peak[0] = max(peak[0], live)
        if live > options.resident + slack:
            failures.append("%d actors resident, more than %d + %d" % (live, options.resident, slack))
        return stats

    # Capacity planning: an actor slot for every actor (a hibernated one is
    # its record and session entry, about 60 B), but instance and string
    # memory for the resident ones only: the counter's instance fits in 128 B,
    # its strings in 16. Tracing off: nobody exports the traces here, and on it
    # takes 512 B of journal and a context per maxEvents. Left to grow, a
    # segment doubles every pool together, instance memory derived from
    # maxActors: size the pools, do not let it grow.
    resident_room = options.resident + 2 * BATCH
    config = {"system": {"maxActors": options.actors + 4 * BATCH, "maxEvents": 4096,
                         "instanceBytes": 128 * resident_room,
                         "stringBytes": 16384 + 16 * resident_room, "tracing": False}}
    with tinyactors.compile_charts([("counter.scxml", read_chart("counter", options.datamodel))]) as compiled:
        host = tinyactors.Host(compiled, system="stress", config=config, log=log, store=store,
                               resident=options.resident)

    # 1. populate, R resident at most after each turn
    started = time.perf_counter()
    sessions = array.array("Q")
    for i in range(options.actors):
        sessions.append(host.spawn("counter").id)
        if i % BATCH == BATCH - 1:
            host.run_until_idle()
            check_resident(host, 0)
        if i and i % 500_000 == 0:
            print("  populate: %d actors, %.1f s" % (i, time.perf_counter() - started), flush=True)
    host.run_until_idle()
    store.flush()
    populated = time.perf_counter() - started
    stats = check_resident(host, 0)
    print("populate: %d actors in %.1f s (%.0f spawned and hibernated a second), %d resident, %d hibernated" % (
        options.actors, populated, options.actors / populated, stats["actors"] - stats["hibernated"],
        stats["hibernated"]), flush=True)

    # 2. traffic: random actors, woken from the store, swept back
    started = time.perf_counter()
    sent = array.array("I", bytes(4 * options.actors))
    gets = store.gets
    for first in range(0, options.events, BATCH):
        items = []
        for _ in range(min(BATCH, options.events - first)):
            index = random.randrange(options.actors)
            sent[index] += 1
            items.append((sessions[index], "inc", {"k": index}))
        if host.send_many(items) != len(items):
            failures.append("send_many refused events")
        host.run_until_idle()  # WAKE: loaded from the store, woken, the events run, the surplus swept back
        check_resident(host, 0)
    store.flush()
    traffic = time.perf_counter() - started
    check_resident(host, 0)
    print("traffic: %d events in %.1f s (%.0f a second): %d woken from the store" % (
        options.events, traffic, options.events / traffic, store.gets - gets), flush=True)

    # 3. verify a sample
    started = time.perf_counter()
    sample = range(options.actors) if options.actors <= options.sample else random.sample(range(options.actors), options.sample)
    for chunk in range(0, len(sample), BATCH):
        indices = sample[chunk:chunk + BATCH]
        host.send_many([(sessions[index], "report") for index in indices])
        host.run_until_idle()
        check_resident(host, 0)
    wrong = [index for index in sample if counts.get(sessions[index]) != sent[index]]
    if wrong:
        failures.append("%d of %d actors counted wrong, e.g. actor %d: %r, sent %d" % (
            len(wrong), len(sample), wrong[0], counts.get(sessions[wrong[0]]), sent[wrong[0]]))
    stats = check_resident(host, 0)
    region = []
    host.host_stats(lambda result: region.append(result["host"]["regionBytes"]))
    host.run_until_idle()
    verified = time.perf_counter() - started
    host.close()
    store.flush()
    size = os.path.getsize(os.path.join(directory.name, "actors.db"))
    peak_rss = resource.getrusage(resource.RUSAGE_SELF).ru_maxrss * (1 if sys.platform == "darwin" else 1024)
    print("verify: %d actors' counts in %.1f s, %d wrong" % (len(sample), verified, len(wrong)))
    print("store: %d puts, %d gets, %.1f MB (%.0f B an actor)" % (store.puts, store.gets, size / 1e6, size / options.actors))
    print("memory: peak resident %.0f MB (%.0f B an actor); region %.1f MB (%.0f B an actor); at most %d actors resident after a turn" % (
        peak_rss / 1e6, peak_rss / options.actors, region[0] / 1e6, region[0] / options.actors, peak[0]))
    for failure in failures:
        print("FAIL " + failure)
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
