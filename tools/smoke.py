"""The release's smoke test of an installed wheel (mise/tasks/release).

    python -I tools/smoke.py

Run by an interpreter that has the wheel installed, never the build: on
this machine for each interpreter the release names, and in a Linux
container for glibc and musl on both architectures. It compiles the
counter chart for both datamodels, sends each actor three events, reads
the count from its log line, round-trips a value through the codec, and
prints one line naming the platform. Exits with status 1 when a check
fails.
"""

import pathlib
import platform
import sys

import tinyactors

CHARTS = pathlib.Path(__file__).resolve().parent / "charts"


def count(datamodel):
    logs = []
    with tinyactors.Host({"systemID": "smoke"}, log=lambda host, entry: logs.append(entry.text)) as host:
        chart = (CHARTS / ("counter.%s.scxml" % datamodel)).read_text()
        with tinyactors.compile_charts([("counter.scxml", chart)]) as compiled:
            host.add_image(compiled.image)
        session = host.spawn(host.chart("counter"))
        for value in (1, "two", {"three": [3]}):
            host.send(session, "inc", value)
        host.send(session, "report")
        host.run_until_idle()
    return logs


def main():
    failures = []
    for datamodel in ("python", "ecmascript"):
        logs = count(datamodel)
        if len(logs) != 1 or not logs[0].endswith(" 3"):
            failures.append("%s: the counter logged %r, not a count of 3" % (datamodel, logs))
    value = {"n": [1, 2.5, "x", None, True, b"\x00"]}
    if tinyactors.decode(tinyactors.encode(value)) != value:
        failures.append("the codec does not round-trip %r" % (value,))
    if not tinyactors.__file__.endswith(".abi3.so"):
        failures.append("tinyactors was loaded from %s, not an abi3 extension" % tinyactors.__file__)
    where = "%s %s, Python %s (%s)" % (platform.system(), platform.machine(), platform.python_version(),
                                       " ".join(platform.libc_ver()) or "no libc version")
    for failure in failures:
        print("smoke: %s: %s" % (where, failure), file=sys.stderr)
    if failures:
        sys.exit(1)
    print("smoke: ok: %s" % where)


if __name__ == "__main__":
    main()
