# Roadmap: tinyactors-cpython

**Step 8.4 is implemented and measured (2026-10-14), most of step 8.5
is done, and the API review is applied (2026-10-15; below, "The API
review"), and packaging is built (2026-10-15: `mise run wheel`, `mise
run release`; below, "Packaging"); next: the first release, and the
examples.** The first binding (M1 of the
earlier plan, 2026-10-12) was removed (`6426816a`); the rewrite follows
the design of [AGENTS.md](AGENTS.md), with the changes below ("Decided in
8.4, for the API review"). The design (2026-10-13) is in [AGENTS.md](AGENTS.md) ("The API",
"The loop", "Lifetimes and thin views", "The binding's memory", "The
"python" datamodel"); there is no design document to write first. The
step starts when the root roadmap marks steps 8.2 (core implements
[ADR 0012](../docs/adr/0012-lifetimes-follow-the-loop.md): `ta_table`,
`ta_status_name`, `ta_growable_arena`, one batch at a time, the
resolver's errors and scratch arena; and `detach_event` checked) and 8.3 (the full loop end to end in
C) done. The plan of both bindings is the root roadmap's step 8, which
decides.

## Step 8.4: a minimal Python binding

From the root ROADMAP.md, step 8, "8.4"; read it there before changing
this copy.

### Plan

**Bootstrap (authorized 2026-10-13):** write a new extension and adapter
from scratch before the actor binding. The codec and driver framing
start items 6 and 8 below early; steps 8.2 and 8.3 still precede the host
and actor implementation. The first slice supports driver-frame values
(docs/values.md), and the adapter reports unimplemented methods with
-32601. M1 must fail explicitly until the real host methods exist.
The existing conformance gate stays enabled. The maintainer explicitly
requested committing this deliberately red bootstrap (2026-10-13), as
an exception to the passing-before-commit rule. Record the expected M1
failure in the commit; no suite or gate is weakened to land it.

Bootstrap verification (2026-10-13): lint passes; both the abi3 build on
Python 3.13.16 and the free-threaded build on Python 3.14.8 pass nine
codec and adapter tests. `mise run test` then reaches M1 and fails for
missing `host.create`: protocol has 170 skips and no passes, and the
scenario language probe fails. A separate
`TA_PYTHON_DATAMODELS=quickjs mise run conform -- --milestone M1` gives
the same diagnostic. Neither selection crashes or produces malformed
driver frames.

**Codec completion — done (2026-10-13):** finish the existing value codec with
`UNDEFINED` and `Address` (CBOR tag 32), as AGENTS.md's value table already
requires. Add tests driven by the shared
`values.txt`: its `python` expression lines and applicable roundtrip and
malformed cases, including the non-ECMAScript cases that preserve
references and undefined. Done means those codec cases pass on both
interpreters and the public documentation and mapping table cover the
added types. This serves the binding author and maintainer; it adds only
the already decided codec surface and work during conversion. Host and
actor execution, their per-event work, and core changes are outside this
increment. M1 still reports missing `host.create`; the overall roadmap's
next steps remain core 8.2 and the C loop 8.3 before Host implementation.

Implemented: immutable `Address(text)` with read-only `.text`, text-based
equality/hash among the same exact type and tag 32 encoding; a module-local
`UNDEFINED` singleton, preserved at every value position; and the shared
Python-expression, applicable roundtrip and malformed vector cases. Module
state owns the heap types and singleton, with GC traversal/clear and
refusal of calls before module execution. Public help, docs/values.md and
the mapping table describe the API, errors and lifetimes. Twenty tests
pass on both Python builds. The expected M1 failure is still missing
`host.create`; the gate stays enabled for this authorized bootstrap commit.

**Steps 1 to 9 — done (2026-10-14).** `mise run test` passes: lint,
both builds, 68 tests on each interpreter (`tests/`: every rule of
AGENTS.md's "Rules the suite cannot see"), and the suite's M1 set on the
native datamodel and on quickjs (94 protocol vectors and 4 scenarios
each; the 68 others skipped as the reference's or C-level). The
measurement (`mise run measure`, x86_64 Linux, 4 CPUs, CPython 3.13.16):
**423 B resident per actor on `"python"` against 1093 B on quickjs** at
10,000 actors, Python's heap 193 B and core's region 204 B of it
(AGENTS.md, "Measured cost per session"). One change outside this
project, for the maintainer to confirm: three method-level vectors of
`requests.txt` ("method is not text", "no such method", "params is not a
map") are marked `only reference`, since typed methods cannot send them
(below).

The plan, each step a commit (or a few) that passes this project's
`mise run test`:

1. **The `Host` and its memory:** `Host(config, io_processors, log,
   clock)`, the per-host `ta_growable_arena` with a mark per call, the
   tables (charts, sessions, payloads, pending), `with` and `async with`,
   `ta_host_create` and the `host.config` read; the lint rule
   `allocation-in-tables-and-host` moved to the new files.
2. **The loop:** `ta_python_drain` and `ta_python_turn` as AGENTS.md
   writes them, the synchronous driver (`run_until_idle`, `run`,
   `SimulatedClock.increment`) and asyncio (`await host.idle()`), every
   status of the table.
3. **The compiler, images and inputs** (the direct layer, AGENTS.md
   "Layers"): `compile_charts`, `Compiled` (`image`, `diagnostics`,
   `close`, `with`, `BufferError` while `image` is exported),
   `CompileError`; the resolver protocol on `compile_charts` only, with
   `ResolveError` (and a `SyntaxError` converted), and `.py` sources
   through `src` with the rewrite at resolve time; `add_image(image) ->
   int`, `chart(name)`, `spawn(ref, ...)`, `send`, `send_many` (these
   three with their `traceparent`), `terminate`; `TA_EFFECT_REQUEST`
   answered in C with `TA_ERROR_INVALID` until run-time fetching is
   built. No convenience layer (no
   `DirectoryResolver`).
4. **Effects as thin views:** I/O processors with `answer` and `fail`
   (both keeping `send.traceparent`), `send.traceparent` formatted on
   first read, a record for `async def` processors (refused under
   `with`), a failed send for a processor that raises, the log handler;
   the ack count and the stale check.
5. **Typed methods** with response handlers and the pending table.
6. **Values:** `tinyactors.encode` and `tinyactors.decode`, the value
   table's tests against `values.txt`'s `python` lines.
7. **The `"python"` datamodel:** the class per chart, the rewrite (its
   allow-list as AGENTS.md writes it), the
   instance per session, `_event` and `detach_event`, capture,
   hibernation (by handle at first; written out as CBOR since
   2026-10-14, below), quickjs beside it.
8. **The adapter** (`conform/`) and the suite's M1 set on both
   datamodels.
9. **The measurement:** a task that spawns 10,000 actors of one chart,
   resident and idle, on the `"python"` datamodel and on quickjs, and
   reports bytes per actor from the process, `tracemalloc` and
   `host.stats`' `regionBytes`, with the machine's signature (root
   `AGENTS.md`, "Benchmarks"). Its numbers replace the estimates in
   AGENTS.md ("The "python" datamodel").

### Done means

- **Scope:** the design of AGENTS.md, implemented as written; a change
  to it is the maintainer's, written into AGENTS.md first.
- **The measured bytes per actor at 10,000 actors** on the `"python"`
  datamodel beside quickjs's on the same machine, by this project's task
  (plan step 9), against quickjs's reference: a session scope ~0.5 KB in
  one shared context, the `fit` actor 2.9 KB
  (`tinyactors-core/docs/architecture.md:844`).
- **Gates**, each in `mise run test`:
  - the suite's M1 set (`--milestone M1`: every protocol vector file, the
    `basics` scenario and the outcome canaries) on the native datamodel
    and on quickjs;
  - the value table's own tests, against `values.txt`'s `python` lines;
  - the mapping table (`docs/api-map.md`): every item a core function, a
    method of `requests.md` or an effect, its *planned* marks gone;
  - the layer lints, the prefix, no mutable globals;
  - both interpreters: the abi3 build on 3.13 and the free-threaded test
    build;
  - each rule of AGENTS.md's "Rules the suite cannot see" with a test of
    its own.
- **Then the API review** by the maintainer: how a Python program and a
  Python chart read. After it, the Node session starts (8.5) and this
  binding goes on with 8.5's work for both.

### Decided (2026-10-13), no longer open

- **`dict` and `list` subclasses** are refused by the value table (exact
  types only; a conversion runs no user code).
- **The rewrite's allow-list** is AGENTS.md's ("The "python"
  datamodel"); the README says it is not a sandbox.
- **`add_image(charts)`** left the direct layer: `compile_charts`,
  `Compiled`, `add_image(image) -> int`, `chart(name)` and `spawn(ref,
  ...)` replace it; a mapping form belongs to the convenience layer,
  later.
- **The log entry** is `(session, label, text)`, no chart.
- **Export of a `"python"` actor:** nothing is done; unsupported, with
  the known limitation that an exported actor's row stays hibernated
  until `ta_host_free` (AGENTS.md). Superseded 2026-10-14 ("Hibernation",
  below): `serialize` writes the session out, so the limitation is gone;
  the binding has no export call yet.
- **An `async def` processor under the synchronous driver** raises
  `TypeError` naming it when the `Host` is entered with `with`.
- **A processor that raises before answering** (sync or async) has its
  send failed (`send.fail()`, `error.communication` for the sender), and
  the exception is raised from the driver after the turn.
- **The resolver lives only at compile time** (ADR 0012, decision 7): the `Host` has no resolver. A
  `TA_EFFECT_REQUEST` (a fetch at run time, which stays: ADR 0012,
  decision 8) is answered in C with `ta_host_resume` and
  `TA_ERROR_INVALID` (`error.execution` in the parent): core has already
  normalized the name and looked in the requesting segment's image
  (`tinyactors-core/src/system/route.c:439-497`). How it will be
  answered is decided with the build-out of run-time fetching (root
  `ROADMAP.md`, "Decided later"); not now, and it is not the
  control plane.
- **`Compiled.close()` while a `memoryview` of `image` is held** raises
  `BufferError`, as `bytearray` does.
- **`DirectoryResolver`** is the convenience layer's, pure Python,
  reading files under its root; the direct layer has the resolver
  protocol only (`ta_runtime_resolve` does not confine reads to a root).
- **A Python resolver refuses** by raising
  `tinyactors.ResolveError(message, line=, column=)`; a `SyntaxError` or
  the rewrite's refusal is converted with its `lineno` and `offset`. The
  extension fills `ta_resolve_error`, the message in the compilation's
  scratch arena.
- **`traceparent`:** in, W3C text passed as the `str`'s UTF-8 for the
  call (`send(..., *, traceparent=None)`, `send_many` items, `spawn(...,
  traceparent=None)`), parsed by core; out, `send.traceparent`, formatted
  from the effect's `ta_trace_context` on first read (`None` when
  untraced); `send.answer(..., traceparent=send.traceparent)` keeps the
  trace by default, `None` starts a new one; `send.fail()` carries it.

### Decided in 8.4, for the API review

Made while implementing, each the least the gates or the platform
needed; the maintainer confirms or changes them at the API review.

- **Typed methods take their params as keywords:** `method(handler,
  on_error=None, /, **params)`, passed to core as given. The method-level
  vectors (a param a method does not take, of the wrong type, missing)
  then reach core through the API; `actor_state(session, handler)` could
  not leave `session` out or pass `"x"`.
- **Three request vectors are the reference's:** "method is not text",
  "no such method" and "params is not a map" assumed the request facade
  ADR 0012 removed; no typed method can send them. They carry `only
  reference` in `tinyactors-test/suites/protocol/requests.txt` (a change
  to the suite, made in this step, `e738095`); ta-conform still runs them.
- **Peers** (`peer=`, `peer_add`, `deliver`) were added here to run M1's
  envelope vectors and removed with core's router (ADR 0014, 2026-10-15):
  other systems are the remote I/O processor and `send` by address.
- **A resolver's other exceptions** (an `OSError`, a bug) are raised from
  `compile_charts` after the result is freed; the compiler's diagnostic
  for that `src` is not reported.
- **The rewrite is Python** (`src/datamodel/support.py`, embedded by the
  build into the extension, run once per interpreter): the AST is not in
  the limited C API. A `.py` file's constructs are checked as the
  resolver reads it; its names when its chart is attached (the resolver
  does not know the chart's `<data>`).
- **`_event` holds its data's value**, not a reference on the payload row.
- **Any script's top-level `def`s** are the chart's methods.
- **The system variables** are made at `create` only for a chart whose
  code reads them (the rewrite records which).
- **`run()`** returns when the host is idle with no deadline left.
- **An input during a turn** (an answer, a typed method, a resume) makes
  the turn yield and turn again, rather than report idle; a turn is
  bounded at 64 runs and a drain at 64 batches, so a handler that answers
  every send (a ping-pong) never holds the event loop.

### The memory and throughput review (2026-10-14)

A fresh agent reviewed the binding against the memory and throughput
rules; what it found is fixed:

- a turn had no bound across runs (an answer per send kept asyncio from
  running other tasks): bounded, as above, with a test;
- every Host was in a cycle with itself through In() (freed only by the
  collector), and a host with a SimulatedClock through the clock: In()
  reaches the datamodel through a capsule cleared at free, the clock
  holds its hosts weakly; a test frees a dropped host with the collector
  off;
- deserialize's migration held a session row pointer across Python code:
  it reads the row by its handle again before writing;
- one large send_many kept an arena chunk of its size for the host's
  life: it converts in slices of 256;
- each event made a name `str` per session and a key per bind: names are
  the chart's shared `str`, the key is interned; the measurement reports
  the cost after one event too;
- a thin view whose fields were read could still answer after its ack:
  answer and fail check the batch first;
- a driver could start inside another call (a finalizer): refused.

Left for core (capacity planning, not the binding): a segment's region
grows by doubling under a burst of events and is kept (12.4 KB per actor
on quickjs after one event each at 10,000 actors).
- **Not in the API, by decision** (2026-10-15): export and adopt, which
  are the internal steps of a migration and left core's public API.

### Telemetry (8.5's first item, 2026-10-14) — done

`Host(..., telemetry=handler(host, signal, data))` gets each OTLP request
the encoder writes (`TRACES`, `LOGS`) as `bytes`, copied once;
`host.metrics()` pulls `ta_host_metrics`; the stop posts `trace.end` and
hands over the last take. Whom it serves: the operator and the
observability backend, through the application's exporter; the binding
does no I/O (bindings.md section 2: retries are the application's). Cost:
nothing without `telemetry` in the config (core's null encoder); with
it, one `bytes` per request (not per event) and one Python call. Tests in
`tests/test_telemetry.py` (spans decoded back with `trace_spans_decode`,
the last take at close, metrics in and outside a drain, a handler that
raises, an `async def` handler, watches); checked by hand against
otel-tui 0.7.5's OTLP/HTTP receiver: spans, metrics and logs shown (the
`<log>` lines with their macrostep's trace id), with the resource and
the scope's chart attributes.

Watches (`trace_watch`, `trace_unwatch`) are tested: with sampling at 0, a
watch on one chart traces it alone, ends at its deadline on the host's
clock (a simulated one, and the real one through the asyncio timer), and
`trace_unwatch` ends one early. Testing them found that the host's clock
started at 0: a watch posted before the first turn ended at once. The
binding now advances both clocks at creation and before spawn, deliver
and every typed method (bindings.md section 7: "before every run and
every input that depends on it").

A fresh agent reviewed telemetry; fixed (with tests): a handler's
`close()` during the last take crashed (a second free inside the first),
inputs from a handler there made close loop forever (now bounded and
counted as a call on the stack), `metrics()` there acknowledged the
close's batch, `async with` did not await an `async def` handler's last
take or raise its exception, a take the allocator refused stopped that
signal silently (now reported and retried next turn), and a close a
handler asked for kept its last take's exception until a later
`close()` (now raised by the driver). The README says the handler runs
in the turn, that only a turn's first exception is raised, and that a
collected `Host` skips the last take.

Application spans above actors are tested (`ApplicationTraces`) and
checked with the OpenTelemetry SDK beside the host in otel-tui: one trace
from the application's `POST /orders` through two actors' macrosteps and
microsteps to the application's `warehouse.reserve` (from
`send.traceparent`) and the macrostep its answer started. That found the
run's span records stretched over the handlers after the run: core
stamps them between two `ta_host_advance_time` calls, and the binding
advanced only before a run. With an encoder it advances after each run
too (one clock read per run; nothing without telemetry).

### Hibernation (8.5's second item, 2026-10-14) — done

`host.sweep(max_scan, limit)` (`ta_host_sweep`), `host.hibernate(session)
-> bytes` (`ta_host_hibernate`, the blob copied once) and
`host.wake(session, blob)` (`ta_host_wake`), with `wake=handler(host,
session)` for `TA_EFFECT_WAKE`. Whom it serves: the application
developer with more actors than memory, and the capacity planner, who
now has a measured cost per hibernated actor. Cost: nothing per event
for a resident actor; per hibernation one `bytes` (the blob) and, on the
`"python"` datamodel, one CBOR encoding of the session's variables (the
blob leaves the process, so this is the process boundary); per wake one
Python call and the decoding. API surface: three methods and a keyword.

**Decision changed:** "hibernation keeps the handle" (2026-10-13) left
each hibernated `"python"` session's instance on the Python heap, which
defeats a population of millions with a thousand resident. `serialize`
now writes the declared variables and `_event` as CBOR through the
value table and `destroy` removes the row; `deserialize` builds a new
instance. A variable outside the value table refuses the hibernation
(`TA_ERROR_UNSUPPORTED`); the session stays resident. AGENTS.md has the
rule.

**The gate:** `tools/stress.py` (`mise run stress`; `mise run test`
runs 20,000 actors with 100 resident on both datamodels, `--full` the
two million): two million actors of a counter chart, at most 1000
resident, the rest in SQLite, 200,000 events to actors drawn at random,
then a sample of 10,000 reports its count. Both datamodels pass: every
count right, at most 1000 resident after each eviction (`host_stats`),
85 B and 86 B of peak resident memory per actor, 60 B of core's region
per actor (numbers in the README). Getting there found two things:

- left to grow, a segment doubles every pool together and derives its
  instance memory from `maxActors`: 31 KB of region per actor at two
  million. The fix is configuration (`maxActors` for every actor,
  instance and string memory for the resident ones), which the README's
  capacity note gives; not a binding change;
- the CLOCK sweep skips an actor that ran since the last sweep, so with
  every event waking a different actor it could not get back to 1000
  resident (1354 at the peak). The application falls back to its own
  least recently used order (`tools/stress.py`, `evict`).

Tests in `tests/test_hibernation.py`: both datamodels wake with their
variables and their `_event` (read by an `<onexit>` at terminate),
events wait for the wake (one WAKE however many), a due delayed send
wakes its actor, the sweep's second pass, refusals (events waiting,
already hibernated, a `set` in a variable, a bad blob), a hibernated
`"python"` session holds no instance, a wake handler that raises, an
`async def` wake handler (and its `TypeError` under `with`).

A fresh agent reviewed hibernation; fixed (with tests):

- the binding guessed from the call depth whether an output it took
  (`hibernate`, `metrics`) had opened a batch of its own, wrong both
  ways: from a call inside a call outside a turn (a `send_many`
  iterator) it left that batch open and every later run was refused
  (`TA_ERROR_STATE`); between turns it acknowledged the batch a drain had
  taken at its bound and not dispatched, so its effects were lost. A
  flag now holds from a take to its ack, and the drain checks its bound
  before a take;
- a blob whose copy into `bytes` failed was acknowledged anyway: the
  actor had no blob anywhere. It is woken back from the view first;
- `sweep` and `wake` wrapped numbers out of range (`sweep(10, -1)` asked
  for 34 GB); they raise `OverflowError`, as `hibernate` did;
- `payload_serialize` answers `TA_ERROR_UNSUPPORTED` for a value outside
  the table, as `datamodel.h` asks (`to_wire` keeps `TA_ERROR_INVALID`),
  and a `KeyboardInterrupt` while writing a session is kept for the
  driver, not cleared;
- `tools/stress.py` checks the resident bound after every eviction (it
  sampled every 64 batches), treats only `TA_ERROR_STATE` as "not at rest
  now", and cannot loop when the sweep's cold actors refuse;
- the docs said a delayed send goes into the blob and a hibernated
  session holds no Python object: the payloads of its pending delayed
  sends and waiting events stay in the host. They also say that without
  a `wake` handler the actor stays hibernated, and that export and
  migration refuse what hibernation refuses.

Not changed: `deserialize` sets each variable twice (to `None`, then its
value), a small cost per wake; a `KeyboardInterrupt` during `wake`'s
`deserialize` is raised at the next turn rather than by `wake()`.

### Migration (8.5's third item, 2026-10-14) — done

`host.migrate(handler, /, *, chart=None, session=None, wake=False,
timeout=0) -> migration` (`ta_host_migrate`; the handler at `MIGRATION_DONE` with
`migrated`, `failed` and `pending` from `ta_host_migration_pending`), and
`migrated=handler(host, migration)` for `TA_HOST_EFFECT_MIGRATED`. The
wake handler gets the migration that asks (`wake(host, session, migration)`; `None`
for an event), which an application that keeps few actors resident
needs to tell the migration's WAKEs from an event's. Whom it serves: the
application developer who deploys a new version of a chart under load,
and the operator, who sees the migration's result. Cost: nothing per event; per
moved actor a thin view only with a `migrated` handler; per migration a dict
entry and, at its end, a list of the pending sessions.

The conform adapter runs `hibernate` and `migrate` steps: the suite's M4
scenarios `hibernate` and `migrate` pass on both datamodels (as does M3's
`invoke`; `snapshot` waits for snapshots). The gate stays M1 (the runner
selects by milestone, and M4 holds `snapshot`).

**The gate:** `tools/migration.py` (`mise run migration`; `mise run test`
runs 20,000 actors with 100 resident on both datamodels, `--full` two million
with 1000): version 1 of a counter chart, most actors hibernated in
SQLite, version 2 added, every actor moved while events go to random
actors; every sampled count and version right, the migration leaves none
behind, version 1's segment retired, at most R resident (plus a batch)
after every eviction. Getting there found:

- an application that wakes every actor the migration asks for at once has
  them all resident; one that queues the migration's WAKEs must wake them in
  the migration's order (the migration retries the actors it waits for from the front
  of its list, a quantum per run) and must not put back one the migration
  asked for before it moved (the migration asks once). The README's
  "Migration" gives the pattern;
- events for actors still waiting for the migration piled up in the host
  (the segment's event pool, doubling every pool: 281 MB of region at
  200,000 actors). An event's WAKE now wakes its actor at once (93 MB
  peak at 200,000); the migration on the WAKE tells it from the migration's;
- core's walk was quadratic in the hibernated actors, which sized the
  full gate at 500,000 (answered: "Requests to core"; the gate is two
  million now). The one-actor migrations an event's WAKE started went with it:
  core takes a woken actor into the migration itself.

Tests in `tests/test_migration.py`: both datamodels move with their
bindings (the upgrade's data) and keep a delayed send, a hibernated
actor asked for with its migration, an event's WAKE with none, the timeout
leaving a sleeping actor in `pending`, one session or one chart, a
`"python"` actor with a `set` failing and staying, the stale view, a
handler that raises, a migration unfinished at the stop, refused arguments.

### Snapshots (8.5's fourth item, 2026-10-14) — done

`host.snapshot() -> bytes` (`ta_host_snapshot_pack`) and
`Host.restore(snapshot, config, ...)` (`ta_host_restore`), both
datamodels' state beside the host's. Whom it serves: the application
developer who restarts or moves a host of the same build, and the
operator who inspects one. Cost: nothing per event; per snapshot the
region copied once into `bytes` and the datamodels' state written once.

**Asked of core and done (2026-10-14):** a binding's datamodel must
restore its rows at the handles the regions name, and `ta_table` could
not add a row at a given handle. `ta_table_snapshot` and
`ta_table_restore` (`tinyactors-core/src/base/table.c`) save and restore
a table's handle namespace (generations and free list); the rows are the
binding's to refill.

The conform adapter runs `snapshot` and `restore`: every scenario of M4
passes on both datamodels, and the gate is M4's protocol vectors and
scenarios (the W3C suite joined it with M2, below).

**The gate:** `tools/snapshot.py` (`mise run snapshot`; `mise run test`
runs 20,000 actors with 100 resident and 5,000 all resident on both
datamodels, `--full` two million with 1000 and 100,000 all resident): a
population on SQLite, events, a delayed send for every resident actor,
the snapshot to a file, the host closed, a new one restored (whose own
snapshot must be the same bytes), more events, the delayed sends due;
every sampled count and delayed send right, at most R resident. Numbers
in the README. Getting there found that a test's config with too few
events for its resident actors grows the region tenfold (doubling every
pool), and a snapshot copies the region as it is: size the pools.

A fresh agent reviewed snapshots; fixed (with tests):

- two segments running one image: a restored session found the first
  segment's chart (by image hash and index), so when that segment
  retired the other's sessions ran a detached chart, and on quickjs core
  crashed (its datamodel matched the same way). Core now gives a
  datamodel the segment of each attachment (`ta_datamodel_chart.segment`,
  from the system's first serial), and both datamodels name a session's
  chart by segment and index;
- the real clock resumed at the snapshot's time only on a machine up for
  less time: the offset is now the difference, whatever the clock;
- the snapshot is refused before the datamodels' state is built when the
  host is not quiescent (a reply pending counts), and quickjs's state is
  written once in the usual case, not twice;
- values are written one by one, so one nested 61 to 64 deep no longer
  refuses the snapshot, and a `MemoryError` is no longer reported as a
  value outside the table;
- a restore refuses rows that do not match the tables or come twice, and
  a config of another system;
- core's `ta_table_restore` read past a crafted blob on a 32-bit
  `size_t`; the API description's pattern for unnamed records is wider.

Left as is: a value written through the value table loses sharing; the
images of a pack are walked once per image (`ta_host_snapshot_image`),
quadratic in segments, of which a host has few.

Tests in `tests/test_snapshot.py`: both datamodels go on after a restore
with their variables, `_event`, a payload waiting in a mailbox and a
delayed send due on a new clock that starts at 0; a restored host's
snapshot is the same bytes; a hibernated actor wakes after a restore; a
`set` refuses the snapshot and the host goes on; a snapshot from a
handler is refused; a blob that is no snapshot is refused; an unaligned
buffer restores.

### Invoked services (8.5's fifth item, 2026-10-14) — done

`Host(..., services={type: handler})` (`TA_EFFECT_INVOKE`), the
`Invocation` record with `send_back` (`TA_EVENT_IN_FROM_INVOKE`) and
`done` (`TA_EVENT_IN_INVOKE_DONE`), and the service's `receive` (sends
with `IO_PROCESSOR` `TA_NIL`) and `stop` (`TA_EFFECT_UNINVOKE`). Whom it
serves: the application developer whose charts call the application
(an HTTP client, a database, a model) as SCXML services. Cost: nothing
per event that is not the service's; per invocation one record (its
type, src and invokeid interned) and a dict entry; per round trip a thin
view and one `ta_host_send`. No change to core: it had every part.

**The gate:** `tools/services.py` (`mise run services`; `mise run test`
runs 10,000 actors with 50,000 round trips on both datamodels, `--full`
200,000 with 1,000,000): every actor invokes a counter, pings it at
random, then a third finish with done data, a third are cancelled, the
rest keep theirs; every pong counted once, every done datum right, every
cancelled service stopped once and no other, the ended services
collected. Numbers in the README. Getting there found that quickjs keeps
each actor's last event in core's pool, so its region doubled every
pool (980 MB at 200,000) until `maxEvents` counted the actors; the
README says so.

Tests in `tests/test_services.py`: both datamodels talk to a service and
get its done data (with `_event.invokeid`); leaving the state, and
terminating the invoker, stop the service once; a service without
`receive` or `stop`; no handler for the type, and a handler that raises,
give `error.communication` and raise after the turn; services left at
the stop are dropped, not stopped; autoforward reaches the service (and
forwards its own answers back to it); `services` must be a dict. The
review added: a service done in its handler is neither kept nor
stopped; a send to a service that failed to start fails; a failure to
start an invocation cancelled in the same run is not reported; a
snapshot with a running service is refused.

### Snapshots with invoked services (2026-10-15) — done

Snapshots were refused while an invoked service ran, which an
application cannot tell in advance. Decided with the maintainer: a
snapshot is for a copy of the whole application (every chart version at
once, which only the host level has): rehearse a deploy, capture a host
for a developer, prepare a test world; and for taking a host out of the
process. On a copy the real services must not be reached, and a
rehearsal wants stand-ins, so a restore starts each running invocation
again through `Host.restore`'s `services=`: the real handlers go on,
stand-ins make a sandbox, no handler gives its invokers
`error.communication` (SCXML 5.10.3: errors "such as those arising from
<send> and <invoke>"; a crashing `receive` gives the same). The request
to core below was answered in core by this session at the maintainer's
request. The binding keeps no table of its own: core holds the facts.

### The W3C suite, M2 (2026-10-15) — done

The adapter offers `w3c.run` (tinyactors-test/docs/driver.md, "The W3C
environment"): the start chart booted on a host of its own, BasicHTTP as
a sync I/O processor whose handler writes the send as a POST, reads it
back and answers with it as the incoming request (`origin` the location,
`raw` the message), and a `SimulatedClock` moved to the next deadline
whenever the host is idle. **191 of 191 on quickjs, 171 of 171 on the
native datamodel** (the other 20 are `skip-python.txt`'s, each with a
reason); the gate is M4's protocol vectors, the W3C suite and the
scenarios on both datamodels.

What it took, each a core call exposed or a datamodel rule, with a test
in `tests/`:

- `Host.boot(segment)`: `ta_host_boot`, which the binding lacked.
- `Host.next_deadline`: `ta_run.next_deadline`, as seconds from now, so
  a `SimulatedClock` can move straight to it.
- `send.answer(..., origin=, raw=)`: `ta_event_in`'s fields, which an
  I/O processor that reads a transport's message passes on.
- **The chart declares the variables `<foreach>` and `<script>` bind**
  (W3C 150, 151, 302, 304, 525): a `<foreach>`'s `item` and `index`
  (SCXML 4.6: "a new variable" when there is none) and a script's
  assignments outside its defs, `None` until set, serialized with the
  `<data>` ids. Asked of core and done by this session: the compiler
  wrote a `<foreach>`'s item as any location, so the datamodel could not
  tell it from an `<assign>`'s (which must stay `error.execution` when
  undeclared, W3C 286); it is `TA_EXPRESSION_ITEM` now.
- `hasattr` among the builtins (W3C 330: whether `_event`'s fields are
  bound).

### The API review (2026-10-15) — applied

The review of the direct layer (the doc "tinyactors Python binding: API
review") asked seven questions; each recommendation is in:

- **Seconds at every boundary:** `migrate(timeout=)` (ticks rounded up),
  `SimulatedClock.increment(seconds)` and `.now` (a float) join
  `next_deadline`; ticks stay inside the extension.
- **`on_error` by keyword too:** a typed method takes it second or as
  `on_error=`, never as a param for core.
- **No `actor_send`:** a typed method mirrors a method of requests.md
  only where the language has no direct call (bindings.md, section 10);
  `host.send` is `actor.send`'s, and its suite cases are reference only.
- **Addresses only in `send`:** kept; an address is what arrives from
  another system.
- **The resolver is any callable** `resolver(base, source)`.
- **A reply needs a second drive:** kept; the README's first example
  says so.
- **XML in conditions:** no API change; the README shows conditions as
  methods of a `.py` file.

Then, to complete the surface against core's public headers:
`tinyactors.compile_system` (`ta_compile_system`, an image from a
`<system>` manifest whose actors `Host.boot` spawns) and
`Host.actor_by_name` (`ta_host_actor_by_name`). What stays out:
`ta_host_segment_count` (`host_stats` has `segments`) and
`ta_host_snapshot_segments` (`Host.restore` reads the images from the
pack).

### Packaging (2026-10-15) — built, not yet released

One abi3 wheel per native release target (root `docs/adr/0001`,
decisions 21 to 24): `mise run wheel` and `mise run release`, through
`mise/wheel.rb`. Checked here: all four wheels build from Linux, pass
`twine check --strict`, and are byte-identical when built again from the
same commit; this machine's (x86_64 Linux, glibc 2.39) installs into
fresh 3.13.16 and 3.14.8 environments, where the smoke test and all of
`tests/` pass against the installed wheel. The other Linux cases were
run without the containers (no Docker daemon in that session), with
python-build-standalone's 3.13.16 and `LD_BIND_NOW=1`: x86_64 on musl
1.2.4 (pip installs the wheel; the smoke test and all of `tests/`
pass), and aarch64 under qemu on glibc 2.39 and on musl (Alpine 3.20's
loader), where the smoke test passes and 158 of the 161 tests (the
other three start a child interpreter, which qemu without binfmt cannot
run). Not checked yet: the container stage itself, and the macOS
wheels on a Mac (their symbols only, decision 14). Reviewed
against PyPA's specifications and PyPI's rules (2026-10-15): the wheels
pass `twine check --strict` and `check-wheel-contents`, uv selects each
on its platform, and the Linux file needs no library and no versioned
symbol; the review's fixes are in (`--resume` for a stopped upload,
`LD_BIND_NOW` in the smoke tests, the dev-release wording, the dev
number's bound, classifiers). The first release needs its changelog
section (the release-changelog skill) and `PYPI_TOKEN` in fnox, and
comes before the first dev build.

## How the work runs

The root ROADMAP.md, step 8, "Rules for the work", sets these for every
session; here they are as they apply to this one.

- **The Python session owns `tinyactors-cpython/` and nothing else.** A
  Node session owns `tinyactors-napi/` (from step 8.5), in the same
  checkout; a coordinating session owns `tinyactors-core`,
  `tinyactors-test` and the root documents.
- **This file is the session's.** The session keeps it current and
  commits it with its own paths. It never edits the root `ROADMAP.md`:
  the coordinator reads this file, moves each request below into the
  root's "Requests to core", and marks the step done there after its
  audit and review.
- **Core and the suite are frozen.** A gap in either (AGENTS.md, "Gaps in
  core") is written under "Requests to core" below and committed alone
  (`docs(cpython): request to core: ...`). The coordinator makes the
  change, with C tests and suite cases, and says when to go on.
- **One checkout, on `main`, commits of this project's paths only:**
  `git commit -- tinyactors-cpython/` (`git add` a new file by its path
  first). Never `git add -A`, `git add .` at the root, `git commit -a`,
  `git stash`, `git checkout -- .` or `git reset --hard`: they take or
  undo the other sessions' work. History stays linear: `git pull
  --rebase` when `main` moved.
- **`.git/index.lock` belongs to another session's git command.** When
  git says the index is locked, wait a few seconds and run the same
  command again. Never delete the lock.
- **Tests: this project's only.** Run this project's `mise run test`
  before every commit, and the suite against the adapter (`mise run
  conform`). Never the root `mise run test` and never `tinyactors-core`'s
  build or tests: the coordinator runs those. `mise run build` here runs
  core's `amalgamate` task, the only core task this session runs.
- **An audit after the step**, by the coordinator: the binding against
  the guide's "never" list, its `AGENTS.md`'s "What not to build", and its
  mapping table. What it finds is fixed here.

## Requests to core

Gaps in core or in the suite this session finds. Each says what is
missing, why, and which suite case or test shows it; the coordinator moves
it into the root ROADMAP.md's "Requests to core" and answers there.

- **Open: one exhausted pool doubles every pool.** What: when any pool of a
  segment runs out, `segment_grow` doubles all of them, derives
  `instanceBytes` again from the doubled `maxActors`, and holds the old and
  the new region at once (`src/host/host.c`, `segment_grow`). Why: a
  `send_many` of 1000 events with `maxEvents` 256 took a region of
  `maxActors` 200,000 from 15 MB to 127 MB, and at 2,000,000 from 118 MB
  to 1243 MB; quickjs, which keeps each actor's last event, grew a 10,000
  actor host to 13.7 KB per actor (1.5 KB with `maxEvents` sized). The
  capacity planner cannot see it coming: `host.stats` reports
  `regionBytes`, not each pool's capacity and high-water mark. Ask: grow
  only the pool that ran out, keep an explicit `instanceBytes`, and report
  each pool's capacity and high-water mark in `host.stats`. Which test:
  the memory investigation's `grow_cliff.py` (2026-10-15).
- **Open: the trace journal is sized from `maxEvents`.** What: with
  tracing on (the default), `traceBytes` is 512 × `maxEvents` and
  `traceContexts` is `maxEvents` (`src/system/system.c`), 618 B per event
  slot. Why: sizing `maxEvents` for quickjs's actors (above) adds 618 B per
  actor to the region for traces, whatever the trace volume. Ask: a trace
  journal sized for the trace volume (a fixed default, or one the config
  states), not derived from the event pool.
- **Open: the region is zeroed whole at creation.** What: `ta_system_init`
  zeroes the whole region (`src/system/system.c`), so everything a config
  reserves is resident at once. Ask: take memory already zeroed (calloc,
  mmap) so unused pool space costs address space, not memory. Unverified
  saving: what a config reserves beyond use.
- **Open: the host's maps live outside the region.** What: the `moved` map
  (12 B per entry at load one half, kept for each migrated actor's life)
  and a migration's member map are host allocations `regionBytes` does not
  count: about 50 MB kept and 50 to 100 MB while a migration runs at 2,000,000
  actors (estimated from 400,000). Ask: count them in `host.stats`, or a
  dense per-segment array indexed by serial.
- **Open, a known limit: no snapshot while a migration runs.** What:
  `ta_host_snapshot` refuses with `TA_ERROR_STATE` while a migration is active
  (`src/host/host.c`, `ta_host_snapshot`), and a rollout that wakes a
  mostly hibernated population can run for hours, so a host cannot be
  captured during it. Why it is not technical: a migration is plain data in
  host memory (`migration_job`, `src/host/host_internal.h`: id, flags,
  deadline, a walk cursor into a segment's actors, the sessions it waits
  for, a retry list, counters), all valid after a restore, which puts
  every region back as it was; the snapshot just does not write it. The
  binding's side is ready: `migration_done` is a handler of the host
  (2026-10-15), so a restored host's migrations reach the one
  `Host.restore` is given. No test shows it yet; the snapshot gate
  snapshots before migrating.

Answered 2026-10-15, in core, by this session at the maintainer's
request:

- **A restored host lost its external invocations' services without a
  word.** What: core kept an external invocation's actor, slot and
  invoke id, not its type, src or params, so after `ta_host_restore`
  nobody could start the service again, and the invoker waited forever
  (the binding refused snapshots with a service running instead). Now the
  invocation's request keeps its type and src (two shared strings) and
  its params (one retained payload of the invoker's datamodel) while it
  runs, and the restored host's first `ta_host_run` announces each one
  again, before any actor runs: a `TA_EFFECT_INVOKE` flagged
  `TA_EFFECT_FLAG_RESTORED`, with its token, so the embedder starts its
  service again, a stand-in, or answers `TA_EVENT_IN_ERROR_COMMUNICATION`.
  At the first run, not at the restore (retaining the params needs the
  datamodels' state back) nor at a take (the binding takes a batch for
  `host.config` while it builds the host). Who pays: 8 B per request slot
  of the region (`ta_request` 32 to 40 B; `ta_system` version 4 refuses
  older snapshots), two string references and one payload per running
  external invocation, nothing per event. Test: `test_host_effects.c`
  `test_restore_announces_external_invocations`; the binding's
  `tests/test_services.py`.

Answered 2026-10-14, in core's host layer (`src/host/host.c`,
`src/host/session_map.c`), by this session at the maintainer's request:

- **A waking migration was quadratic in the hibernated actors**
  (`retry_push` looked through the whole retry list before adding, and
  every run compacted it): a migration now keeps the sessions it waits for in
  a session map (the moved sessions' open-addressing table, made one
  structure, reserved through the host's allocator before any insert),
  and only the actors in memory on a retry list tried round robin from a
  cursor. A run costs its quantum, however many the migration waits for; the
  migration gate went from 500,000 actors to two million.
- **A migration never ended while it waited for an actor it would not ask for
  again**: a hibernated member goes back on the migration's retry list when
  `ta_host_wake` brings it back (the migration's WAKE or an event's), the migration
  asks again when it finds one hibernated after a wake, and an exported
  member leaves the migration. Without `TA_HOST_MIGRATE_WAKE` an actor nobody
  wakes still holds the migration until its timeout, as the README says.
- **A WAKE no migration asked for had `migration` `TA_NIL`**, against `boundary.md`:
  effects now carry 0 for no migration.

What the design needs from core is step 8.2 of the root
roadmap. The first binding's two requests were answered on 2026-10-12
(the runner skips envelope-level vectors for a binding, `34c09cfa`; the
tick rate through `host.config`, `9bd33f03`); their text is in this
file's history before `6426816a`.
