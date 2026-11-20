# Changelog: tinyactors-cpython

What changed in tinyactors-cpython in each release. The summary of a release
is written for someone upgrading; the lists below it are generated from the
commits and from the public API (docs/adr/0002-changelogs.md).

## [0.4.0] - 2026-11-19

### Summary

This release is about writing applications rather than driving the C API. A `Host` now takes its compiled charts and its `system` at creation; I/O processors are objects with `receive(send)`; handlers no longer take the host; `spawn` returns an `Actor` you send to, inspect and `await actor.ask(...)`; and a `store` with `resident` hibernates the surplus and wakes actors for you (MemoryStore and SQLiteStore are provided). `compile_dir`, the `Spawner` I/O processor, `on_done`, `on_error`, `on_handler_error`, error classes for the commonest statuses (`StaleError`, `StateError`, `InvalidError`, `UnsupportedError`; any other stays a `TinyactorsError`), `decode_traces`, `__version__` and awaitable typed methods under asyncio fill in the rest. The README now leads with this API, then "Operating a host" and "Wire format"; a guide covers a send to an actor that is not there, and a chat server ships as an example.

Sends follow SCXML more closely: a sender waits until its send is decided (at once for a send the host delivers itself, at the ack of the batch for one it hands to an I/O processor), so a processor that answers in its handler is answered before the sender goes on, and the `sync` option is gone; sends to other systems go to the host's `transport`, as a `send.message` the other side passes to `host.send`.

Stored state is safer: `wake` refuses an older blob or another host's, and a finished migration lists the sessions it could not move and why. Also new: `error.execution` carries a reason and message, quickjs has a 1 GiB default memory limit, and a host whose config sets no trace seed gets a random one.

### Upgrading

- **Creating a host** (9c84f1c): pass the images to `Host` (a `Compiled`, image bytes or a list) and the system id as `system=` instead of systemID in the config; `config=` holds the rest. `Host.restore` takes the snapshot and the charts.
- **Handlers** (9c84f1c): an I/O processor is an object with `receive(send)` and an optional `location`, given in `io_processors`. Every handler loses its host parameter (`receive(send)`, `log(entry)`, `telemetry(signal, data)`, a service's `handler(invocation)`, `migration_done(result)`, a typed method's `handler(result)` and `on_error(error)`, ...); read `view.host` where you need it.
- **Actors** (9c84f1c): `spawn` takes a chart's name or a ChartRef and returns an `Actor` (its `id` is the session); every call that takes a session takes an `Actor`. Replace `actor_by_name(name)` with `host.actor(name)`, which raises `StaleError` instead of returning `None`. `await host.actors(chart=...)` lists the live ones.
- **Hibernation** (9c84f1c, dcbe4f0): replace `wake=` with `store=` (save, load, delete), and `resident=` for a bound on resident actors. `wake` now raises `TA_ERROR_STALE` for a blob older than the session's last hibernation: give it the latest one. A blob of another `Host` raises `TA_ERROR_INVALID` (a848f66): wake a blob only in the host that wrote it, or in one restored from its snapshot.
- **Compiled images and drivers** (ca00eff): `Compiled.image` is `bytes`, copied at its first read and kept after `close()` (a first read after `close()` raises `TA_ERROR_STALE`, so read it before); drop `memoryview(compiled.image)` and `bytes(compiled.image)`, and `close()` no longer raises `BufferError`. `host.run()` is removed: wait for timers with `async with` and `await host.idle()`, or test with a `SimulatedClock` and `run_until_idle()`.
- **Sends wait** (d7db9ad): drop `sync` from every `ioProcessors` entry (the config refuses it). A slow handler now delays its sender, so hand the message on and return.
- **Other systems** (f4c2819, f53ae02, bf0b8a1): drop the `remote` entry from `ioProcessors` and give `Host(..., transport=handler)`; without a transport each such send fails, and an `async def` transport needs `async with`. The transport moves `send.message` and the other side gives it to `host.send` unchanged; report a failure with `Host.fail(target, sendid)`. `send.origin` and the transport's `send.target` are `Address` values (`str(...)` or `.text` for the text). Charts no longer see a `tinyactors` entry in `_ioprocessors`, and a `<send>` can no longer name it as a type.
- **Answers to an ended sender** (34e8372): `send.answer()` and `send.fail()` return `False` instead of raising `TA_ERROR_STALE`; check the result.
- **Migration results** (43deb01): `migration_done`'s `result["failed"]` is a list of `{session, reason}`, not a count; use `len(result["failed"])` for the number.
- **Scripts in executable content** (fa0f761): compile older images again. On the ECMAScript datamodel a `let`, `const` or `class` in a `<script>` of a transition, `<onentry>` or `<onexit>` is now local to each run; declare a name later expressions read in a chart-level script or a `<data>`.
- **Spawner** (da0656f): new in this release, it spawns the chart's current version under the name each time and answers `spawned` with `{name, address}`; it does not look the name up, so one actor should own each name. `Host.locate`, added and removed during this release, is not in it.
- **Inspecting a hibernated actor** (712305d): the actor.state method (`state()` on an `Actor`) now refuses a hibernated actor with reason `state` instead of answering `{}`; wake it first.
- **Migrating what is not there** (0a231a5): `host.migrate` now raises `TA_ERROR_INVALID` for a chart name the host does not have and `TA_ERROR_STALE` for a session that ended, where the migration used to end at once with nothing moved.

<!-- Generated by `mise run changelog:write`. Do not edit below: `mise run changelog:check` renders it again. -->

### Breaking changes

- the Spawner is a plain spawn; Host.locate removed (da0656f)
- Host(charts, system=...), processor objects, Actor handles, store and resident (9c84f1c)
- __version__, Compiled.image as bytes, host.run() removed (ca00eff)
- send.answer() and send.fail() return False for a sender that ended (34e8372)
- a <script> in executable content runs again: its let and const are its own (fa0f761)
- a migration names the sessions it could not move, and why (43deb01)
- wake refuses a blob of an earlier hibernation (dcbe4f0)
- a message between systems in two halves, and an address's parts (f53ae02)
- one numbering of I/O processors in every layer (bf0b8a1)
- a host's sends to other systems go to its transport (f4c2819)
- a sender waits for its send; "sync" goes (d7db9ad)

### Changes

**Added**

- Spawner holds a name once per version; Host.locate (b0e5ffd)
- the chat as an example application, run by mise run test (5063964)
- Actor.ask and Spawner (48a4674)
- compile_dir, a directory's charts and helper files in one image (24190ca)
- decode_traces, spans without a host (5e5b090)
- typed methods can be awaited (12faafb)
- Address.from_name and Address.from_session (eedee0a)
- one error class per status (5529761)
- the binding picks a random trace seed when the config gives none (19d8ee2)
- quickjs has a default memory limit of 1 GiB (7b19ec8)
- actor.get lists an actor's pending delayed sends, actor.state where its history states would return (712305d)
- error.execution says why in its data (6a5df83)

**Fixed**

- the Spawner looks a name up as a name, as Node's does (f066819)
- terminating a hibernated actor no longer deletes its blob before core wakes it (2953d5c)
- the adapter writes no error of core's; two config vectors are reference-only (5f780d2)
- a Host draws its identity from os.urandom, so wake refuses another host's blob (a848f66)
- actor.context tells undefined from null (867b693)
- an invoke of a type with no service tells the invoker and throws nothing; answer() and fail() give false when the sender ended (eec9515)
- migrate refuses an unknown chart and an ended session (0a231a5)
- host.send(message) takes UNDEFINED as absent for sendid, origin and traceparent (542e013)

<details>
<summary>Internal (11)</summary>

- key a table by the sender's fragment of _event.origin (0c34bd5)
- the README leads with the application API; "Operating a host" and "Wire format" apart (83efcfc)
- Revert "fix(napi): version comes from a module written at pack time" (8fdca56b) (204ac04)
- roadmap step 8.6, the application API of both bindings (fc63118)
- the suite checks core's traceSeed default where core runs it; the adapters pass configs through (3eaf516)
- error.execution's message is checked by its type, the same on 3.13 and 3.14t (53cfebf)
- a growing segment grows the pool that ran out (ecb80b2)
- the request "one exhausted pool doubles every pool" answered (1694d2e)
- an I/O processor's handler dispatches, it does not wait for delivery (65ad95c)
- rewrite the README and the module's docstrings for readers (edeb386)
- move the repository to tinyactors-dev/tinyactors (1de22dc)

</details>

## [0.1.0] - 2026-10-15

### Summary

The first release of tinyactors for Python: SCXML statecharts run as actors
in a host, driven from Python. Charts can be written with Python as their
datamodel, or in ECMAScript (quickjs). A host can be driven from a script or
from asyncio, and the values actors exchange are plain Python objects. The
extension is a direct layer over the C library: each method mirrors one of
its calls, and the package's API map names the call behind each one.

Besides compiling charts, spawning actors and sending events, the release
exposes the rest of the host. Actors can hibernate to blobs the application
stores and later wake from them. Running actors can migrate to a new version
of their chart. A whole host can be snapshotted and restored, in this
process or another one. Charts can invoke services the application
registers, systems talk to each other through an I/O processor the
application supplies, and the host emits OpenTelemetry traces, logs and
metrics as OTLP bytes for the application to send on.

It ships as one wheel per platform for CPython 3.13 and later (the stable
ABI): macOS on Apple silicon and Intel, and Linux on x86-64 and arm64.
There is no source distribution, and the free-threaded interpreters are not
supported yet. The package is alpha: its API can change in later 0.x
releases.

