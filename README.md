# tinyactors for Python

SCXML actors in a host, driven from Python: charts written in Python (the
`"python"` datamodel) or in ECMAScript (quickjs), a `Host` driven from a
script or from asyncio, and values that are Python objects. The extension
is the direct layer: each item mirrors a call of the C library
([docs/api-map.md](docs/api-map.md) names the core behind every one).

```python
import tinyactors

CHART = """<scxml xmlns="http://www.w3.org/2005/07/scxml" version="1.0"
               datamodel="python" name="greeter" initial="idle">
  <datamodel><data id="greeted" expr="0"/></datamodel>
  <state id="idle">
    <transition event="hello">
      <assign location="greeted" expr="greeted + 1"/>
      <log label="greeter" expr="f'hello, {_event.data[&quot;who&quot;]} ({greeted})'"/>
      <send type="urn:example:app" target="out" event="greeted">
        <content expr="{'count': greeted}"/>
      </send>
    </transition>
  </state>
</scxml>"""

def app(host, send):                      # an I/O processor: the actor's output
    print("app got", send.event, send.data)
    send.answer("thanks")                 # an event back to the sender

config = {"systemID": "demo", "ioProcessors": [{"type": "urn:example:app"}]}
with tinyactors.Host(config, {"urn:example:app": app},
                     log=lambda host, entry: print("log:", entry.text)) as host:
    with tinyactors.compile_charts([("greeter.scxml", CHART)]) as compiled:
        host.add_image(compiled.image)    # the host copies the image
    session = host.spawn(host.chart("greeter"))
    host.send(session, "hello", {"who": "world"})
    host.run_until_idle()
    host.actor_context(lambda host, result: print(result["variables"]), session=session)
    host.run_until_idle()                 # the reply's handler runs at a later drain
```

A typed method such as `actor_context` returns nothing: its handler runs
when the reply is drained, so a script drives the host once more to read
it (above: one `run_until_idle()` for the event, one for the reply).
Under asyncio, `await host.idle()` does the same.

## Installing

    pip install tinyactors

One wheel per platform, for CPython 3.13 and later (the stable ABI):
macOS 11 or later on Apple silicon and Intel, and Linux on x86-64 and
arm64 with glibc 2.17 or later or musl 1.1 or later (one file serves
both). There is no source distribution, so other platforms, and the
free-threaded interpreters (3.13t, 3.14t), have no install yet.
Development builds of `main` are published as dev releases: once a
release is out, `pip install --pre tinyactors` asks for the newest of
them.

## The API

- `compile_charts(sources, resolver=None) -> Compiled`: `(name, text)`
  pairs into one image. A resolver is a callable `resolver(base, source)
  -> str` that reads a static `src=` at compile time; it refuses
  with `ResolveError(message, line=None, column=None)`. A compilation with
  an error raises `CompileError` with every diagnostic. `Compiled.image`
  is a read-only buffer, valid until `close()` (a context manager).
- `compile_system(manifest, resolver=None) -> Compiled`: a `(name, text)`
  manifest, a `<system>` of `<chart name src>`s (read through the
  resolver) and `<actor name chart>`s, into one image whose actors
  `boot(segment)` spawns under their names.
- `Host(config, io_processors=None, *, log=None, clock=None,
  telemetry=None, wake=None, migrated=None, services=None,
  migration_done=None)`:
  `config` is the host's config as a dict
  ([host-config.md](../tinyactors-core/docs/host-config.md));
  `io_processors` maps an I/O processor's type to `handler(host, send)`;
  `log` gets each `<log>` line as `handler(host, entry)`; `clock` is a
  `SimulatedClock` or `None` for the real clocks; `telemetry`
  gets each OTLP request as `handler(host, signal, data)`, `wake` each
  hibernated actor an event or a migration asks for as `handler(host,
  session, migration)`, `migrated` each actor a migration moved as
  `handler(host, migrated)`, and `migration_done` each migration's end
  as `handler(host, result)` (all below).
- Inputs, applied at the call: `add_image(image) -> segment`,
  `chart(name) -> ChartRef`, `spawn(ref, name=None, params=None,
  traceparent=None) -> session`, `send(target, event, data=None, *,
  traceparent=None, origin=None)` (`target` a session or a local
  address), `send_many([(session, event, data, traceparent),
  ...]) -> accepted`, `terminate(session)`, `boot(segment)` (the image's
  boot actors: its `<system><actor>`s, or the first chart compiled, each
  under its name). `actor_by_name(name) -> session` finds a live actor
  spawned with a name (`None`: none).
- Drivers: `run_until_idle()`; `run()`, which sleeps to each deadline
  while there is one; `SimulatedClock.increment(seconds)`; and under
  asyncio (`async with Host(...)`), `await host.idle()`. `host.next_deadline`
  is the seconds from now to the next deadline (`None`: none), which a
  `SimulatedClock` can be incremented by. Every time the API takes or
  gives is in seconds (an int or a float); the host converts with the
  config's `ticksPerSecond`.
- An I/O processor answers with `send.answer(event, data=None, *,
  traceparent=send.traceparent, origin=None, raw=None)` (`origin` and
  `raw` become `_event.origin` and `_event.raw`: where to reply, and the
  transport's message) or fails the send with `send.fail()`.
- Typed methods, one per method of
  [requests.md](../tinyactors-core/docs/requests.md) but `actor.send`
  (which is `send`):
  `actor_state`, `actor_context`, `actor_list`, `actor_get`, `host_stats`,
  `host_config`, `trace_configure`, `trace_watch`, `trace_unwatch`,
  `trace_end`, `trace_spans_decode`, each `(handler, on_error=None, /,
  **params)`, returning `None`; `on_error` may also be passed as
  `on_error=`. The method's params are the other keywords, passed
  to core as given; `handler(host, result)` or `on_error(host, error)`
  runs when the reply is drained, later. An error without `on_error` is
  raised by the driver after the turn.
- `send` and `entry` are thin views: their fields are read when you read
  them, and a read after the batch's ack raises `TinyactorsError` with
  `status == "TA_ERROR_STALE"`. Read what you need inside the handler. An
  `async def` I/O processor (under asyncio only) gets a record instead,
  which it can answer later. A handler that raises before answering has
  its send failed (the sender gets `error.communication`), and the
  exception is raised from the driver after the turn.
- `encode(value) -> bytes`, `decode(data) -> value`: the value table
  ([docs/values.md](docs/values.md)) as CBOR.

A `Host` is used from the thread that made it; other threads are refused.
Several hosts, one per thread, talk through the I/O processor each
config marks `"remote"` (below, "Other systems").

## Telemetry

With `"telemetry"` in the config the host traces its actors and writes
OTLP requests (protobuf): spans of every macrostep, `<log>` lines and
failed actions as log records, and counters as metrics. The host does no
I/O: the binding hands each request to your handler as `bytes` you keep,
`signal` being `"traces"` or `"logs"`, and you post it. Metrics are
pulled with `host.metrics()`. Leaving the `Host` (`with`, `async with`,
`close()`) ends the spans still open (`trace.end`) and hands over the
last requests; a `Host` the collector frees does not, so leave it. Posting,
retrying and capping what waits are yours, and the handler runs in the
turn: a slow post holds the loop (and, under asyncio, the event loop),
so give it a timeout, or make it an `async def` handler, which runs as a
task (`async with` awaits the last take's). A handler's exception is
raised by the driver after the turn, the first of a turn; every request
whose handler raised is lost.

```python
import urllib.request

def export(host, signal, data):           # OTLP/HTTP, to a collector or otel-tui
    request = urllib.request.Request("http://localhost:4318/v1/" + signal, data,
                                     {"Content-Type": "application/x-protobuf"})
    urllib.request.urlopen(request, timeout=2).close()

config = {"systemID": "shop",
          "telemetry": {"flushInterval": 5_000_000_000,       # host clock: ns by default
                        "resource": {"service.name": "shop"}}}
with tinyactors.Host(config, telemetry=export) as host:
    ...
    export(host, "metrics", host.metrics())
```

**Application spans above actors.** A `traceparent` given to `send` or
`spawn` makes the macrostep it starts a child of the application's span,
in the application's trace; sends between actors keep the trace, and an
I/O processor gets the sending step's context in `send.traceparent`.
With the OpenTelemetry SDK:

```python
from opentelemetry import propagate, trace

tracer = trace.get_tracer("shop")

def warehouse(host, send):                # continue the actor's trace in the application
    with tracer.start_as_current_span("warehouse.reserve",
                                      context=propagate.extract({"traceparent": send.traceparent})):
        carrier = {}
        propagate.inject(carrier)
        send.answer("reserved", traceparent=carrier["traceparent"])

with tracer.start_as_current_span("POST /orders"):
    carrier = {}
    propagate.inject(carrier)
    host.send(session, "order", order, traceparent=carrier["traceparent"])
    host.run_until_idle()
```

A backend then shows one trace: `POST /orders`, the actors' macrosteps
and microsteps below it, `warehouse.reserve` below the step that sent to
the warehouse, and the macrostep its answer started below that.

`flushInterval` is in the host's clock (`system.ticksPerSecond`); without
it a request is ready only when the staging is full, or at the stop.
`host.trace_spans_decode(handler, traces=data)` decodes a traces request
back into spans, and `trace_configure` and `trace_watch` choose what is
traced ([requests.md](../tinyactors-core/docs/requests.md)). A watch adds
rules for a while, on the host's clock, in front of the configured ones:
with tracing sampled down, it traces one chart in full until it ends.

```python
host.trace_configure(print, sample=0.01)                 # 1% of new traces
host.trace_watch(lambda host, result: print("watch", result["id"]),
                 rules=[{"chart": "checkout", "sample": "sample", "detail": "full"}],
                 seconds=60)                             # every checkout, for a minute
```

## Hibernation

A host keeps millions of actors when only some are in memory: the rest
are blobs in the application's store. The host does no I/O; the binding
hands each blob over as `bytes`, and asks for it back.

- `host.sweep(max_scan, limit) -> [session, ...]`: the CLOCK sweep, at
  most `limit` actors that did not run since the previous sweep and can
  be hibernated now.
- `host.hibernate(session) -> bytes`: the actor leaves memory, and its
  blob is yours to store. An actor that is not at rest (events waiting,
  in a macrostep, an invoked child) raises `TinyactorsError`, and so does
  a `"python"` actor holding a value outside the value table (a `set`, a
  `tuple`, an object): it stays resident. A pending delayed send stays
  in the host, and when it falls due it wakes the actor.
- `wake=handler(host, session, migration)`: an event (or a due delayed
  send) came for a hibernated actor (`migration` is `None`), or a
  migration asks for it (`migration` is its id, below); it waits in the mailbox while your handler
  loads the blob and calls `host.wake(session, blob)`, now or later (an
  `async def` handler runs as a task, under `async with`). One call per
  hibernation, however many events come. Without a `wake` handler the
  call is dropped: the actor stays hibernated, and its events wait, until
  you call `host.wake`.

```python
import sqlite3

db = sqlite3.connect("actors.db", isolation_level=None)
db.execute("CREATE TABLE IF NOT EXISTS blobs (session INTEGER PRIMARY KEY, blob BLOB)")

def wake(host, session, migration):
    (blob,) = db.execute("SELECT blob FROM blobs WHERE session = ?", (session,)).fetchone()
    host.wake(session, blob)

def evict(host):                          # after a turn: the cold actors leave
    for session in host.sweep(1 << 20, 1024):
        try:
            blob = host.hibernate(session)
        except tinyactors.TinyactorsError:    # not at rest, or a value outside the table
            continue
        db.execute("INSERT OR REPLACE INTO blobs VALUES (?, ?)", (session, blob))
```

**Plan the capacity up front.** A hibernated actor keeps its slot and its
session's record in the host (about 60 B), so `system.maxActors` counts
every actor; instance and string memory are needed for the resident ones
only. Left to grow, a segment doubles all its pools together and sizes
instance memory from `maxActors`, about 31 KB per actor at two million.
For `R` actors resident out of `N`:

```python
config = {"systemID": "big", "system": {
    "maxActors": N + room, "maxEvents": 4096,
    "instanceBytes": 2048 * (R + room), "stringBytes": 16384 + 64 * (R + room)}}
```

`mise run stress` is the pattern at scale (`tools/stress.py`): two
million actors of a counter chart, at most 1000 resident, the rest in
SQLite; 200,000 events to actors drawn at random, so nearly every event
wakes one from the store and the sweep sends another back. Measured on 2026-10-15, x86_64 Linux, 4 CPUs, CPython 3.13.16 with the GIL, the gates' configs sized for their work ("Sizing the config"), one run:

| datamodel | populate (spawn and hibernate 2,000,000) | 200,000 events (woken, swept back) | peak resident | core's region | SQLite file |
|---|---|---|---|---|---|
| `"python"` | 15.8 s (126,000 a second) | 14.7 s (13,600 a second; 199,869 wakes) | 164 MB (82 B per actor) | 115 MB (58 B per actor) | 355 MB |
| quickjs | 25.9 s (77,000 a second) | 17.3 s (11,500 a second; 199,869 wakes) | 165 MB (82 B per actor) | 115 MB (58 B per actor) | 409 MB |

The region is core's (about 58 B per actor, hibernated or not); the
store is SQLite's file. Every actor's count matched the events it was
sent, and after every eviction at most 1000 actors were resident.

## Migration

A new version of a chart (`add_image` of an image with a chart of the
same `name`) runs the actors spawned from then on; `host.migrate` moves
the running ones to it, at their stable points, in the next runs. Each
moved actor keeps its session id, its state configuration (states match
by id) and its delayed sends, and gets `error.platform.upgrade` first,
whose data is version 1's `<datamodel>` bindings: version 2 takes from
there what it keeps.

```xml
<transition event="error.platform.upgrade">
  <assign location="n" expr="_event.data['n']"/>
</transition>
```

- `host.migrate(*, chart=None, session=None, wake=False, timeout=0) ->
  migration`: starts a migration of every chart, one `chart` by name, or
  one `session`, and returns its id. With `wake`, a hibernated actor is
  asked for (`wake=handler(host, session, migration)` with this
  migration's id); without it, it waits, and the migration ends with it
  in `pending` at `timeout` (seconds; 0: none).
- `migration_done=handler(host, result)`, a handler of the host: each
  migration's end, with `{"migration", "migrated", "failed", "pending":
  [session, ...]}`. Being the host's, it also gets the end of a migration
  a restored host goes on with.
- `migrated=handler(host, migrated)`: each actor moved, a thin
  `Migrated` view of `session`, `segment`, `chart` (its name) and
  `migration`.
- **A migration ends when every actor it looks at moved, or at its
  `timeout`.** A hibernated actor moves once it is woken, by the migration's
  WAKE or by an event's; with `wake`, the migration asks again when it finds an
  actor hibernated after a wake. Without `wake`, an actor nobody wakes
  holds the migration until its timeout, or for good without one, and its
  handler never runs: give such a migration a `timeout`. A run costs the migration a
  quantum of actors (`migrateQuantum`), however many it waits for.
- An actor that cannot be written out stays on its version and counts as
  failed: on the `"python"` datamodel, one whose variables or pending
  delayed sends hold a value outside the value table. One that is running
  an invocation waits until it ends.

**With most actors hibernated, wake the migration's actors a batch at a
time.** The migration asks for every hibernated actor at once, early; wake them
all and they are all resident. Queue the migration's WAKEs and wake a batch
per round: the migration takes each woken actor in its next runs. An event's
WAKE (`migration` is `None`) wakes its actor at once, and the migration takes that
one too. Put actors back to sleep once they moved (one put back before
is asked for again: work, not a stall).

```python
asked = collections.deque()

def wake(host, session, migration):
    if migration is not None:
        asked.append(session)             # the migration's: a batch at a time
    else:
        host.wake(session, store.get(session))   # an event's: now

host = tinyactors.Host(config, wake=wake, migration_done=on_done)
...
migration = host.migrate(wake=True)
while not done:
    for _ in range(min(256, len(asked))):
        session = asked.popleft()
        host.wake(session, store.get(session))
    host.run_until_idle()
    evict_moved(host)                     # the moved actors at rest go back to the store
```

`mise run migration` is the pattern at scale (`tools/migration.py`), on
SQLite: 2,000,000 actors of a counter chart, at most 1000 resident, moved
to version 2 while 200,000 events go to actors drawn at random; it
checks every sampled actor's count and version, that the migration left none
behind, that version 1's segment retired, and the resident bound after
every eviction. Measured on 2026-10-15, x86_64 Linux, 4 CPUs, CPython 3.13.16 with the GIL, the gates' configs sized for their work ("Sizing the config"), one run:

| datamodel | populate (2,000,000, 200,000 events) | migrate, live (200,000 events meanwhile) | woken from SQLite | peak resident | core's region (both versions, then after) | SQLite file |
|---|---|---|---|---|---|---|
| `"python"` | 59.9 s | 129.7 s (15,400 actors a second) | 2,218,199 | 473 MB (237 B per actor) | 230.9 MB, then 115.5 MB (58 B per actor) | 556 MB |
| quickjs | 71.1 s | 153.3 s (13,000 actors a second) | 2,218,199 | 475 MB (238 B per actor) | 230.9 MB, then 115.5 MB | 756 MB |

A run costs the migration a quantum of actors however many it waits for, so
the time grows with the population: 13.6 s at 500,000 on the
`"python"` datamodel, against 74.9 s when the migration went over everything
it waited for in every run. The peak is the migration's: both versions'
regions until version 1 retires, and the migration's map of the sessions it
waits for (at most a quarter full after it is rebuilt).

## Snapshots

`host.snapshot() -> bytes` is the whole host in one blob: every
segment's region (actors, queues, timers, the actors' hibernation
records), both datamodels' sessions and payloads, the images, the system
id and the host's time. `tinyactors.Host.restore(snapshot, config, ...)`
makes a new host running it, with the config it was taken with (its
system id is checked; its I/O processors and tick rate are not in the
snapshot) and the handlers `Host(...)` takes. Every session id stays,
and the host's clock goes on from the snapshot's time, whatever clock
reads it: a new `SimulatedClock`, or the real one in another process or
on another machine, is offset to it.

```python
blob = host.snapshot()                    # quiescent: not from a handler
host.close()
...
host = tinyactors.Host.restore(blob, config, log=log, wake=wake)
```

- **A snapshot is for the same build of the library, not a backup.** It
  holds raw region memory. It is for a copy of the whole application,
  every chart version at once: rehearse a deploy on it (add the new
  image, migrate, run the clock ahead, look), capture a host when
  something goes wrong and step through it on the same build, start
  every test from one prepared world; and for taking a host out of the
  process for a while. Durability is your database's; to keep actors out
  of memory in a running process, hibernate them.
- **Invoked services are started again.** The services you run are not
  in a snapshot. The restored host's first run announces each invocation
  that was running, and the handler you pass as `services=` to
  `Host.restore` gets it, with `invocation.restored` true and the same
  `invokeid`, `src` and params: pass your real handlers to go on, stand-ins
  for a copy that must not reach the real services, or none for a type,
  and its invokers get `error.communication`.
- **The hibernated actors stay in your store.** The snapshot holds their
  records, not their blobs; the restored host asks for them with the
  same WAKE.
- It needs a quiescent host: it is refused from a handler, with an
  effect or a typed method's reply not yet handled (run the host first),
  and while a migration runs (`TA_ERROR_STATE`); and when a
  `"python"` session or payload holds a value outside the value table
  (`TA_ERROR_UNSUPPORTED`). The host goes on either way. Values are
  written through the value table: two variables of one session that
  share a list share it no more after a restore.

`mise run snapshot` is the pattern at scale (`tools/snapshot.py`): a
deploy rehearsed on a copy of a live host. The live host has its
population on SQLite as in "Hibernation", 200,000 events, a delayed send
for every resident actor, and half of those paying through an invoked
payment service that has not answered. Its snapshot is written to a
file, its store copied (a copy writes blobs too), and a copy restored
from the file with a stand-in payment service, which gets every payment
in flight again (`restored`, with its amount) and declines it. The copy
must snapshot to the same bytes before it runs; then it adds version 2
and migrates every actor, waking the hibernated ones a batch at a time,
with 200,000 events meanwhile, while the live host goes on with 200,000
events of its own. Both hosts' sampled actors must hold every count,
delayed send and payment, the copy's on version 2 and declined, the
live host's on version 1 and still paying, and the copy must never reach
the real service. Measured on 2026-10-15, x86_64 Linux, 4 CPUs, CPython 3.13.16 with the GIL, the gates' configs sized for their work ("Sizing the config"), one run each, both hosts in one process:

| datamodel | actors | resident | snapshot | snapshot time | restore time | migrating the copy | peak resident, both hosts | every actor right |
|---|---|---|---|---|---|---|---|---|
| `"python"` | 2,000,000 | 1000 | 116.1 MB (58 B per actor) | 0.14 s | 0.12 s | 145 s | 652 MB | yes |
| quickjs | 2,000,000 | 1000 | 116.2 MB | 0.14 s | 0.13 s | 172 s | 654 MB | yes |
| `"python"` | 100,000 | all | 56.6 MB (566 B per actor) | 0.66 s | 0.53 s | 22.3 s | 427 MB | yes |
| quickjs | 100,000 | all | 64.0 MB (640 B per actor) | 3.43 s | 1.29 s | 21.6 s | 684 MB | yes |

A snapshot is mostly the region, as the config sized it: the same 100,000
resident actors made a 372 MB snapshot and a 1761 MB peak before the
gate's config was sized for the work. Migrating 2,000,000
mostly hibernated actors is the store's work: each is woken, moved and
hibernated again (2,018,332 wakes, events included).

## Invoked services

A chart invokes a service the application runs with `<invoke>` of a type
the application registered: `Host(..., services={type: handler})`. The
handler runs when the invocation starts, with an `Invocation`, and
returns the service: any object, kept until the invocation ends.

```xml
<state id="talking">
  <invoke id="counter" type="urn:example:counter" src="counter://local">
    <param name="seed" expr="seed"/>
  </invoke>
  <transition event="go"><send target="#_counter" event="ping"/></transition>
  <transition event="pong">...</transition>
  <transition event="done.invoke.counter" target="finished"/>
</state>
```

```python
class Counter:
    def __init__(self, invocation):
        self.invocation, self.count = invocation, 0

    def receive(self, host, send):            # a <send target="#_counter">, or autoforwarded
        self.count += 1
        if send.event == "finish":
            self.invocation.done({"total": self.count})   # done.invoke.counter
        else:
            self.invocation.send_back("pong", {"n": self.count})

    def stop(self, host):                     # the invoker cancelled it (left the state, ended)
        ...

host = tinyactors.Host(config, services={"urn:example:counter": lambda host, invocation: Counter(invocation)})
```

- `invocation`: `session` (the invoker), `invokeid`, `type`, `src`,
  `data` (the params, or the content), `traceparent`, `running`,
  `restored` (started again after `Host.restore`, "Snapshots"); and
  `send_back(event, data=None)` (an event from the invoked child, its
  `invokeid` set) and `done(data=None)` (`done.invoke.<invokeid>`). It is
  a record: keep it and answer later, as long as it runs; after `done`
  or a cancel, answers raise `TinyactorsError` (`TA_ERROR_STALE`).
- The service's `receive(host, send)` gets each send to it as a thin
  view (`event`, `data`, `sendid`, `traceparent`; `type` is `None`), and
  its `stop(host)` the cancel; both are optional. A service that ended
  with `done` is not stopped, nor kept: a handler may call `done` and
  return.
- A service that fails is `error.communication` for its chart, whatever
  failed: no handler for the type, a handler that raises, a `receive`
  that raises (the service crashed: it is dropped, not stopped), or no
  handler for a restored invocation. The invoker gets it (unless it
  cancelled the invocation in the same run), and each send to the
  invocation gets one too until the invoker leaves the state. The driver
  raises the exception after the turn; a restored invocation with no
  handler raises nothing (starting it again was yours to decline).
- Leaving the `Host` drops the running services without stopping
  them.

`mise run services` is the pattern at scale (`tools/services.py`):
200,000 actors, each with a counter service, 1,000,000 round trips
(actor, service, actor), then a third finish with done data, a third are
cancelled and the rest keep theirs; it checks every pong, every done
datum, that every cancelled service was stopped once and no other, and
that the host holds only the services still running. Measured on 2026-10-15, x86_64 Linux, 4 CPUs, CPython 3.13.16 with the GIL, the gates' configs sized for their work ("Sizing the config"), one run:

| datamodel | start 200,000 invocations | 1,000,000 round trips | end (done, cancel) | peak resident | core's region |
|---|---|---|---|---|---|
| `"python"` | 3.1 s (64,000 a second) | 38.3 s (26,000 a second) | 4.5 s | 408 MB (2041 B per actor and service) | 50 MB (251 B per actor) |
| quickjs | 5.0 s (40,000 a second) | 62.6 s (16,000 a second) | 5.9 s | 711 MB (3553 B per actor and service) | 62 MB (309 B per actor) |

This machine ran slower than the one of 2026-10-14's numbers: the same
build (the services commit) does 28,000 round trips a second here
against 55,000 there. Side by side on this machine, five runs each
alternating, today's build does 25,900 a second against that build's
28,700: about 10 percent slower, within each one's spread of 15
percent; the likely cost is the params each running invocation now keeps
for a restore (unverified).

An invocation is a request of core's: size `system.maxRequests` for the
invocations that run at once. quickjs keeps each actor's last event in
core's event pool (its datamodel does not let it go), so on quickjs
`system.maxEvents` counts the actors too; left too small, a segment
doubles every pool (980 MB of region here).

## Other systems

How systems talk (threads and a queue, a WebSocket to a browser, HTTP) is
the application's choice. A chart addresses another system with the plain
SCXML target, `tinyactors://<systemID>/#_scxml_<n>` or `.../@<name>`; the
host delivers a target of its own system itself, and hands every other one
to the I/O processor the config marks `"remote"` (at most one):

```python
config = {"systemID": "a", "ioProcessors": [{"type": "urn:app:mesh", "remote": True}]}

def mesh(host, send):            # every send to another system
    queue_for(send.target).put((send.target, send.event, send.data, send.origin))

host = tinyactors.Host(config, {"urn:app:mesh": mesh})
```

The other side delivers it as an input, by address, with the sender's
address as origin, so a chart's reply to `_event.origin` goes back the
same way:

```python
target, event, data, origin = queue.get()
other.send(target, event, data, origin=origin)   # _event.origintype is SCXML's
```

A send the transport took needs no answer; one it cannot deliver is
`send.fail()`, and the sender gets `error.communication`. The data is a
Python value: encode it for the wire as you like (`tinyactors.encode` gives
the value table's CBOR). Without a remote processor, a send to another
system is `error.communication`.

## Charts in Python

A `datamodel="python"` chart's session is an instance of a class made for
the chart: its variables are its slots (its `<data>` ids, and the names
a `<foreach>`'s `item` or `index` or a `<script>` outside its defs
assigns, which the chart declares too, as SCXML 4.6 and a module's top
level do; each is `None` until set), `_event`, `_sessionid`, `_name`
and `_ioprocessors` are read-only, and `In(state)` is SCXML's `In()`.
Expressions are compiled once per chart into functions of the session,
by a restrictive rewrite:

- *expressions:* literals, arithmetic, comparisons, boolean operations,
  conditional expressions, indexing and slicing, comprehensions and
  generator expressions, f-strings, calls to the chart's methods and to
  the allowed builtins, attribute access except dunder names;
- *statements*, in `<script>` and `.py` files a `src=` names: assignment,
  plain or augmented, to a variable or to its items or attributes;
  `if`, `for`, `while`, `pass` and expression statements; `def` at the top
  level of a script, which makes a method of the chart;
- *refused:* `import`, `global`, `nonlocal`, `lambda`, `await`, `yield`,
  the walrus (`:=`), `del`, `try` and `raise`, `with`, `class`;
- *builtins:* `len`, `min`, `max`, `sum`, `abs`, `round`, `sorted`,
  `reversed`, `enumerate`, `zip`, `range`, `any`, `all`, `isinstance`,
  `hasattr`, `str`, `int`, `float`, `bool`, `list`, `dict`, `tuple`, `set`,
  `frozenset`, `map`, `filter`.

An undeclared name, a refused construct or an assignment to a system
variable is `error.execution` when the expression is evaluated, as SCXML
requires; a `.py` file's refused constructs are compile errors at their
line. What crosses to another actor (`<send>` data, `<donedata>`, invoke
params, data from the application) is copied through the value table:
actors share no mutable value.

Inside a chart, an expression is XML text: `<` is written `&lt;`, `&` is
`&amp;`, and a quote inside a quoted attribute is `&quot;`. Write any
logic longer than a comparison as a method in a `.py` file the chart
names with `<script src>`, and call it from the condition; the file is
plain Python, checked by the same rewrite at compile time:

```python
# rules.py
def under_limit(tries, limit):
    return tries < limit
```

```xml
<script src="rules.py"/>
...
<transition event="retry" cond="under_limit(tries, 3)" target="trying"/>
```

The resolver given to `compile_charts` reads `rules.py`, for example
`lambda base, source: pathlib.Path("charts", source).read_text()`.

## Limits

- **Charts are trusted code. The rewrite is not a sandbox:** it keeps a
  chart inside SCXML's scope rules, nothing more. A chart in Python runs
  in your interpreter, cannot be preempted in the middle of an
  expression, and is limited in memory by nothing (unlike quickjs, which
  has an operation limit and its own allocator).
- **Python values lie outside the host's allocator.** The host, its
  tables and its scratch allocate through `PyMem_RawMalloc`; the values
  of a Python chart's sessions and payloads are Python objects on
  Python's heap.
- **Nesting is limited to 64,** which stops cycles but not a value that
  shares subtrees (`a = [a, a]` sixty times): another reason charts are
  trusted code.
- A `"python"` actor hibernates and migrates only while its variables,
  and the data of its pending delayed sends, are values of the value
  table.

## Memory

Measured with `mise run measure` (2026-10-15, x86_64 Linux, 4 CPUs,
CPython 3.13.16 with the GIL; 10,000 actors of one chart with three
`<data>`, resident and idle, every pool sized for them and tracing off;
then once more after one event to each actor, which keeps its `_event`
until the next):

| datamodel | resident per actor | traced per actor | core's region per actor | resident after one event | region after one event |
|---|---|---|---|---|---|
| `"python"` | 387 B | 188 B | 268 B | 571 B | 268 B |
| quickjs | 1066 B | — | 268 B | 1500 B | 268 B |

"Traced" is tracemalloc's: the sessions' Python objects and the
binding's rows (it includes an 8 B array of the harness). After an event
a `"python"` session keeps one more object, its `_event` (about 100 B;
its name, invoke id, origin and origin type are strings its chart, or
the host, holds once for every session, found by core's ids for them).
With the pools sized, the region does not grow after the events; left
at its default `maxEvents` (256), quickjs's grew to 12,440 B per actor,
since a segment that grows doubles every pool.

### Sizing the config

Most of a host's memory is what its config reserves, not what its actors
use: core sets its pools up front and never allocates after that. The
gates' first configs reserved four to sixteen times what they used; sized
for the work, the 100,000 resident actors of the snapshot gate went from
362 MB of region to 47 MB, and the services gate's 200,000 actors from
269 MB to 50 MB. What a config should say (the core's host-config
documentation has every key):

- `maxActors`: every actor, resident or hibernated (a hibernated one costs
  about 58 B of region), with some room.
- `instanceBytes` and `stringBytes`: for the resident actors only, at
  what their charts use (128 B and 16 B per actor for the gates' charts);
  left out, they are derived from `maxActors`, which counts the
  hibernated ones too.
- `maxEvents`: the events in flight at once, and on quickjs one more per
  resident actor (it keeps each actor's last event).
- `maxRequests`: the invocations of services that run at once.
- `tracing`: on by default; it takes 512 B of journal and a context per
  `maxEvents` slot. Turn it off where nobody exports traces.
- Size, do not let it grow: a segment that runs out of any pool doubles
  every pool at once.

## Building

`mise run build` builds the extension for CPython 3.13 (abi3) and for the
free-threaded test interpreter; `mise run test` lints, runs `tests/` on
both, and runs the conformance suite's milestone set against the adapter
(`python -m conform`) on both datamodels.

`mise run wheel` builds the wheels, one per platform, with zig from any
machine, into `build/wheels/`, and smoke-tests this machine's in fresh
environments for 3.13 and 3.14. `mise run release -- <version>` checks,
builds, verifies and smoke-tests a release, in Linux containers too
(Docker; `--skip-containers` leaves them out); `--publish` tags it,
uploads it to PyPI and creates the GitHub release, and `--dev --publish`
uploads `main` as a prerelease.

## License

MIT, as the repository ([LICENSE](../LICENSE)).
