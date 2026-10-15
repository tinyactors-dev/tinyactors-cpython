"""Bytes per actor at a population of actors of one chart, resident and
idle, on the "python" datamodel and on quickjs (ROADMAP.md, plan step 9).

    python tools/measure.py [--actors N] [--json PATH]

Each datamodel is measured in a process of its own, so one cannot inherit
the other's heap. A measurement creates a host, adds the image, takes the
baselines, spawns N actors (each runs its initial macrostep), runs the host
until idle, and reports per actor:

- resident: the process's resident set (Linux: /proc/self/statm; elsewhere
  the peak, getrusage's ru_maxrss), after minus before;
- traced: Python's allocator, tracemalloc's traced memory, after minus
  before. tracemalloc traces every domain, so it holds the Python objects
  and the host's own blocks (PyMem_RawMalloc): the region, the tables, the
  arena; it does not hold quickjs's heap (its own malloc);
- after one event to each actor, which then keeps its _event (SCXML 5.10:
  until the next event): resident again, the region, and "kept", traced
  less the region's growth (the region's blocks are PyMem_RawMalloc's,
  which tracemalloc sees), what Python's heap keeps per actor;
- region: host.stats' regionBytes after, divided by N: the host's blocks
  for the segment, sized for the population up front (maxActors), so the
  share of an actor of what the region holds whatever the datamodel.

Run by `mise run measure`, which prints the machine's signature beside the
numbers (root AGENTS.md, "Benchmarks").
"""

import argparse
import array
import json
import os
import pathlib
import platform
import resource
import subprocess
import sys
import tracemalloc

import tinyactors

# The charts are files in tools/charts/, <name>.<datamodel>.scxml.
CHARTS = pathlib.Path(__file__).resolve().parent / "charts"
DATAMODELS = ("ecmascript", "python")


def read_chart(name, datamodel):
    """The text of the chart file tools/charts/<name>.<datamodel>.scxml."""
    return (CHARTS / ("%s.%s.scxml" % (name, datamodel))).read_text(encoding="utf-8")


def resident():
    """The process's resident bytes now (Linux), else its peak."""
    try:
        with open("/proc/self/statm") as statm:
            return int(statm.read().split()[1]) * os.sysconf("SC_PAGE_SIZE")
    except OSError:
        peak = resource.getrusage(resource.RUSAGE_SELF).ru_maxrss
        return peak if sys.platform == "darwin" else peak * 1024


def region(host):
    got = []
    host.host_stats(lambda host, result: got.append(result["host"]["regionBytes"]))
    host.run_until_idle()
    return got[0]


def measure(datamodel, actors):
    # every pool sized for the population, so nothing grows: a segment that grows doubles every
    # pool (quickjs keeps each actor's last event, so maxEvents counts the actors); tracing off
    config = {"systemID": "measure", "system": {"maxActors": actors + 16, "maxEvents": actors + 4096,
                                                "tracing": False}}
    with tinyactors.Host(config) as host:
        with tinyactors.compile_charts([("fit.scxml", read_chart("fit", datamodel))]) as compiled:
            host.add_image(compiled.image)
        chart = host.chart("fit")
        warm = host.spawn(chart)  # the chart's code attached and warmed
        host.run_until_idle()
        host.terminate(warm)
        host.run_until_idle()
        tracemalloc.start()
        before = (resident(), tracemalloc.get_traced_memory()[0], region(host))
        sessions = array.array("Q", (host.spawn(chart) for _ in range(actors)))  # 8 B an actor, in traced
        host.run_until_idle()
        after = (resident(), tracemalloc.get_traced_memory()[0], region(host))
        for first in range(0, actors, 256):  # one event each: each keeps its _event
            host.send_many([(session, "inc") for session in sessions[first:first + 256]])
            host.run_until_idle()
        evented = (resident(), tracemalloc.get_traced_memory()[0], region(host))
        tracemalloc.stop()
    return {"datamodel": datamodel, "actors": actors,
            "resident": (after[0] - before[0]) / actors,
            "traced": (after[1] - before[1]) / actors,
            "region": after[2] / actors,
            "resident_evented": (evented[0] - before[0]) / actors,
            "traced_evented": (evented[1] - before[1]) / actors,
            "region_evented": evented[2] / actors,
            # traced less the region's growth: what the datamodel keeps (the
            # region's blocks come from PyMem_RawMalloc, which tracemalloc sees)
            "kept_evented": ((evented[1] - before[1]) - (evented[2] - after[2])) / actors}


def signature():
    return {"machine": platform.machine(), "system": platform.platform(), "processor": platform.processor(),
            "cpus": os.cpu_count(), "python": platform.python_version(),
            "build": "free-threaded" if not getattr(sys, "_is_gil_enabled", lambda: True)() else "with the GIL"}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--actors", type=int, default=10000)
    parser.add_argument("--json")
    parser.add_argument("--one", choices=DATAMODELS)
    options = parser.parse_args()
    if options.one:
        print(json.dumps(measure(options.one, options.actors)))
        return
    results = []
    for datamodel in ("python", "ecmascript"):
        output = subprocess.run([sys.executable, __file__, "--one", datamodel, "--actors", str(options.actors)],
                                check=True, capture_output=True, text=True).stdout
        results.append(json.loads(output))
    machine = signature()
    print("machine: %s" % ", ".join("%s %s" % (key, value) for key, value in machine.items()))
    print("bytes per actor at %d actors of one chart, resident and idle:" % options.actors)
    print("%-12s %10s %10s %10s | after one event each: %9s %8s %8s" % (
        "datamodel", "resident", "traced", "region", "resident", "region", "kept"))
    for result in results:
        print("%-12s %10.0f %10.0f %10.0f | %30.0f %8.0f %8.0f" % (
            result["datamodel"], result["resident"], result["traced"], result["region"],
            result["resident_evented"], result["region_evented"], result["kept_evented"]))
    if options.json:
        with open(options.json, "w") as out:
            json.dump({"machine": machine, "results": results}, out, indent=2)


if __name__ == "__main__":
    main()
