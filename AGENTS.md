# AGENTS.md: tinyactors-cpython

The Python extension `tinyactors`: one C extension module, written against
the CPython C API, that drives `ta_host`. It is being rewritten from
scratch (`6426816a` removed the first one; [ROADMAP.md](ROADMAP.md) says
where the work is). This file is the rulebook the rewrite starts from: it
holds the binding's rules, true at every step. The repository root's
`AGENTS.md` applies too.

Read first, in this order:
[`tinyactors-core/docs/embedding.md`](../tinyactors-core/docs/embedding.md)
(how to use the core: the loop, lifetimes, memory, a complete loop in C),
[`tinyactors-core/docs/bindings.md`](../tinyactors-core/docs/bindings.md)
(the guide for a binding),
[`tinyactors-core/docs/boundary.md`](../tinyactors-core/docs/boundary.md)
(the contract of every host call), [ADR 0012](../docs/adr/0012-lifetimes-follow-the-loop.md),
and the public headers. Build from those, not from the removed binding or
the proposals' sketches.

## Whom it serves

First the **application developer writing Python**: chart logic in Python
(the native `"python"` datamodel), a `Host` driven from a script or from
asyncio, values that are Python objects. Then the **binding author**, held
to the guide by the conformance suite, and the **maintainers**, who run one
suite against every implementation. Every API item is a host input, a
typed method over a posted message, the handling of an effect, or a call
of the compiler (`docs/api-map.md`); what core does not have is not in
the API.

## Layers

Decided by the maintainer (2026-10-13; bindings.md section 1, "Two
layers"):

- **The direct layer is the C extension `tinyactors`.** It mirrors the C
  API more or less one to one: `compile_charts` is `ta_compile_charts`,
  `Host.chart` is `ta_host_chart`, `Host.spawn` is `ta_host_spawn` and
  takes the chart reference `chart` returns, `Host.add_image` is
  `ta_host_add_image` and returns the segment. Everything below ("The
  API") is this layer, and it is what step 8.4 builds.
- **A convenience layer, pure Python on top, is later work and not built
  now.** It calls only the direct layer and adds shapes, not behaviour:
  an `add_image` from a name→text mapping (compile, add, close in one
  call; the earlier `add_image(charts)` belongs here), spawning by chart
  name, a chart builder so people need not write XML (over
  `ta_xml_writer`, `compile.h:84-110`, once the direct layer exposes it),
  and **`DirectoryResolver(root)`** (decided 2026-10-13): a resolver
  written in Python that reads `source` as a file under `root`, relative
  to `base`, and refuses a path that leaves `root`. It does not bind
  core's file resolver: `ta_runtime_resolve`
  (`tinyactors-core/src/runtime/runtime_hosted.c:78`) accepts absolute
  paths and does not confine reads to a root. The direct layer has only
  the resolver ("The resolver").
  Nothing in 8.4 depends on it.

## Names

Our names carry the prefix `ta_python_*`: every function this binding
defines or declares, static helpers included, and its types and globals.
A platform's name appears only where the platform requires it: the module
init function `PyInit_tinyactors` (CPython's import finds it by that name,
PEP 489), a slot such as `Py_tp_traverse` filled with our
`ta_python_host_traverse`. When you write a name in prose, say whose it
is. The lint rule `functions-carry-the-prefix` checks functions. Python
names follow the root's acronym rule: constants in capitals, methods
snake_case (`session_id`).

## The API

Decided by the maintainer (2026-10-13); every item is in
[`docs/api-map.md`](docs/api-map.md) with the core behind it.

- **`tinyactors.compile_charts(sources: list[Source], resolver=None) ->
  Compiled`**: `ta_compile_charts`, with `resolver` as
  `ta_compile_options.resolve` (below, "The resolver"). A `Source` is a
  name and a text (`ta_source`, `compile.h:27-30`); the first is the
  default boot chart. A compilation with an error raises
  **`CompileError`**, a subclass of `TinyactorsError`, carrying every
  diagnostic (`ta_compile_diagnostics`: `file`, `line`, `column`,
  `message`, and the severity) and freeing the result before it raises.
- **`tinyactors.compile_system(manifest: Source, resolver=None) ->
  Compiled`**: `ta_compile_system` (2026-10-15), the image of a
  `<system>` manifest: its `<chart name src>`s, read through the
  resolver, and the `<actor name chart>`s `host.boot` spawns. The same
  path as `compile_charts` (`ta_python_compile` takes a
  `ta_python_compilation`), the same `Compiled` and `CompileError`.
- **`Compiled`** holds the compile result: **`compiled.image`** is a thin
  view of `ta_compile_image`'s bytes (a read-only buffer, valid until
  `close`; a read after it raises `TinyactorsError` with
  `TA_ERROR_STALE`), **`compiled.diagnostics`** the warnings and notes
  of a compilation that succeeded, converted once. It is a context
  manager, `with tinyactors.compile_charts(...) as compiled:`, whose exit
  and **`close()`** call `ta_compile_free`; the collector calls it as a
  fallback.
- **`tinyactors.Host(config, io_processors={type: handler}, log=handler,
  clock=None, telemetry=None, wake=None, migrated=None,
  services=None, migration_done=None)`.**
  Everything is per host: the config (a `dict`, written as CBOR into the
  call's arena for `ta_host_create`), the handlers and the clock. A host
  has no resolver ("The resolver"). No global configuration (no
  `logging.basicConfig`, no
  module-level handler, no default logger). The extension reads
  `host.config` once at creation for `system.ticksPerSecond` (bindings.md
  section 3).
- **`with Host(...) as host`** (the synchronous driver) and **`async with
  Host(...) as host`** (asyncio) on the same class. Leaving either frees
  the host (`trace.end`, the last take of traces, `ta_host_free`); pending
  response handlers are dropped, never called.
- **`host.add_image(image) -> int`**: `ta_host_add_image`, which copies
  the bytes during the call (any buffer: `compiled.image`, `bytes` read
  from a `.tai` file); returns the segment, as C does. The compile
  result can be closed as soon as it returns.
- **`host.chart(name) -> ChartRef`**: `ta_host_chart`, the current
  version of chart `name` (`host.h:121-126`); the reference holds the
  segment and the chart index.
- **`host.spawn(ref, name=None, params=None, traceparent=None) -> int`**
  (`ta_host_spawn`, `ta_spawn`), **`host.send(session, event, data=None,
  *, traceparent=None)`** (`ta_host_send`), **`host.send_many(items) ->
  int`** (`ta_host_send_many`: each item carries the arguments of `send`,
  `traceparent` included; it returns the length of the accepted prefix,
  `host.h:192-195`), **`host.terminate(session)`**
  (`ta_host_terminate`), **`host.boot(segment)`** (`ta_host_boot`, the
  image's boot actors), **`host.actor_by_name(name) -> int | None`**
  (`ta_host_actor_by_name`, 2026-10-15: the live actor spawned with
  `name`, newest segment first; `None` for core's 0): applied at the call, read during it; a payload
  moves to core on success and is released on refusal. Under asyncio an
  input made outside a turn schedules one turn.
- **`traceparent` in: W3C text, parsed by core.** `ta_spawn.traceparent`
  and `ta_event_in.traceparent` are `ta_string_view`s of W3C text
  (`tinyactors-core/include/tinyactors/system.h:125-132`, `:171-173`),
  which core parses itself (`ta_traceparent_parse`, `trace.h:79`): the
  extension passes the `str`'s own UTF-8 for the call ("The binding's
  memory"), no conversion. Text that does not parse starts a new trace,
  as `None` does (`tinyactors-core/src/system/tracing.c:13-22`). An HTTP handler
  continues its caller's trace:

  ```python
  def on_request(request):
      host.send(session, "http.request", request.json(),
                traceparent=request.headers.get("traceparent"))
  ```
- **The resolver** is any callable **`resolver(base, source) -> str`**
  (a function, a lambda, an object with `__call__`; 2026-10-15, the
  API review), given to `compile_charts` only:
  **it lives only at compile time** (boundary.md section 12).
  It is the compile-time callback
  (`ta_compile_options.resolve`) for every static `src`, and an invoked
  chart it fetches is compiled into the same image. **A resolver refuses
  by raising `tinyactors.ResolveError(message, line=None,
  column=None)`**; a `SyntaxError` it raises, or the rewrite's own
  refusal of a `.py` file, is converted with its `lineno` and `offset`.
  The extension fills `ta_resolve_error` from it, the message's UTF-8
  copied into the compilation's scratch arena (`request->scratch`), and
  the compiler reports it as a diagnostic of the resolved file, at that
  line and column. Any other exception a resolver raises (an `OSError`, a
  bug) is raised from `compile_charts` after the result is freed (8.4;
  ROADMAP.md, "Decided in 8.4").
- **`TA_EFFECT_REQUEST` is a fetch at run time, answered in C, never by
  Python.** It is how a chart fetches what is not in its image (an
  `<invoke srcexpr>`, or a `src` the compiler did not match), and it stays (decided 2026-10-13; ADR 0012, decision
  8; boundary.md section 12). Core has already
  normalized the name and looked in the requesting segment's image
  (`tinyactors-core/src/system/route.c:439-497`), so until run-time
  fetching is built the extension answers every REQUEST with
  `ta_host_resume` and `TA_ERROR_INVALID`: the parent gets
  `error.execution` (`route.c:544-550`). It looks nothing up. How a binding
  will fetch is decided with the build-out (root `ROADMAP.md`, "Decided
  later"); not now. It is not the control plane
  (`ta_host_post`, `REPLY`, `NOTIFICATION`).
- **`.py` sources through `src`.** A chart writes `<script
  src="reader.py"/>`: a Python file, free of XML's whitespace and
  escaping, whose top-level `def`s become the chart's methods. The
  resolver runs the same restrictive rewrite on a `.py` file as it
  resolves it ("The "python" datamodel"), so a refused construct or an
  unknown name is a `CompileError` at its line in `reader.py`, at compile
  time.
- **I/O processors: `handler(host, send)`**, called once per
  `TA_EFFECT_SEND` whose `IO_PROCESSOR` is that type's index in the
  config's `ioProcessors`. Actor output to the application goes through
  them, and nothing else (ADR 0012, decision 6).
  - `send.traceparent`: the sending step's trace as W3C text, or `None`
    when the send is untraced. The effect holds the binary
    `ta_trace_context` (`tinyactors-core/include/tinyactors/trace.h:39-44`,
    `host.h:284`); `TA_EFFECT_FIELD_TRACEPARENT` formats it as text
    (`host.h:330`, empty when untraced), so the view formats it on first
    read, like every other field.
  - `send.answer(event, data=None, *, traceparent=send.traceparent,
    origin=None, raw=None)`:
    `ta_host_send` to the sending session (`SESSION`) with the send's
    `sendid`, `origintype` the processor's type, and the send's trace
    (decided 2026-10-13: an answer keeps the trace); passing
    `traceparent=None` starts a new trace. `origin` and `raw` are
    `ta_event_in`'s (2026-10-15, W3C's BasicHTTP: the location to reply
    to and the message as read).
  - `send.fail()`: `ta_host_send` with `TA_EVENT_IN_ERROR_COMMUNICATION`,
    the `sendid` and `send.traceparent`
    (`tinyactors-core/include/tinyactors/system.h:155-157`: core places
    `error.communication` in the sender's queue); the event name is not
    repeated.
  - An I/O processor that makes a request of its own carries the trace
    on:

    ```python
    def http_processor(host, send):
        headers = {"traceparent": send.traceparent} if send.traceparent else {}
        response = client.post(send.target, json=send.data, headers=headers)
        send.answer("http.response", response.json())   # keeps send.traceparent
    ```

    Not a proposal, a note: `answer()` formats the context as text and
    core parses it back. A binary context field on `ta_event_in` would
    avoid the round trip; nothing needs it now.
  - A plain function gets a **thin view** (below, "Thin views"); an
    **`async def`** handler is scheduled as a task and gets a
    **materialized record**, made before it is scheduled. A processor
    configured `sync` stops the run until its send is answered: its
    sender waits until then (`host.h:215-228`), so an `async def`
    handler may answer it later.
  - **An `async def` handler under the synchronous driver**, which runs
    no event loop: entering the `Host` with `with` raises `TypeError`
    naming the handler. The check is made at registration
    (`inspect.iscoroutinefunction`), and `async with` accepts it.
  - **A handler that raises before answering** (a plain function, or an
    `async def` task): the binding calls `send.fail()` for it, as ADR
    0009 has a runtime that cannot do what an effect asks answer with a
    failed completion, so the sender gets `error.communication`
    (`system.h:155-157`); the exception is raised from the driver after
    the turn, as any handler's is ("The loop").
- **`telemetry=handler(host, signal, data)`** is the application's
  exporter (bindings.md section 4, "What reaches the application"): at a
  `TA_HOST_EFFECT_TRACES` or `_LOGS` the extension takes the request
  (`ta_host_traces`, `ta_host_logs`), copies the view once into `bytes`
  (boundary.md section 7) and calls the handler with `"traces"` or
  `"logs"`; without a handler the request is taken and dropped, so the
  encoder goes on. An `async def` handler runs as a task (and, like an
  `async def` processor's, is refused under `with`); a handler's
  exception is raised after the turn and its request is lost (retrying is
  the application's I/O). **`host.metrics() -> bytes`** pulls
  `ta_host_metrics` the same way: outside a drain its batch of one output
  is acknowledged at once, from a handler it joins the open batch. At the
  stop, `trace.end` and the last takes go to the handler; an exception
  there is raised by `close()` once the host is freed, or by the driver
  whose call ran a close a handler asked for (2026-10-14). The last take
  counts as a call on the stack (a handler's `close()` is deferred, its
  `metrics()` joins the batch) and is bounded (64 batches of at most 64
  takes per signal), since a handler may make inputs there; `async with`
  awaits the tasks of an `async def` handler's last take. A take the
  allocator refuses is reported and retried at the next turn: core gives
  no second effect until a take.
- **Hibernation with the application's store** (bindings.md section 4,
  2026-10-14): **`host.sweep(max_scan, limit) -> list[int]`** is
  `ta_host_sweep` (the CLOCK sweep: sessions that did not run since the
  previous sweep and can be hibernated now); **`host.hibernate(session)
  -> bytes`** is `ta_host_hibernate`, its blob copied once (outside a
  drain the batch of that output is acknowledged at once, from a handler
  it joins the open one, as `metrics()`), refused with `TA_ERROR_STATE`
  when the actor is not at a stable point; **`wake=handler(host,
  session, migration)`** gets each `TA_EFFECT_WAKE` (an event or a due
  delayed send for a hibernated session, one effect per hibernation,
  `migration` `None`; or a migration asking for it, `migration` its id),
  and the handler
  calls **`host.wake(session, blob)`** (`ta_host_wake`), now or later (an
  `async def` handler runs as a task; refused under `with`). Without a
  `wake` handler the effect is dropped: the session stays hibernated and
  its events wait until the application calls `wake`. An output taken
  with no batch open opens a batch of its own, which the binding
  acknowledges at once; with one open (a handler's call) it joins it.
  The binding tracks this with a flag set from a take to its ack, never
  by guessing from the call depth; and a drain stops at its bound before
  a take, so no batch stays open between turns. A blob the copy into
  `bytes` fails on is woken back from the view before the ack. Where the
  blobs live, when to sweep and how many stay resident are the
  application's; `tools/stress.py` is the pattern, with SQLite.
- **Migration** (bindings.md section 4, 2026-10-14; the handler made the
  host's, and "migration" renamed migration, 2026-10-15): **`host.migrate(*,
  chart=None, session=None, wake=False, timeout=0) -> migration`** is
  `ta_host_migrate` (every chart, a chart's name, or one session;
  `TA_HOST_MIGRATE_WAKE`; a timeout in seconds, converted to host ticks
  rounded up and counted from the clock advanced at the call) and returns core's migration id; the migration
  moves actors in later runs, so the call schedules a turn. Its
  `MIGRATION_DONE` goes to the host's **`migration_done=handler(host,
  result)`** with `{"migration", "migrated", "failed", "pending"}`, the
  pending sessions read with `ta_host_migration_pending` at the dispatch
  (valid until the next run) into a `bytes` the list is made from;
  without the handler nothing is read. A handler of the host, not of the
  call: nothing waits in a table of ours, and a restored host's
  migrations reach the handler `Host.restore` was given.
  **`migrated=handler(host, migrated)`** gets each
  `TA_HOST_EFFECT_MIGRATED` as a thin `Migrated` view (`session`,
  `segment`, `chart`, `migration`); without it nothing is read.
  `SEGMENT_FREED` is dropped: the host keeps the images. How to migrate a
  mostly hibernated population with a bound on what is resident is the
  application's (README, "Migration"; `tools/migration.py`).
- **Snapshots** (2026-10-14): **`host.snapshot() -> bytes`** is
  `ta_host_snapshot_pack` with one datamodel state, labelled
  `tinyactors-cpython`: a CBOR map of the `"python"` datamodel's state
  (below, "The "python" datamodel") and quickjs's
  (`ta_datamodel_ecmascript_snapshot`), copied once into `bytes` (no
  arena: a snapshot's size is no call's scratch). **`Host.restore(snapshot,
  config, io_processors=None, **handlers)`** is a class method that runs
  `Host(...)`'s creation with `ta_host_snapshot_unpack`,
  `ta_host_snapshot_image` and `ta_host_restore` in place of
  `ta_host_create`, then restores both datamodels before anything runs;
  a pack another program labelled is refused, an unaligned buffer copied.
  The restored host's clock reads its ticks plus an offset (mod 2^64)
  that puts it at the snapshot's time when it is restored (bindings.md
  section 7), whatever the clock reads. The effective config's system id
  must be the pack's. Quiescent only, which core checks first, before the
  datamodels' state is built: from a handler (a batch is open), with an
  effect or a reply pending, or during a migration, `TA_ERROR_STATE`.
  The services the application runs are not in it: the restored host's
  first run announces each external invocation again (below, "Invoked
  services").
  The ECMAScript state is written into room for the last one's length,
  so it is written once in the usual case.
- **Invoked services** (bindings.md section 4, 2026-10-14):
  **`services={type: handler(host, invocation)}`**. At a
  `TA_EFFECT_INVOKE` the handler of its type (`EVENT`) gets a record,
  `tinyactors.Invocation` (made before the ack: a service outlives it),
  of `session`, `invokeid` (`SENDID`), `type`, `src` (`TARGET`), `data`
  (the payload's value), `traceparent`; the type, src and invokeid are
  interned, shared by a population. What the handler returns is the
  service, kept in a dict on the host keyed by core's (`SEGMENT`,
  `TOKEN`), as bindings.md says: no table of invoke ids.
  **`invocation.send_back(event, data=None)`** is `ta_host_send` with
  `TA_EVENT_IN_FROM_INVOKE` and the token, **`invocation.done(data=None)`**
  with `TA_EVENT_IN_INVOKE_DONE` (the service is dropped then); both
  refuse with `TA_ERROR_STALE` once it ended. A `TA_EFFECT_SEND` with
  `IO_PROCESSOR` `TA_NIL` goes to the service's **`receive(host, send)`**
  (a thin `Send` view, `type` `None`), a `TA_EFFECT_UNINVOKE` to its
  **`stop(host)`** (then it is dropped); both optional. No handler, or a
  handler that raises: `ta_host_send` with
  `TA_EVENT_IN_ERROR_COMMUNICATION` to the invoker, and the exception is
  raised after the turn; none when an UNINVOKE of the same token follows
  in the open batch (cancelled in the run that started it). A failed
  start stays as an entry marked failed, so each send to it gets
  `ERROR_COMMUNICATION` with its sendid, until the UNINVOKE (no `stop`).
  A handler that calls `done` leaves nothing kept. A `receive` that
  raises is a crash: the invoker gets `ERROR_COMMUNICATION` with the
  send's sendid, the entry stays as failed without the service (dropped,
  not stopped). **`invocation.restored`** is `TA_EFFECT_FLAG_RESTORED`:
  a restored host's first run announces each invocation that was
  running (core keeps its type, src and params), the handler
  `Host.restore`'s `services=` names starts it again (or a stand-in), and
  a type with no handler gives `ERROR_COMMUNICATION` without raising.
  At the stop the services are dropped, not
  stopped, as pending handlers are. `tools/services.py` is the pattern.
- **Other systems** (ADR 0014, 2026-10-15): the config's I/O processor
  marked `"remote"` is an ordinary I/O processor handler that gets every
  send to another system's address (core decides local or not); its
  `send.origin` is `TA_EFFECT_FIELD_ORIGIN`, the sender's address. The
  other side calls **`host.send(target, event, data, origin=...)`**, where
  `target` may be a local address (`str`) that core resolves
  (`ta_host_resolve`) before the payload is made in the target session's
  datamodel. Success needs no answer; `send.fail()` gives the sender
  `error.communication`. No peers, envelopes or routes: the transport and
  the wire encoding are the application's.
- **`log=handler(host, entry)`** gets this host's `<log>` lines
  (`TA_EFFECT_LOG`) as thin views of `(session, label, text)`, no chart
  (decided 2026-10-13). Without a handler, the lines are discarded.
- **Typed methods over post**: `actor_state`, `actor_context`,
  `actor_list`, `actor_get`, `host_stats`, `host_config` and `trace_*`
  (one per method of `tinyactors-core/docs/requests.md` that Python has
  no direct call for: `actor.send` is `host.send`, so it has none;
  2026-10-15, the API review), each
  `(handler, on_error=None, /, **params)`, returning `None`, with
  `on_error` also taken by keyword (never passed to core as a param): the
  method's other params are keywords, passed to core as given, so core checks them and
  answers in the reply (8.4; ROADMAP.md, "Decided in 8.4"). The handler `(host, result)` runs when the method's
  `REPLY` is dispatched, in a later drain; `on_error(host, error)` gets a
  `TinyactorsError(code, reason, path, message)`; an error with no
  `on_error` is raised by the driver after the turn. A refusal at the
  call (`ta_host_post`: `TA_ERROR_NO_MEMORY`, `TA_ERROR_STATE`) raises at
  once. No synchronous request, no raw `post`, no reply object, no future.
- **The message id is the handle of a row in the pending table**
  (`ta_table`) holding `(handler, on_error)`: the drain reads the
  `REPLY`'s `id` in C, finds the row with `ta_table_get` (O(1); the
  generation refuses a stale id), removes it and calls the handler. At
  `ta_host_free` the remaining rows are dropped and their handlers never
  called.
- **`DONE` and `ERROR` are internal** (the rows of the session are
  released); no notification is
  defined (`actor.subscribe` and `actor.changed` are removed, ADR 0012
  decision 4), and the extension drops a
  `TA_HOST_EFFECT_NOTIFICATION` whose method it does not know.

## The loop

The extension runs embedding.md's loop (section 2) in C: the guide's two
functions (bindings.md section 3) with our names. The comments carry the
rules:

```c
static void ta_python_drain(ta_python_host *host) {         /* one batch at a time */
    const ta_host_effect *e; uint32_t n;
    while ((n = ta_host_effects(host->h, &e)) > 0) {
        for (uint32_t i = 0; i < n; i++) ta_python_dispatch(host, i);
                                                            /* handlers may call the host:
                                                               inputs apply now, effects come next take */
        ta_host_effects_ack(host->h);                       /* everything handed out for it ends */
        host->acks++;                                       /* this batch's thin views are stale from
                                                               here: core keeps no batch counter */
    }
}

/* one turn: returns what the driver does next */
static ta_python_next ta_python_turn(ta_python_host *host) {
    ta_python_drain(host);                                  /* what inputs and posts appended:
                                                               a pending reply needs no run */
    ta_host_advance_time(host->h, ta_python_now(host));     /* ta_host_time: ticks at host.config's
                                                               rate, wall ns */
    for (;;) {
        ta_run run = ta_host_run(host->h, TA_PYTHON_BUDGET); /* bounded: fuel per call */
        ta_python_drain(host);
        switch (run.status) {
        case TA_RUN_EFFECTS:                                /* a sync I/O processor's send: answered in
                                                               the drain, or its sender waits */
        case TA_RUN_EFFECTS_FULL: continue;                 /* run again now */
        case TA_RUN_YIELDED:      return TA_PYTHON_NEXT_YIELD; /* budget spent, actors still runnable */
        case TA_RUN_IDLE:         host->deadline = run.next_deadline; /* UINT64_MAX: none */
                                  return TA_PYTHON_NEXT_IDLE;
        case TA_RUN_EXHAUSTED:    host->pool = run.detail;  /* a TA_POOL_* */
                                  return TA_PYTHON_NEXT_EXHAUSTED;
        case TA_RUN_FAULT:        host->fault = (ta_status)run.detail; /* negative: ta_status_name */
                                  return TA_PYTHON_NEXT_FAULT;
        default:                  host->fault = TA_ERROR_UNSUPPORTED;  /* a status unknown here */
                                  return TA_PYTHON_NEXT_FAULT;
        }
    }
}
```

`ta_run` and the statuses are `tinyactors-core/include/tinyactors/system.h:196-218`,
`ta_host_time` is `host.h:205-208`. `ta_host_run` refuses while a batch
is open (`TA_RUN_FAULT`, detail `TA_ERROR_STATE`), which the drain before each run rules out. There is no nested
drain: Python code a handler runs may call the `Host` to add inputs, post
or read views; it never takes or acks.

| `ta_python_turn` returns | Synchronous driver (`run_until_idle()`, `run()`, `SimulatedClock.increment(seconds)`) | asyncio (`await host.idle()`) |
|---|---|---|
| `TA_PYTHON_NEXT_YIELD` | check signals (`PyErr_CheckSignals`; a `KeyboardInterrupt` is raised between turns), then turn again | yield to the loop: `loop.call_soon(turn)` |
| `TA_PYTHON_NEXT_IDLE` | `run_until_idle()` returns; `run()` sleeps to `deadline` (converted with `system.ticksPerSecond`) and turns again; `increment(seconds)` advances the simulated clock (`ta_host_advance_time`, the simulated time as the wall clock) and turns until idle, arming nothing | one `loop.call_at` timer at `deadline` (none at `UINT64_MAX`), replacing the last; `idle()` completes; an input from outside a turn schedules one turn with `loop.call_soon` |
| `TA_PYTHON_NEXT_EXHAUSTED` | raise `MemoryError` naming the pool; the host stays usable and the next turn goes on where the run stopped | the same, from the turn |
| `TA_PYTHON_NEXT_FAULT` | raise `TinyactorsError` with `ta_status_name(fault)` (the detail); not retried | the same |
| a handler raised, or a reply's error had no `on_error` | finish the turn (the ack comes anyway), then raise the first | the same |

- **Time in the API is seconds** (an `int` or a `float`), converted
  with the config's `ticksPerSecond`: `next_deadline`, `migrate`'s
  `timeout`, `SimulatedClock.increment` and `.now` (2026-10-15, the API
  review). Ticks stay inside the extension.
- **Both clocks before every run**, and at creation and before an input
  that depends on them (spawn, deliver, every typed method: a
  `trace.watch` ends at a time of the host's clock), and with an encoder
  after every run too (the run's span records are stamped between the
  clocks around it; else they stretch over the handlers of the drain
  after it, to the next advance), one timer (the sync driver's sleep or
  one asyncio `TimerHandle`) at `next_deadline`. A host created with a
  `SimulatedClock` arms no timer.

## Lifetimes and thin views

ADR 0012: **everything the host hands out about a batch lives until the
ack** (effect fields, `TRACEPARENT`, traces, logs, metrics,
hibernation blobs), across any later call into the host. A
datamodel's outputs live until its next call. Snapshots and the refused
config's error fill a `ta_buffer` the extension lends.

**Thin views.** Wherever C values are exposed to Python, the extension
hands out a thin view where it can, not an eager conversion, as the
datamodel does with `_event.data` (the row's value, not a copy). A view
is a small object holding the `Host`, the ack count when it was made, and
the effect's index; it converts a field when Python reads it, with the
accessors (`ta_host_effect_number`, `_text`), and caches it. A handler
pays nothing for a field it does not read.

- **Staleness:** the `Host` counts acks (`host->acks`, bumped after
  `ta_host_effects_ack` in `ta_python_drain`). Reading a view's field
  when the count differs, or after the host was freed, raises
  `TinyactorsError` with status `TA_ERROR_STALE` (`ta_status_name`, the
  status core gives a stale handle, `types.h:108`), and so do a stale
  send view's `answer()` and `fail()`. Core has no batch counter and needs
  none: only the extension acks. What a handler already read out of a
  view is a Python object and outlives the ack.
- **`send.data`:** for a payload of the `"python"` datamodel, the row's
  value itself (the payload is retained by core until the ack,
  `host.c:1187-1191`, `:1044-1048`): no conversion. Capture copied it for
  this send, so no actor holds it any more. For a quickjs payload,
  converted on first read: `ta_datamodel_ecmascript_payload_to_cbor`
  (a view valid until quickjs's next call) read at once with
  `ta_cbor_reader` into Python objects.
- **Materialized, and when:** an `async def` processor's body runs after
  the ack (the task starts when the loop next runs it, after the drain
  returns), so before scheduling it the extension builds a record:
  `session` (`int`), `sendid` (`str`), the processor's type (the `str`
  the extension holds from the config), `event`, `target`, `data`,
  `traceparent` (`str` or `None`). Its `answer()` and `fail()` work any time while the host lives
  (`ta_host_send`'s refusal, a session that ended, raises). Not chosen:
  materializing every live view at the ack (a list of views per batch,
  and conversions of fields nobody may read); copying `session` and
  `sendid` into every view (a `str` per send, read or not). A plain
  function that wants to answer later reads the fields it needs and is
  written as `async def`, or answers before it returns.
- **Converted once, not viewed:** a reply's `result` is decoded with
  `ta_cbor_reader` when its `REPLY` is dispatched, and the handler gets
  Python objects. The trace: its `WIRE` lives until the ack; its one
  reader is the handler, which reads it whole and often keeps it
  (`host_stats` stored for a dashboard); a view of a nested CBOR map
  would need random access into bytes that end at the ack. The `id` is
  read in C only.

| Object Python sees | Owner | Created | Converts | Valid until | Thread |
|---|---|---|---|---|---|
| `Host` | the application | `Host(...)` | the config, into the arena, for `ta_host_create` | `with` exit, `close`, or the collector (a fallback) | the creating one; others refused |
| `send` (thin view) | the handler, borrowed | dispatch of a `TA_EFFECT_SEND` to a plain function | `session`, `sendid`, `event`, `target`, `data`, `traceparent` on first read | the ack; then stale | the host's |
| send record | the task | dispatch, before scheduling an `async def` processor | every field, at once | the collector | the host's |
| log `entry` (thin view) | the handler, borrowed | dispatch of a `TA_EFFECT_LOG` | `session`, `label` (`AUXILIARY`), `text` (`EVENT`) on first read | the ack; then stale | the host's |
| `Compiled` | the application | `compile_charts` | — | `with` exit, `close()` (`ta_compile_free`), or the collector (a fallback) | the creating one |
| `compiled.image` (thin view) | the `Compiled` | first read | none: a read-only buffer over `ta_compile_image`'s bytes | `close()`; then stale | the creating one |
| `compiled.diagnostics`, a `CompileError`'s diagnostics | the application | `compile_charts` | every diagnostic, once (their strings live in the result's arena until `ta_compile_free`) | the collector | — |
| a reply's `result` | the handler | dispatch of a `TA_HOST_EFFECT_REPLY` | the whole result, once | the collector | the host's |
| `TinyactorsError` from a reply | `on_error`, or the driver | the same dispatch | `code`, `reason`, `path`, `message` | the collector | the host's |
| a session id | the application | `spawn` | — (`int`, 64 bits) | — | — |
| `_event` (in a chart) | the session's instance | `bind_event` | the strings during the call (`datamodel.h:35-37`); `data` is the row's value, the Event holds a reference on the row | the next `bind_event`, `detach_event` or `destroy` | the host's |
| system variables (in a chart) | the session's instance | `create` | once, during the call (`datamodel.h:62-63`); immutable, shared by reference | the session's end | the host's |

## The host's allocator

The host allocates through the `ta_allocator` its environment gives it
(`tinyactors-core/include/tinyactors/allocator.h`), and this binding gives
it `PyMem_RawMalloc` and `PyMem_RawFree`, which are in the limited API
from 3.13 (CPython's `Include/pymem.h`, 3.13: lines 90-98), the wheel's
floor. The raw domain needs no GIL ("Memory allocator which doesn't
require the GIL to be held", the same file, line 91), which a
free-threaded build has none of anyway. Python's own objects stay outside
the host's allocator; the README says so (bindings.md, section 8). The
two functions take core's parameter lists (`void *context, size_t size,
size_t align` and `void *context, void *ptr, size_t size`), which the
lint exempts by that signature. `malloc`, `calloc`, `realloc` and `free`
stay refused by the layer lint.

**Alignment: `align` is at most 8, and `PyMem_RawMalloc` gives 16.** The
host passes 8 on every allocation it makes
(`tinyactors-core/src/host/host.c`, `host_alloc`), the largest alignment
a region object needs is 8 (`src/base/config.h`, `TA_ALIGN_MAX`), and the
allocator's contract says "a power of two, at most 8 from this library"
(`include/tinyactors/allocator.h:9`). A spike on CPython 3.13.16 (macOS
arm64) called `PyMem_RawMalloc` 800 times, sizes 1 to 100000, under
`PYTHONMALLOC` default, `debug`, `malloc` and `pymalloc`: the smallest
alignment seen was 16 in every mode. So the binding's `alloc` need not
align by hand (an `assert(align <= 8)` states the contract). A raw
allocator an application installs with `PyMem_SetAllocator` is not
covered by the spike (unverified).

## The binding's memory

The extension allocates almost nothing (bindings.md, section 6), and
what it allocates comes from the host's allocator through core's base
APIs:

- **One `ta_growable_arena` per host** (`tinyactors/arena.h`, ADR 0012
  decision 5), created with the `Host` and freed with
  it; its chunks are kept. Each API call takes a mark at entry
  (`ta_growable_arena_position`) and releases to it at exit
  (`ta_growable_arena_release`), on every path, exceptions included. A
  call made while another is running on the same thread (a `__iter__`,
  a getter, a `__del__` that calls the `Host`) allocates above the outer
  mark and releases to its own: nothing moves and the outer call's
  memory is untouched. It holds the call's CBOR (the config, a typed
  method's params, a payload for quickjs, `tinyactors.encode`), written
  by a `ta_cbor_writer` whose allocator is the arena's `ta_allocator`
  view; converted text; and the `ta_event_in` structs of a `send_many`.
- **Strings go to core as the `str`'s own UTF-8**, a view for the call
  (`PyUnicode_AsUTF8AndSize`; CPython keeps the UTF-8 in the `str` object
  once made, so a non-ASCII `str` pays that once), bytes through the
  buffer protocol. A `traceparent` is such a view too: core parses the
  W3C text itself ("The API").
- **The value table accepts exact built-in types** (`PyDict_CheckExact`,
  `PyList_CheckExact`, ...), so a conversion runs no user code: a `dict`
  or `list` subclass is refused (decided 2026-10-13).
- **Tables** (`ta_table`, `tinyactors/table.h`), grown through the host's
  allocator, freed with the host; keep handles, never row pointers
  across a `ta_table_add`:

  | Table | Row | Added | Removed |
  |---|---|---|---|
  | charts | the chart's class, its compiled functions, its slot names | `chart_attach` | `chart_detach` |
  | sessions | `{instance, chart, extra}`: the instance, its chart's row, the params until the first event or the old bindings after a `deserialize` | `create`, `deserialize` | `destroy` (after `serialize` too: a hibernated session is its blob) |
  | payloads | `{value, refcount}` | `capture`, `from_wire`, an input (`send`, `spawn`'s params, `answer`'s data), `migrate`'s bindings | refcount 0: the last reference is dropped after the outer call into core returns (deferred: dropping it can run Python code) |
  | pending | `{handler, on_error}` of a typed method | the method's call | its `REPLY`'s dispatch; `ta_host_free` (dropped, not called) |

- **The datamodel's last output.** `to_wire`, `serialize` and
  `payload_serialize` write with a writer the datamodel keeps, overwritten
  by its next call; strings handed back to core (`eval_string`, `log`)
  are the UTF-8 of a `str` the datamodel holds until its next call.
- **Payloads are copied, not encoded,** between the API and the
  `"python"` datamodel; CBOR only toward quickjs. A
  value the application passes in becomes a payload of the target
  session's datamodel (its chart's `datamodel`, `ta_image_chart_datamodel`).
- **Statuses are named by core:** a bare status in a message is
  `ta_status_name(status)` (`"TA_ERROR_STATE"`), never a table of ours.
- **Where the allocator is called:** in the host's create, free and
  allocator, and through core (`ta_table`, `ta_growable_arena` while it
  grows, the compiler's options); nowhere else, nothing per event or per
  evaluation. The lint rule `allocation-in-tables-and-host` checks it.
- **Images are bytes:** `ta_host_add_image` copies them, so a `Compiled`
  can be closed once `add_image` returns. No image rows, no
  `SEGMENT_FREED` bookkeeping. The compiler allocates through
  `ta_compile_options.alloc`, which the extension sets to
  `PyMem_RawMalloc` and `PyMem_RawFree` as for the host; a resolver's
  text and messages go into the compilation's scratch arena
  (`request->scratch`, from the start of `ta_compile_charts` to its
  return), so the extension keeps nothing per `src` past the call.
- **A `Compiled` holds a compile result** until its `close()`;
  `compiled.image` is a buffer over it. **`close()` while a buffer
  exported from `image` is held raises `BufferError`** (decided
  2026-10-13), as CPython's `bytearray` does when resized with exports:
  `mv = memoryview(compiled.image); compiled.close()` raises, and the
  result stays alive until `mv.release()`; without it `bytes(mv)` would
  read freed memory. The `Compiled` counts its exports in the buffer
  slots (CPython's `Py_bf_getbuffer` and `Py_bf_releasebuffer`, filled
  with our functions; in the limited API from 3.11 by CPython's
  documentation, not yet in the abi3 spike below: unverified).

## The "python" datamodel

Registered beside quickjs, first; matched by the chart's
`datamodel="python"`.

- **Per chart, once, at `chart_attach`:** a class whose `__slots__` are
  the chart's `<data>` ids and `_event`, with read-only properties
  `_sessionid`, `_name` and `_ioprocessors` (reading the session's system
  variables object); the top-level `def`s of the chart-level `<script>`s
  become its methods. Every expression is compiled once, by a restrictive
  rewrite of its AST, into a function of the session: `tries < 3` becomes
  `s.tries < 3`. A name the chart does not declare, or a construct the
  rewrite does not allow, is a compile error, reported when the
  expression is evaluated as `error.execution`, as SCXML requires
  (W3C test 312: an illegal expression in `<assign>` raises
  `error.execution`). `In(state)` calls `ta_datamodel_environment_in`
  with the evaluation's environment. A `.py` file a chart names through
  `src` gets the same rewrite in the resolver, so its errors are
  reported at compile time instead ("The API", "`.py` sources").
- **The rewrite's allow-list** (decided 2026-10-13), held in
  `src/datamodel/support.py` (Python, embedded by the build: the AST is
  not in the limited C API, so the rewrite walks it with Python's `ast`,
  once per chart; `expressions.c` is its C side) and the README:
  - *expressions:* literals, arithmetic, comparisons, boolean operations,
    conditional expressions, indexing and slicing, comprehensions and
    generator expressions, f-strings, calls to the chart's methods and
    to the allowed builtins, attribute access except dunder names;
  - *statements*, in `<script>` and `.py`: assignment, plain or
    augmented, to a variable or to its items or attributes (the
    variables: the `<data>` ids, then the names a `<foreach>`'s `item`
    or `index` (core's `TA_EXPRESSION_ITEM`) or a `<script>` outside its
    defs binds, declared by the chart, `None` until set; 2026-10-15,
    for W3C tests 150, 151, 302, 304, 525); `if`,
    `for`, `while`, `pass` and expression statements; `def` only at the
    top level of a script (any script's top-level defs are the chart's
    methods);
  - *refused:* `import`, `global`, `nonlocal`, `lambda`, `await`,
    `yield`, the walrus (`:=`), `del`, `try` and `raise`, `with`,
    `class`;
  - *builtins:* `len`, `min`, `max`, `sum`, `abs`, `round`, `sorted`,
    `reversed`, `enumerate`, `zip`, `range`, `any`, `all`,
    `isinstance`, `hasattr` (2026-10-15: whether an `_event` field is
    bound, W3C test 330), `str`, `int`, `float`, `bool`, `list`, `dict`,
    `tuple`, `set`, `frozenset`, `map`, `filter`; and not `open`,
    `eval`, `exec`, `getattr`, `setattr`, `vars`, `globals`,
    `__import__`.
  - **It is not a sandbox.** It keeps a chart inside SCXML's scope rules;
    charts in Python are trusted code (bindings.md section 8), and the
    README says so.
- **Per session, one instance.** The system variables are made into
  Python objects once, at `create` (core's views are valid only during
  that call, `datamodel.h:62-63`), immutable, and shared by reference.
  Assigning to an undeclared name or to a system variable raises
  (`__slots__`, read-only properties), and the evaluation is
  `error.execution`.
- **`_event`:** one immutable Event per event, built at `bind_event`
  (its strings converted during the call, `datamodel.h:35-37`), holding
  its data's payload value (the row's value itself, a reference on the
  Python object rather than on the row, so core's release of the row
  frees the row and the Event keeps the value); `_event.data` is that
  value. The datamodel sets `TA_DATAMODEL_EVENT_DETACHABLE`, so core calls
  `detach_event` at the stable point after the event's macrostep
  (`src/core/core.c:468-478`, `:1870`) and frees its own copy of the
  event. The datamodel keeps the Event and its value until the
  next `bind_event` or `destroy`: SCXML keeps `_event` bound to the last
  event until another is processed (5.10), so the `<onexit>` handlers of
  an actor terminated at rest read it (`core.c:1876-1878`) and
  `serialize` writes it. `detach_event` has nothing to drop.
  Its strings are shared, by core's ids for them and with the lifetime
  of their owner, never through a table of strings: the name by
  `name_id` in the chart row's `names`, the invoke id and the origin by
  `invoke_slot` in the chart row's `invokeids` and `origins` (the text is
  checked: an id generated per execution gets a `str` of its own), the
  origintype by `origintype_id` in the I/O processor types the datamodel
  keeps from the system variables, the type among the module's three.
  A blob or a snapshot keeps the three ids with the Event, so a woken or
  restored session shares them again.
- **Capture** (`<send>` data, `<donedata>`, invoke params) copies mutable
  values crossing to another actor and shares immutable ones; actors
  share no mutable value.
- **Hibernation writes the session out.** `serialize` writes the
  session's declared variables and its `_event` as CBOR through the value
  table (`{"vars": {id: value}, "event": [its eight fields and core's ids of
  its name, invoke and origintype] | null}`),
  with the writer the datamodel keeps until its next call; core puts the
  bytes in the blob and calls `destroy`, which removes the row: a
  hibernated `"python"` session's instance, variables and `_event` are
  gone, and the blob the application stores holds them. The payloads of
  its pending delayed sends and of the events waiting in its mailbox stay
  in the payload table: those stay in the host, and the blob does not
  carry them. `deserialize` reads it back into a new instance
  of the chart's class (a new row, so a new handle), and keeps the old
  bindings in the row for `migrate`. A variable holding a value outside
  the table (a `set`, a `tuple`, an object) makes `serialize` refuse with
  `TA_ERROR_UNSUPPORTED`: `Host.hibernate` raises and the session stays
  resident. Decided 2026-10-14, replacing "hibernation keeps the handle"
  (2026-10-13): with the handle, a hibernated session kept its instance
  on the Python heap, so hibernation freed the region but not the
  memory a population of millions needs to leave. The same `serialize`
  serves a migration's export (`ta_host_migrate`), which reaches
  it with no reason (`tinyactors-core/src/system/hibernate.c:329`), with
  `payload_serialize` for the payloads of pending delayed sends: an actor
  that holds a value outside the table, or has such a payload pending,
  refuses that too (its migration fails and it stays on the old
  version). Migration within the host (`tests/test_migration.py`, the
  suite's `migrate` scenario): `deserialize` with the new chart makes an
  instance of the new class and sets the variables both declare;
  `migrate` returns the old bindings as a payload, exactly the old
  chart's variables (quickjs's also names the new chart's new variables,
  undefined).
- **Snapshots of the datamodel** write, as one CBOR document through the
  value table, the session, payload and loop tables' handle namespaces
  (`ta_table_snapshot`) and every row: a session's variables, `_event`,
  `extra` and system variables, each value as CBOR of its own (so it
  nests as deep as in a send), with its chart named by its attachment's
  segment and the chart's index (`ta_datamodel_chart.segment`: two
  segments may run one image, and the restore attached the charts again,
  under new handles); a payload's value and references; a loop's items.
  The restore puts the namespaces back (`ta_table_restore`) and fills the
  rows at their handles, refusing a row twice or rows that do not match
  the tables. A value outside the table refuses the snapshot
  (`TA_ERROR_UNSUPPORTED`, a `MemoryError` stays one); values shared
  between variables are copied apart.
- **Measured cost per session** (`mise run measure`, 2026-10-14: x86_64
  Linux, 4 CPUs, CPython 3.13.16 with the GIL; 10,000 actors of a chart
  with three `<data>`, one of them a list, resident and idle): **433 B
  resident per actor** on the `"python"` datamodel against **1104 B** on
  quickjs, of which about 193 B are Python's heap (tracemalloc: the
  instance, its list, the session row) and 204 B core's region (both
  datamodels). After one event each, a session also keeps its `_event`,
  one Event of about 100 B (its name is the chart's shared `str` for that
  name in the image): 814 B resident against quickjs's 13777 B, most of
  which is core's region growing its pools.
  The estimate it replaces (the instance 32 B + 8 B per slot, a row
  ~16 B, the system variables ~100–250 B) was close for the instance (80
  B with five slots, plus its GC header); the system variables cost
  nothing here, because they are made only for a chart whose code reads
  them, and the row was 32 B then (instance, chart, the params until the
  first event, flags; 24 B since flags left it, 2026-10-14). Compiled code belongs to the chart, never to a session.

## C rules

- **At most three parameters** for every C function this binding defines
  or declares, static helpers and callback types included (root
  `AGENTS.md`, "At most three parameters"). A function whose signature a
  typedef fixes is exempt by that typedef, not by name: `PyCMethod`
  (`METH_METHOD | METH_FASTCALL | METH_KEYWORDS`, five parameters),
  `PyCFunctionFastWithKeywords` (`METH_FASTCALL | METH_KEYWORDS`, four),
  `ta_allocator`'s two. `tools/lint/utils/fixed-signature.yml` lists them
  by their parameter types; add a typedef there, with its header and line,
  before a function it fixes fails the rule.
- **Bytes are `ta_bytes` in and `ta_buffer *` out**, never a pointer and a
  length, in every function (`buffers-are-ta-buffer`).
- **No serialization on the hot path** (root `AGENTS.md`): spawn, send,
  send_many, terminate, deliver, advance_time, resume, run, the effects
  and their ack take and give C structs. CBOR is for `ta_host_post`, the
  config and bytes that leave the process.
- **Acronyms** are all capitals or all lowercase (root `AGENTS.md`,
  "Acronyms"): `ta_python_otlp_writer`, `TA_PYTHON_IO_...`.
- **Layers**, each an ast-grep rule:
  - `extension-calls-core-and-python`: `src/` calls core (`ta_*`, `TA_*`),
    CPython's API (`Py*`, never a private `_Py*`) and a short list of C
    library functions (memory and strings), nothing else. A C library
    function the binding needs joins the list with its reason, in the
    commit that first calls it.
  - `datamodel-calls-environment-and-python`: `src/datamodel/` calls of
    core only `ta_datamodel_environment_*`, `ta_image_*` (the chart's
    expressions, read at `chart_attach`), `ta_cbor_*` (`to_wire`,
    `from_wire`) and `ta_table_*`, and our own `ta_python_*`; never the
    host or the system (`datamodel.h`: "they must never call ta_system_*
    functions").
  - `one-timer-in-loop`: the loop's timer and wakes (`call_at`,
    `call_later`, `call_soon`, `sleep`) appear in `src/loop.c` only.
- **No mutable globals** (`no-mutable-globals`): no global and no static
  local the extension writes. The module is imported once per
  interpreter, and a host may be driven from any thread of the
  free-threaded build, so state lives in the module's state
  (`PyModule_GetState`) or on the `Host`. A constant is a `const` object
  (`static const char *const names[]`; `static const char *p` is a
  mutable pointer). CPython's tables, which its API takes by non-const
  pointer, are exempt by type: `PyMethodDef`, `PyMemberDef`,
  `PyGetSetDef`, `PyType_Slot`, `PyType_Spec`, `PyModuleDef`,
  `PyModuleDef_Slot`, `PyStructSequence_Desc`, `PyStructSequence_Field`.
- **One thread per host.** Record the creating thread and refuse calls
  from others (bindings.md section 5); the GIL is not a substitute.
- **The wheel is abi3 from 3.13** (`Py_LIMITED_API 0x030D0000`;
  `pyproject.toml`, `mise.toml`). A spike compiled `PyMem_RawMalloc`,
  `PyMem_RawFree`, `PyType_GetModuleByDef`, `PyType_GetModule`,
  `PyModule_GetState`, `PyType_FromModuleAndSpec`, `PyErr_SetInterruptEx`,
  `PyStructSequence_NewType`, `Py_CompileString`, `PyEval_EvalCode`,
  `PyDict_GetItemRef`, `PyList_GetItemRef`, `PyErr_GetRaisedException`,
  `PyErr_SetRaisedException`, `PyUnicode_AsUTF8AndSize`,
  `PyObject_GetAttrString`, `PyObject_CallMethod`, `PyErr_SetString`, the
  slots `Py_mod_exec`, `Py_mod_gil` (with `Py_MOD_GIL_NOT_USED`),
  `Py_tp_traverse`, `Py_tp_clear`, the flags `METH_METHOD | METH_FASTCALL
  | METH_KEYWORDS` and the typedefs `PyCMethod` and
  `PyCFunctionFastWithKeywords` with clang 22 against CPython 3.13.16's
  headers and `Py_LIMITED_API 0x030D0000`, with no error; at `0x030C0000`
  (3.12), `PyMem_Raw*`, `PyDict_GetItemRef`, `PyList_GetItemRef`,
  `PyType_GetModuleByDef` and `Py_mod_gil` are undeclared. Module state
  comes from `PyType_GetModuleByDef` (it searches the type's MRO, so a
  method called on a subclass finds the module) and `METH_METHOD`.
- **The free-threaded build is for tests only.** The limited API cannot
  be compiled for a free-threaded interpreter before 3.15 (`Python.h`
  refuses `Py_LIMITED_API` with `Py_GIL_DISABLED`: 3.13, line 51), so the
  test task builds the same sources again without `Py_LIMITED_API` for
  the free-threaded interpreter (`TA_PYTHON_FREE_THREADED` in
  `mise.toml`, installed by uv). The functions above are in the limited
  API from 3.13 and in the full API of 3.14t alike, so the sources need no
  compatibility macros. A free-threaded wheel waits for abi3t (3.15).

## Values

- **The value table** is `tinyactors-core/docs/protocol.md`, "Values",
  with Python's side in `docs/values.md`: `int` to 64 bits (exact),
  `float`, `str` (valid UTF-8: a lone surrogate is refused, never
  replaced), `bytes`, `list`, `dict` with `str` keys, `True`, `False`,
  `None`, the singleton `UNDEFINED`, and `Address` for tag 32 (the "not
  ECMAScript" vectors of `values.txt` hold it). Test `bool` before `int`.
- **`tinyactors.encode(value) -> bytes`** and **`tinyactors.decode(data)
  -> object`** expose the one conversion the API uses (`src/values.c`,
  streaming with `ta_cbor_writer` and `ta_cbor_reader`), each with its row
  in `docs/api-map.md`. What they cannot read or write is refused with an
  exception, never turned into a value. The adapter frames the driver's
  calls with them and imports nothing else; since the vectors then cross
  this codec both ways, the binding's own value tests (against
  `values.txt`'s `python` lines) catch a value mapped wrong both ways.
- **Nesting is limited to 64**, which stops cycles but not a value that
  shares subtrees; the README says charts are trusted code.

## Rules the suite cannot see, each with a test of its own

- **Dropping a payload's last reference can run Python code** (a
  finalizer, a weakref callback), and core calls `release` outside a run
  too (the ack, `ta_host_free`, posts, deliver), where it holds the run's
  rules. Defer the last drop to a list emptied after the outer call into
  core returns (bindings.md, section 8).
- **Handles are `ta_table` handles** with a generation: a stale one is
  refused, never a pointer; a reply whose id names no pending row is
  dropped.
- **A thin view read after its ack raises** `TA_ERROR_STALE`, and a
  record made for an `async def` processor answers after the ack.
- **Marks nest:** a conversion that runs Python code which calls the
  `Host` again leaves the outer call's arena memory intact, and an
  exception releases to the mark.
- **The session scope refuses** an assignment to an undeclared name or to
  a system variable (`error.execution`), and generator expressions and
  lambdas in an expression see session variables (through the rewrite).
- **A `BaseException` that is not an `Exception`** (`KeyboardInterrupt`,
  `SystemExit`) still gives `error.execution` to the chart, and the
  binding raises the same object again after the run.
- **Handler closures make cycles:** fill the CPython slots
  `Py_tp_traverse` and `Py_tp_clear` with our functions.
- **One drain at a time and an ack before every run:** a test that a
  handler's typed method is answered in the next take, and that a view's
  fields read in a handler stay valid after later calls in the same batch.
- **Pending handlers are dropped at `ta_host_free`**, never called.
- **An `async def` I/O processor under `with`** raises `TypeError` naming
  it; **a processor that raises before answering** has its send failed
  (the sender gets `error.communication`) and the exception raised after
  the turn, for a plain function and an `async def` task alike.
- **`compile_charts` raises `CompileError` with every diagnostic**, a
  `.py` file's refused construct at its own line, a `ResolveError` at
  the line and column it names; `compiled.image` read after `close()`
  raises `TA_ERROR_STALE`, and `close()` while a `memoryview` of it is
  held raises `BufferError`.
- **`TA_EFFECT_REQUEST` is answered in C with `TA_ERROR_INVALID`** (the
  parent's `error.execution`) until run-time fetching is built; no Python
  code runs.
- **Traces cross:** a `traceparent` passed to `send` or `spawn` is the
  trace of the step it starts, `send.traceparent` is the sending step's,
  `send.answer()` keeps it and `send.answer(..., traceparent=None)` does
  not.
- **The README says** that charts are trusted code and the rewrite is
  not a sandbox, and that Python values lie outside the host's allocator
  (bindings.md, section 8).

## What not to build

- No synchronous request, no raw `post`, no request facade, no reply
  object or future: typed methods take response handlers.
- No re-reading `ta_host_effects` until stable, no consumed marks, no
  nested drain, no ack inside a handler.
- No eager conversion of an effect: a thin view, and a record only where
  a value must outlive the ack.
- No copy of a host output before the next call (it lives until the ack).
- No allocation of our own per call: the host's arena with a mark.
- No handle table, status table, arena or image rows of our own
  (`ta_table`, `ta_status_name`, `ta_growable_arena`,
  `ta_host_add_image` copies).
- No global configuration: handlers, logging and the clock are per host.
- No resolver on the `Host` and no Python callback for
  `TA_EFFECT_REQUEST` now: the compile call's resolver lives only at
  compile time, and a REQUEST is answered in C with `TA_ERROR_INVALID`
  until run-time fetching is built (root `ROADMAP.md`, "Decided
  later").
- No `DirectoryResolver` in the extension: it is the convenience layer's,
  in Python.
- No application callback for `DONE` or `ERROR`; no subscription.
- No defaults, limits, methods, events or messages of our own; nothing C
  does done again in Python (bindings.md section 2).
- No convenience layer yet ("Layers"): no `add_image` from a mapping, no
  spawn by chart name, no chart builder in 8.4.
- No serialization on the hot path, no struct layout read from Python.

## Tasks

`mise.toml` pins python (3.13, and the free-threaded interpreter the tests
build for) and uv; the C toolchain is the root's.

- `mise run build`: core's amalgamation into `build/core`
  (`mise run //tinyactors-core:amalgamate -- --out build/core`), the only
  core task this project's tasks run, never core's build; the extension
  compiles against it with `Toolchain::Target` (root
  `mise/lib/toolchain.rb`; never a compiler from `PATH`, never `$CC`).
- `mise run lint`: the rules' tests, the rules over `src/`, the adapters'
  lint (`mise run //tinyactors-test:lint -- conform`: the adapter imports
  only `tinyactors` and the standard library), `docs/api-map.md` against
  core (`mise/lib/api_map.rb`), and the root's checks on this project's
  files: the build scripts' portability and acronyms (`.py` files
  included).
- `mise run conform [-- runner options]`: the suite against
  `python -m conform`, with `--kind binding`, on the native datamodel and
  on quickjs (both must pass); `-- --milestone <M>` runs one of the
  suite's sets (`tinyactors-test/suites/milestones.txt`).
- `mise run test`: lint, build, both interpreters, and the suite's set
  named by `MILESTONE` in `mise/cpython.rb` once the adapter exists. Its
  `#MISE sources` list this project's files, core's and the suite's. An
  exhaustive test goes behind `--full` (root `AGENTS.md`).
- `mise run wheel [target...]`: one abi3 wheel per native release target
  (`mise/wheel.rb`; root `docs/adr/0001`, decisions 21 to 24), built with
  the root's zig against the target's own CPython 3.13 headers, checked
  by its symbols (exports `PyInit_tinyactors` only, loads no library,
  leaves undefined only the C API and `Wheel::LIBC`), zipped
  reproducibly into `build/wheels/`; this machine's is installed into
  fresh 3.13 and 3.14 environments and smoke-tested (`tools/smoke.py`).
- `mise run release -- <version> [--publish] | --dev [--publish]`: the
  checks (a clean tree, the version, the changelog, PyPI, the test task),
  the version committed and tagged `tinyactors-cpython/v<version>`, every
  wheel, the smoke tests here (with `tests/` against the installed
  wheel) and in glibc and musl containers of each Linux architecture,
  `SHA256SUMS`; with `--publish`, `uv publish` with fnox's `PYPI_TOKEN`,
  then the push and the GitHub release; a publish that stopped after its
  upload began keeps its commit, tag and files, and `--resume` uploads
  exactly those (PyPI refuses new bytes under a name it has) and pushes.
  The smoke tests load the extension with `LD_BIND_NOW=1`. A new C
  library function the extension calls fails the build until
  `Wheel::LIBC` lists it.

Put the built extension on the adapter's import path in the conform task
(`PYTHONPATH`), never in the adapter: its lint refuses `sys.path`.

## Commits and the changelog

Commit subjects are the changelog's input (root `AGENTS.md`, "Releases
and changelogs"): scope `cpython` (`feat(cpython): ...`), `!` for a
breaking change with a body that says what a caller does.

## Code search, static checks and design claims

- **ast-grep for code search and rewrites** (`ast-grep run -p ... -r
  ...`), `rg` only for plain text. Invoke `ast-grep`, never `sg`. A new
  static check is an ast-grep rule in `tools/lint/rules/` with its test in
  `tools/lint/tests/`.
- **Design claims are checked** with the reasoned-analysis skill: a
  scenario in this design, evidence (`path:line`, a primary source or a
  spike) or the word unverified, and a refutation by a fresh agent before
  a finding changes the design, the guide or a request to core.

## Gaps in core

This project never patches `tinyactors-core` or the suite
(`tinyactors-test`), and works around nothing in them. A gap is a request
to core in [ROADMAP.md](ROADMAP.md): what is missing, why, and which
suite case or test shows it. The binding waits for the change.

## Layout

The Node binding has the same files on purpose, and the two are compared
file by file.

```
README.md, CHANGELOG.md, mise.toml, pyproject.toml, sgconfig.yml
ROADMAP.md           the work in progress: plan, progress, how it runs, requests to core
docs/api-map.md      each API item → ta_* function, method or effect (checked)
docs/values.md       the Python side of the value table
src/                 extension.h, docs.h (every public docstring: when and how to use an item,
                     where it fits; tests/test_docs.py checks each item has one),
                     module.c, host.c (Host, inputs, the arena's marks),
                     loop.c (drain, turn, both drivers), effects.c (dispatch, thin views,
                     records), methods.c (typed methods, the pending table), values.c,
                     compile.c (compile_charts, compile_system, Compiled, the resolver, ResolveError),
                     datamodel/{datamodel.h, datamodel.c, rows.c, expressions.c,
                     support.py (the rewrite, the exceptions; embedded, not shipped)}
conform/             the suite's adapter, python -m conform; not in the wheel
tests/               what the suite cannot see: threads, GC, asyncio, refusals, stale views
tools/lint/          rules/, tests/, utils/ (ast-grep)
mise/cpython.rb      what the tasks share
mise/wheel.rb        the wheel: the release build per target, its checks, the zip, the smoke tests
mise/tasks/          build, test, conform, lint, wheel, release
build/               core's amalgamation and the extension (not in git)
```
