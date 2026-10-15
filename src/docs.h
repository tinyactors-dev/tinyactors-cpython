/* The docstrings of the extension's public API: the module, its
 * functions, its types and their methods and fields. Each says when and how
 * to use the item and where it fits (GLOSSARY.md has the words). The type
 * and method tables point here; tests/test_docs.py checks that every public
 * item has one. A text that starts with a signature line and "--" gives
 * the item its signature in help() and inspect. */
#ifndef TA_PYTHON_DOCS_H
#define TA_PYTHON_DOCS_H

static const char ta_python_doc_module[] =
    "SCXML actors in a host, with charts in Python or ECMAScript.\n"
    "\n"
    "The pieces, from the outside in (GLOSSARY.md in the repository has every\n"
    "word):\n"
    "\n"
    "- A Host runs many actor systems as one. It is the only object that\n"
    "  holds actors; everything else here is made for it, by it or about it.\n"
    "  Create one with Host(config, ...), drive it with run_until_idle(), run()\n"
    "  or `await host.idle()`, and leave it with `with` / `async with` or\n"
    "  close().\n"
    "- Charts are SCXML state machines. compile_charts() turns documents into\n"
    "  an image (Compiled.image); compile_system() does it for a <system>\n"
    "  manifest that also names the actors an application starts with, which\n"
    "  Host.boot() spawns. Host.add_image() runs an image as a new\n"
    "  segment of the host: one actor system, with its own memory. Adding an\n"
    "  image with a chart of a name the host already runs is a deploy: the new\n"
    "  segment holds the chart's current version, and Host.migrate() moves the\n"
    "  running actors over (they get error.platform.upgrade).\n"
    "- Actors are running instances of a chart: Host.chart(name) gives the\n"
    "  current version as a ChartRef, Host.spawn() starts an actor and returns\n"
    "  its session id (Host.actor_by_name() finds a named one again),\n"
    "  Host.send() and send_many() give it events. A chart's\n"
    "  datamodel is \"python\" (expressions in a restricted Python, values are\n"
    "  Python objects, _event is an Event) or \"ecmascript\" (quickjs).\n"
    "- What actors ask of the world reaches your handlers, which you pass to\n"
    "  Host(...): io_processors for <send> to a transport (each gets a Send),\n"
    "  services for <invoke> of a service you run (each gets an Invocation),\n"
    "  log for <log> (a LogEntry), wake for a hibernated actor that has work\n"
    "  (Host.wake answers it), migrated (a Migrated) and\n"
    "  migration_done for migrations, and telemetry (OTLP bytes). Other systems (another thread's host, a browser) are reached\n"
    "  through the I/O processor the config marks \"remote\": its handler\n"
    "  carries a Send to the other side, where Host.send(address, ...,\n"
    "  origin=send.origin) delivers it. A handler that is a plain\n"
    "  function gets a thin view, valid until the host acknowledges the batch\n"
    "  it came in; an async def handler gets a record.\n"
    "- Questions about the host itself are typed methods (actor_state,\n"
    "  host_stats, trace_watch, ...): each takes a handler that runs at a\n"
    "  later drain with the answer.\n"
    "- Taking actors out of memory: Host.hibernate() writes an actor at rest\n"
    "  to bytes you store, Host.sweep() says which ones are cold, Host.wake()\n"
    "  brings one back. Host.snapshot() and Host.restore() copy a whole host,\n"
    "  every segment at once: for rehearsing a deploy, capturing a host for a\n"
    "  developer, preparing tests, or taking a host out of the process. None\n"
    "  of these is durability: keeping data is your database's.\n"
    "- Values crossing between Python, quickjs and other processes are those\n"
    "  of the value table: int, float, str, bytes, list, dict with str keys,\n"
    "  bool, None, UNDEFINED and Address. encode() and decode() are its CBOR\n"
    "  form. SimulatedClock drives time in tests.\n"
    "- Errors core reports are TinyactorsError (its status names core's\n"
    "  status); a compilation that fails raises CompileError with every\n"
    "  Diagnostic.";

static const char ta_python_doc_compile_charts[] =
    "compile_charts(sources, resolver=None)\n"
    "--\n"
    "\n"
    "Compile SCXML documents into one image, the form a Host runs.\n"
    "\n"
    "`sources` is a list of (name, text) pairs: each text is one SCXML\n"
    "document, named for diagnostics and for src= references between them;\n"
    "the first is the image's default boot chart. Use one call per image:\n"
    "all the charts a host should run as one segment together (a chart and\n"
    "the charts it invokes), and a later call for a new version.\n"
    "\n"
    "`resolver`, a callable resolver(base, source) -> str, is called at\n"
    "compile time for each static src= that is not among the sources (a\n"
    "<script src>, an <invoke src>, a .py file of methods); it returns the\n"
    "text, or refuses by raising ResolveError, which the compiler reports at\n"
    "the line and column it names. It lives only for this call.\n"
    "\n"
    "Returns a Compiled: pass Compiled.image to Host.add_image, then close it\n"
    "(or use it as a context manager); its diagnostics are the warnings and\n"
    "notes. A compilation with an error raises CompileError, with every\n"
    "diagnostic, after freeing the result.";

static const char ta_python_doc_compile_system[] =
    "compile_system(manifest, resolver=None)\n"
    "--\n"
    "\n"
    "Compile a system manifest into one image: its charts and the actors it\n"
    "starts with.\n"
    "\n"
    "`manifest` is a (name, text) pair whose text is a <system> document:\n"
    "<chart name=\"worker\" src=\"worker.scxml\"/> for each chart, read through\n"
    "`resolver` (as for compile_charts), and <actor name=\"first\"\n"
    "chart=\"worker\"/> for each actor Host.boot spawns, under that name.\n"
    "\n"
    "Use it when an application is a fixed set of long-lived actors (a\n"
    "router, a scheduler, a few workers): the manifest says what runs, so a\n"
    "deploy is one image to add and boot, and your program finds each actor\n"
    "by name (Host.actor_by_name, or the address .../@first). Use\n"
    "compile_charts when your program spawns its actors itself, one per\n"
    "order or user. Returns a Compiled, as compile_charts does; an error\n"
    "raises CompileError with every diagnostic.";

static const char ta_python_doc_encode[] =
    "encode($module, value, /)\n"
    "--\n"
    "\n"
    "Encode one Python value as CBOR bytes of the value table.\n"
    "\n"
    "Use it where a value crosses a process boundary: a config written to a\n"
    "file, a message for another system, a value another program decodes. Values you\n"
    "pass to a Host in this process (send, spawn's params, a service's\n"
    "data) need no encoding: they cross as Python objects.\n"
    "\n"
    "Supported: exact built-in int (-2**64 through 2**64-1), float, str,\n"
    "bytes, list, dict with str keys, plus bool, None, Address and\n"
    "UNDEFINED. Address keeps tag 32; UNDEFINED keeps undefined, inside\n"
    "containers too. Container order is kept. Subclasses and tuples are not\n"
    "supported.\n"
    "\n"
    "Raises TypeError for unsupported types or non-str map keys,\n"
    "OverflowError for integers or text outside core's length range,\n"
    "UnicodeError for lone surrogates, ValueError for excessive nesting\n"
    "(cycles included), and MemoryError on allocation failure.";

static const char ta_python_doc_decode[] =
    "decode($module, data, /)\n"
    "--\n"
    "\n"
    "Decode exactly one CBOR item of the value table from bytes.\n"
    "\n"
    "The inverse of encode, for values that arrive from another process.\n"
    "Arrays become lists, maps become dicts with str keys (a repeated key\n"
    "keeps its last value), tag 32 on text becomes Address, undefined\n"
    "becomes UNDEFINED. Other tags and simple values are refused.\n"
    "\n"
    "Raises TypeError for input other than bytes, ValueError for malformed,\n"
    "truncated, trailing or unsupported CBOR, and MemoryError on allocation\n"
    "failure.";

static const char ta_python_doc_compiled[] =
    "The result of compile_charts or compile_system: an image and the\n"
    "compilation's notes.\n"
    "\n"
    "Hold it only as long as you need the image: Host.add_image copies the\n"
    "bytes, so close it right after adding (a `with` block does), or keep it\n"
    "to add the same image to several hosts. To compile once and run many\n"
    "times (at build time, in CI), write `bytes(compiled.image)` to a .tai\n"
    "file and give its bytes to add_image later: no compiler at run time.";

static const char ta_python_doc_compiled_image[] =
    "The image's bytes, as a read-only memoryview over the compile result.\n"
    "\n"
    "Pass it to Host.add_image or copy it with bytes(). It is valid until\n"
    "close(): reading it later raises TinyactorsError (TA_ERROR_STALE), and\n"
    "close() while a memoryview of it is held raises BufferError.";

static const char ta_python_doc_compiled_diagnostics[] =
    "The warnings and notes of a compilation that succeeded, a list of\n"
    "Diagnostic (errors raise CompileError instead).";

static const char ta_python_doc_compiled_enter[] =
    "Use the result in a `with` block: returns it, and close() runs at the block's end.";

static const char ta_python_doc_compiled_exit[] =
    "Leave the `with` block: close() the result, so the image is freed.";

static const char ta_python_doc_compiled_close[] =
    "close()\n"
    "--\n"
    "\n"
    "Free the compile result. The image is stale afterwards; the\n"
    "diagnostics stay. A `with` block calls it on exit, and the collector as\n"
    "a fallback.";

static const char ta_python_doc_host[] =
    "Host(config, io_processors=None, *, log=None, clock=None, telemetry=None, wake=None, migrated=None, services=None, migration_done=None)\n"
    "--\n"
    "\n"
    "A host: many actor systems run as one, the object that holds actors.\n"
    "\n"
    "`config` is the host's configuration as a dict (systemID, system limits\n"
    "such as maxActors and instanceBytes, ioProcessors, telemetry; see the\n"
    "core's host-config documentation). Everything else is your side of the\n"
    "host, all optional:\n"
    "\n"
    "- io_processors: {type: handler(host, send)} for each I/O processor the\n"
    "  config declares; an actor's <send type=...> reaches it as a Send. The\n"
    "  one the config marks \"remote\" gets every send to another system's\n"
    "  address (tinyactors://<other systemID>/...): your transport.\n"
    "- services: {type: handler(host, invocation)} for the services your\n"
    "  application runs; an <invoke type=...> starts one with an Invocation,\n"
    "  and the handler returns the service object (README, \"Invoked\n"
    "  services\").\n"
    "- log: handler(host, entry) for <log> lines (LogEntry).\n"
    "- wake: handler(host, session, migration) when a hibernated actor has\n"
    "  work (migration: the id of the migration that asks, or None); answer\n"
    "  with Host.wake(session, blob).\n"
    "- migrated: handler(host, migrated) for each actor a migration moved.\n"
    "- migration_done: handler(host, result) when a migration (Host.migrate)\n"
    "  ends, with its counts and the sessions it left behind.\n"
    "- telemetry: handler(host, signal, data) with OTLP bytes, signal\n"
    "  \"traces\" or \"logs\".\n"
    "- clock: a SimulatedClock, for tests; the real clock otherwise.\n"
    "\n"
    "A plain function handler gets a thin view, valid until the batch is\n"
    "acknowledged; an async def handler gets a record and needs the asyncio\n"
    "driver (`async with`). A handler's exception is raised by the driver\n"
    "after the turn.\n"
    "\n"
    "A typical program: compile_charts() the charts, add_image() the image,\n"
    "spawn(host.chart(\"order\")) an actor, send() it events, and let the\n"
    "host turn; your handlers do the I/O the actors ask for.\n"
    "\n"
    "Drive the host with run_until_idle() (until nothing is runnable now),\n"
    "run() (sleeping to each timer), or `await host.idle()` under asyncio.\n"
    "Inputs (spawn, send, terminate, ...) apply at the call; their effects\n"
    "reach your handlers in the next turn. One thread per host.";

static const char ta_python_doc_host_closed[] =
    "True once the host was freed (close(), the end of a `with` block); every call refuses then.";

static const char ta_python_doc_host_ticks[] =
    "The host's clock rate, config's system.ticksPerSecond, read once at\n"
    "creation: delays in charts count in these ticks; the API takes and\n"
    "gives seconds (next_deadline, migrate's timeout, SimulatedClock).";

static const char ta_python_doc_host_next_deadline[] =
    "Seconds from now to the host's next deadline (a delayed send, a\n"
    "migration's timeout), as the last turn that went idle left it: 0.0 when\n"
    "it is due, None when there is none. The synchronous driver's run()\n"
    "sleeps for it; with a SimulatedClock, which arms no timer, increment\n"
    "the clock by it to move straight to the next thing that happens.";

static const char ta_python_doc_host_add_image[] =
    "add_image(image, /)\n"
    "--\n"
    "\n"
    "Run an image as a new segment of the host, and return the segment.\n"
    "\n"
    "`image` is any buffer: Compiled.image, or the bytes of a .tai file; the\n"
    "host copies it during the call. Each call makes one segment, even for\n"
    "the same bytes. A chart whose name the host already runs gets a new\n"
    "current version here: new spawns run it, the running actors stay on the\n"
    "old one until Host.migrate moves them, and a segment left with no\n"
    "actors and only older versions is freed.";

static const char ta_python_doc_host_chart[] =
    "chart(name, /)\n"
    "--\n"
    "\n"
    "The current version of the chart `name`, as a ChartRef for spawn.\n"
    "\n"
    "Spawning goes through a ChartRef rather than a name because a host can\n"
    "run several versions of a chart at once (one per add_image of an image\n"
    "that has it): the reference pins the version that was current when you\n"
    "asked. Ask again after add_image of a new version, so new actors start\n"
    "on it; actors already running stay where they are until Host.migrate.\n"
    "Raises TinyactorsError when no image added has a chart of that name.";

static const char ta_python_doc_host_actor_by_name[] =
    "actor_by_name(name, /)\n"
    "--\n"
    "\n"
    "The session of the live actor spawned with `name`, newest segment first;\n"
    "None when there is none.\n"
    "\n"
    "Names are how an application finds its long-lived actors again without\n"
    "keeping their session ids: spawn(ref, name=\"orders\") registers one, and\n"
    "Host.boot registers each actor a <system> manifest declares under its\n"
    "name. Use it to terminate or inspect such an actor, or to give it an\n"
    "input by session; to send it an event you can also skip the lookup and\n"
    "send to the address tinyactors://<systemID>/@<name>, as charts do.";

static const char ta_python_doc_host_spawn[] =
    "spawn(ref, name=None, params=None, traceparent=None)\n"
    "--\n"
    "\n"
    "Start an actor of the chart `ref` (a ChartRef from Host.chart) and\n"
    "return its session id, an int unique in the host.\n"
    "\n"
    "This is how your program creates actors one at a time: one per order,\n"
    "per user, per device. `name` registers it so it can be found again\n"
    "(Host.actor_by_name, the address tinyactors://<systemID>/@<name>);\n"
    "`params` (a dict) sets its <datamodel>'s data of the same ids, the way\n"
    "an <invoke>'s params do; `traceparent` (W3C text) makes its first steps\n"
    "part of your caller's trace. The actor enters its initial states in the\n"
    "next turn. To start the actors an image declares, use Host.boot.";

static const char ta_python_doc_host_send[] =
    "send(target, event, data=None, *, traceparent=None, origin=None)\n"
    "--\n"
    "\n"
    "Give an actor the external event `event` with optional `data` (a value\n"
    "of the value table, as a Python object). `target` is its session (an\n"
    "int), or a local address (a str, tinyactors://<this systemID>/#_scxml_<n>\n"
    "or .../@<name>), which the host resolves.\n"
    "\n"
    "Applied at the call: the event waits in the actor's mailbox and runs in\n"
    "the next turn; a hibernated actor's wake handler is called first.\n"
    "`traceparent` continues your caller's trace. `origin` is the sender's\n"
    "address when the event comes from another system (what its remote I/O\n"
    "processor handed over as send.origin): it becomes _event.origin, a\n"
    "chart's reply goes back there, and _event.origintype is SCXML's. Refused\n"
    "with TinyactorsError when the target does not exist (TA_ERROR_STALE),\n"
    "is another system's address (TA_ERROR_INVALID) or the host is full.";

static const char ta_python_doc_host_send_many[] =
    "send_many(items, /)\n"
    "--\n"
    "\n"
    "Give events to actors in order, one call for many: each item is\n"
    "(target, event[, data[, traceparent]]), as send takes them.\n"
    "\n"
    "Use it when events arrive in batches (a queue consumer, a replay, a\n"
    "load test): one call into the host for the batch instead of one per\n"
    "event. Returns how many were accepted, a prefix of `items`: on a refusal\n"
    "(a full host) the rest were not sent, so run the host to make room and\n"
    "send the rest again.";

static const char ta_python_doc_host_terminate[] =
    "terminate(session, /)\n"
    "--\n"
    "\n"
    "End the actor `session` from outside: it leaves its states (their\n"
    "<onexit> runs), its invocations are cancelled, and it is gone after the\n"
    "next turn.\n"
    "\n"
    "An actor whose chart reaches a top-level <final> ends by itself; use\n"
    "terminate when your program decides an actor's work is over (a user\n"
    "deleted, an order cancelled from an admin tool) or to clean up after a\n"
    "boot that failed part way.";

static const char ta_python_doc_host_boot[] =
    "boot(segment, /)\n"
    "--\n"
    "\n"
    "Spawn the boot actors of a segment add_image returned: the image's\n"
    "<system><actor> entries (compile_system), or its default boot chart\n"
    "(the first source given to compile_charts), each under its name, so\n"
    "actor_by_name finds it. Use it to start an application from its image alone; spawn\n"
    "starts one actor of a chart you name. A boot that fails part way leaves\n"
    "the actors already spawned; a second boot spawns them all again.";

static const char ta_python_doc_host_metrics[] =
    "metrics()\n"
    "--\n"
    "\n"
    "The host's counters now, as OTLP protobuf bytes (an\n"
    "ExportMetricsServiceRequest, for a collector's /v1/metrics).\n"
    "\n"
    "Telemetry has two directions: traces and logs are pushed to your\n"
    "telemetry handler as the host records them, and metrics are pulled by\n"
    "you, on your schedule. Call it when your exporter scrapes or on a timer,\n"
    "and post the bytes as you post the handler's. host_stats answers with\n"
    "the counters as Python values instead. Raises TinyactorsError\n"
    "(TA_ERROR_STATE) when the config has no telemetry.";

static const char ta_python_doc_host_sweep[] =
    "sweep(max_scan, limit, /)\n"
    "--\n"
    "\n"
    "The sessions to hibernate now: at most `limit` actors that did not run\n"
    "since the previous sweep and are at rest (the CLOCK sweep), looking at\n"
    "most at `max_scan` slots.\n"
    "\n"
    "Hibernation keeps a large population in a small memory: most actors\n"
    "wait most of the time. The cycle is yours to run: when host_stats shows\n"
    "more actors resident than you want, sweep, hibernate() each session it\n"
    "returns and store the blobs; when an event comes for one, the wake\n"
    "handler asks for it and Host.wake brings it back. Calling sweep\n"
    "regularly is what makes \"did not run since the previous sweep\" mean\n"
    "cold.";

static const char ta_python_doc_host_hibernate[] =
    "hibernate(session, /)\n"
    "--\n"
    "\n"
    "Take the actor `session` out of memory and return its blob (bytes) for\n"
    "you to store, e.g. in a database keyed by session.\n"
    "\n"
    "The host keeps its record: events and timers for it still arrive, and\n"
    "the wake handler asks for it back. Raises TinyactorsError\n"
    "(TA_ERROR_STATE) when it is not at a stable point or runs an\n"
    "invocation; it stays resident then. A blob wakes only in the host that\n"
    "wrote it.";

static const char ta_python_doc_host_wake[] =
    "wake(session, blob, /)\n"
    "--\n"
    "\n"
    "Bring the hibernated actor `session` back from its `blob`, usually from\n"
    "the wake handler. Its waiting events run in the next turn. You may call\n"
    "it later than the handler; the actor's events wait until then.";

static const char ta_python_doc_host_snapshot[] =
    "snapshot()\n"
    "--\n"
    "\n"
    "Copy the whole host into bytes: every segment's memory, both\n"
    "datamodels' state and the images.\n"
    "\n"
    "What it is for: a copy of the whole application, every chart version\n"
    "at once. Restore it (Host.restore) to rehearse a deploy (add the new\n"
    "image, migrate, run the clock ahead, look), to give a developer the\n"
    "host as it was when something went wrong, to start each test from one\n"
    "prepared world, or to take a host out of the process for a while. It\n"
    "is valid for the same build of the library only: never a backup.\n"
    "\n"
    "The host must be quiescent: not from a handler, with no typed method's\n"
    "reply or other effect pending (run the host first), and not while a\n"
    "migration runs (TinyactorsError, TA_ERROR_STATE). The hibernated\n"
    "actors' blobs are not in it: they stay in your store. The services you\n"
    "run are not in it either; Host.restore starts them again.";

static const char ta_python_doc_host_restore[] =
    "restore(snapshot, /, config, io_processors=None, *, log=None, clock=None, telemetry=None, wake=None, migrated=None, services=None, migration_done=None)\n"
    "--\n"
    "\n"
    "A new Host running a snapshot of Host.snapshot.\n"
    "\n"
    "Pass the config the snapshot was taken with (its system id must match)\n"
    "and the handlers Host() takes. Its clock goes on from the snapshot's\n"
    "time. At its first run each invocation of a service that was running\n"
    "reaches your services= handler again, with invocation.restored true:\n"
    "pass your real handlers to go on, stand-ins for a copy that must not\n"
    "reach the real services, or leave a type out and its invokers get\n"
    "error.communication. A copy that hibernates actors needs its own copy\n"
    "of the store: it writes blobs too.";

static const char ta_python_doc_host_migrate[] =
    "migrate(*, chart=None, session=None, wake=False, timeout=0)\n"
    "--\n"
    "\n"
    "Start a migration: move running actors to the current version of their\n"
    "chart, in the next runs. Returns the migration's id (an int).\n"
    "\n"
    "Use it after add_image of a new version: all charts, the one named\n"
    "`chart`, or one `session`. Each actor moves at a stable point, keeping\n"
    "its session id, its states (matched by id) and its timers, and gets\n"
    "error.platform.upgrade with its old variables first. A busy actor is\n"
    "retried; a hibernated one waits until something wakes it, or, with\n"
    "`wake`, is asked for through the wake handler (with this migration's\n"
    "id). `timeout` (seconds of the host's clock, 0 for none) ends the\n"
    "migration anyway.\n"
    "\n"
    "The host's migration_done handler gets its end, with result\n"
    "{\"migration\", \"migrated\", \"failed\", \"pending\": [session, ...]}; the\n"
    "migrated handler sees each actor that moved.";

static const char ta_python_doc_host_run_until_idle[] =
    "run_until_idle()\n"
    "--\n"
    "\n"
    "Turn until no actor is runnable at the current time: the synchronous\n"
    "driver's step.\n"
    "\n"
    "Use it in scripts, tests and programs that run the host between other\n"
    "work: give inputs, then run_until_idle() to let the actors react and\n"
    "your handlers run. Handlers run inside it; a handler's exception, or a\n"
    "failure of the host, is raised after the turn it happened in. Timers due\n"
    "later stay pending: run() sleeps until them, and with a SimulatedClock,\n"
    "increment() moves to them.";

static const char ta_python_doc_host_run[] =
    "run()\n"
    "--\n"
    "\n"
    "Turn, and sleep to each timer, until the host is idle with no timer\n"
    "left.\n"
    "\n"
    "The synchronous driver for a program that is the host, such as a worker\n"
    "whose actors only answer their own timers and the events your handlers\n"
    "add. A program that also serves other I/O uses asyncio instead\n"
    "(`async with Host(...)`), where the host turns on the event loop. Ctrl-C\n"
    "raises KeyboardInterrupt between turns.";

static const char ta_python_doc_host_idle[] =
    "idle()\n"
    "--\n"
    "\n"
    "asyncio: a future that completes when the host is idle.\n"
    "\n"
    "Inside `async with Host(...)` the host turns on the event loop by itself,\n"
    "after each input and at each timer, beside your other tasks (a web\n"
    "server, a queue consumer). Await idle() when you need the effects of\n"
    "your inputs before going on: in a test, or before reading an answer a\n"
    "typed method's handler stored.";

static const char ta_python_doc_host_close[] =
    "close()\n"
    "--\n"
    "\n"
    "Free the host: the last traces go to the telemetry handler, then the\n"
    "memory is released. Pending typed-method handlers are dropped, running\n"
    "services dropped without stop. `with` and `async with` call it on exit;\n"
    "the collector as a fallback.";

static const char ta_python_doc_host_enter[] =
    "Enter the synchronous driver: returns the host; refuses an async def handler (use async with).";

static const char ta_python_doc_host_exit[] =
    "Leave the synchronous driver: closes the host.";

static const char ta_python_doc_host_aenter[] =
    "Enter the asyncio driver: the host turns on the running event loop.";

static const char ta_python_doc_host_aexit[] =
    "Leave the asyncio driver: awaits the handlers' tasks, then closes the host.";

/* How every typed method works, the second half of each one's docstring. */
#define TA_PYTHON_DOC_TYPED_TAIL                                                      \
    "\n"                                                                              \
    "How it works, as for every typed method (Host.actor_state, host_stats,\n"        \
    "trace_watch, ...): pass the params as keywords; core checks them. The call\n"   \
    "returns None at once and handler(host, result) runs at a later drain with\n"    \
    "the result as Python values, so a script drives the host once more\n"           \
    "(run_until_idle()) and asyncio code awaits host.idle(). An error goes to\n"     \
    "on_error(host, error), given second or as on_error=, as a TinyactorsError\n"    \
    "whose code, reason and path say what was wrong; without on_error the driver\n"  \
    "raises it after the turn. A closed or full host refuses at the call."

static const char ta_python_doc_typed_actor_state[] =
    "actor_state(handler, on_error=None, /, *, session)\n"
    "--\n"
    "\n"
    "Ask which states the actor `session` is in now.\n"
    "\n"
    "The result is {\"value\": <state value>} in xstate's shape: an atomic state\n"
    "is its id (\"idle\"), a compound state {<its id>: <its active child>}, a\n"
    "parallel state {<its id>: {<region id>: ..., ...}}. Use it to show\n"
    "an actor in a debugger or dashboard, or in a test to assert where a chart\n"
    "went after some events. Inside a chart, In(state) asks the same\n"
    "question.\n"
    TA_PYTHON_DOC_TYPED_TAIL;

static const char ta_python_doc_typed_actor_context[] =
    "actor_context(handler, on_error=None, /, *, session)\n"
    "--\n"
    "\n"
    "Ask for the actor `session`'s variables: each <data> of its chart.\n"
    "\n"
    "The result is {\"variables\": {<data id>: <value>, ...}}, in the order the\n"
    "chart declares them, as values of the value table (what cannot be shown\n"
    "that way is None or UNDEFINED). Use it to inspect an actor's data from\n"
    "outside, in a debugger, an admin page or a test, without the chart\n"
    "sending it anywhere. A hibernated actor is refused (reason \"state\"):\n"
    "wake it first.\n"
    TA_PYTHON_DOC_TYPED_TAIL;

static const char ta_python_doc_typed_actor_list[] =
    "actor_list(handler, on_error=None, /, *, limit=256, cursor=0)\n"
    "--\n"
    "\n"
    "Ask for the sessions of the host, a page at a time.\n"
    "\n"
    "The result is {\"sessions\": [session, ...], \"cursor\": int or None}: pass\n"
    "the cursor back for the next page; None means every session was listed.\n"
    "`limit` is 1 to 512. Use it to enumerate actors for an admin page, a\n"
    "health check or a cleanup, then ask actor_get, actor_state or\n"
    "actor_context about each one.\n"
    TA_PYTHON_DOC_TYPED_TAIL;

static const char ta_python_doc_typed_actor_get[] =
    "actor_get(handler, on_error=None, /, *, session)\n"
    "--\n"
    "\n"
    "Ask for the summary of the actor `session`: where it runs and how busy\n"
    "it is.\n"
    "\n"
    "The result is {\"session\", \"segment\", \"chart\" (its name), \"parent\" (the\n"
    "session that invoked it, or None for an actor you spawned), \"mailbox\"\n"
    "(events waiting), \"invocations\" (services it runs)}. Use it to find\n"
    "which version an actor runs (its segment), to walk from a child to its\n"
    "parent, or to spot an actor whose mailbox grows.\n"
    TA_PYTHON_DOC_TYPED_TAIL;

static const char ta_python_doc_typed_host_stats[] =
    "host_stats(handler, on_error=None, /)\n"
    "--\n"
    "\n"
    "Ask for the host's counters, and each chart's.\n"
    "\n"
    "The result is {\"host\": {...}, \"charts\": [...]}. The host's: segments,\n"
    "actors, hibernated, runnable, migrations, regionBytes (core's memory for\n"
    "the live segments), delivered, routed, migrated, retired, fuelClock.\n"
    "Each chart's, summed over its versions: name, actors, hibernated,\n"
    "spawned, ended, turns, macrosteps, fuel, errorsExecution,\n"
    "errorsCommunication, errorsPlatform, faults. Use it for a dashboard or a\n"
    "health check, to decide when to sweep and hibernate, or in a test to\n"
    "count what happened; Host.metrics gives the host's counters as OTLP for\n"
    "a collector.\n"
    TA_PYTHON_DOC_TYPED_TAIL;

static const char ta_python_doc_typed_host_config[] =
    "host_config(handler, on_error=None, /)\n"
    "--\n"
    "\n"
    "Ask for the configuration the host runs with: the config you gave\n"
    "Host(...), with every default filled in.\n"
    "\n"
    "The result is a dict in the config's own shape (systemID, system,\n"
    "ioProcessors, migrateQuantum, telemetry), so a host made from it answers\n"
    "the same. Use it to log or show what a host was really given, or to make\n"
    "a second host like the first (a copy for Host.restore needs the same\n"
    "system id).\n"
    TA_PYTHON_DOC_TYPED_TAIL;

static const char ta_python_doc_typed_trace_configure[] =
    "trace_configure(handler, on_error=None, /, *, detail=\"summary\", values=False, sample=1, records=True, rules=[])\n"
    "--\n"
    "\n"
    "Set what the host traces, for every segment and the ones added later.\n"
    "\n"
    "`detail` is \"summary\", \"decisions\", \"full\" or \"off\"; `values` puts data\n"
    "values in the spans; `sample` (0 to 1) is the share of new traces kept;\n"
    "`records` False keeps traceparents flowing but writes no spans; `rules`\n"
    "(at most 32) pick details per chart, actor, event or source (the core's requests\n"
    "documentation has their form). The result is {}. It replaces the last\n"
    "configuration; watches stay. Use it at startup to set the policy, and\n"
    "again to turn tracing up or down while the host runs; trace_watch is for\n"
    "a closer look for a while. Refused (reason \"state\") when the config has\n"
    "no telemetry.\n"
    TA_PYTHON_DOC_TYPED_TAIL;

static const char ta_python_doc_typed_trace_watch[] =
    "trace_watch(handler, on_error=None, /, *, rules, seconds=900)\n"
    "--\n"
    "\n"
    "Trace something in more detail for a while: rules put in front of the\n"
    "configured ones until `seconds` of the host's clock pass (None: until\n"
    "trace_unwatch).\n"
    "\n"
    "Each rule picks actors (by chart, actor name, event, session or source) and a detail, e.g.\n"
    "{\"chart\": \"checkout\", \"sample\": \"sample\", \"detail\": \"full\"}. The result\n"
    "is {\"id\": int}, the watch's id for trace_unwatch. Use it to debug one\n"
    "chart or one kind of event in production without raising the detail for\n"
    "the whole host.\n"
    TA_PYTHON_DOC_TYPED_TAIL;

static const char ta_python_doc_typed_trace_unwatch[] =
    "trace_unwatch(handler, on_error=None, /, *, id)\n"
    "--\n"
    "\n"
    "End the watch `id` (from trace_watch's result) before its time is up.\n"
    "\n"
    "The result is {}. A watch that already ended, or never was, is an error\n"
    "with reason \"stale\". Use it when the closer look is done, so tracing\n"
    "falls back to the configured policy.\n"
    TA_PYTHON_DOC_TYPED_TAIL;

static const char ta_python_doc_typed_trace_end[] =
    "trace_end(handler, on_error=None, /)\n"
    "--\n"
    "\n"
    "End every open span now and hand what was recorded to the telemetry\n"
    "handler, before the reply.\n"
    "\n"
    "The spans end at the last clock the host saw, marked incomplete. The\n"
    "result is {}. close() calls it for you; call it yourself to end the\n"
    "spans at a moment you choose, when your shutdown does other work before\n"
    "it closes the host. Refused (reason \"state\") when the config has no\n"
    "telemetry.\n"
    TA_PYTHON_DOC_TYPED_TAIL;

static const char ta_python_doc_typed_trace_spans_decode[] =
    "trace_spans_decode(handler, on_error=None, /, *, traces)\n"
    "--\n"
    "\n"
    "Read the spans out of OTLP trace bytes, such as the data the telemetry\n"
    "handler got with signal \"traces\".\n"
    "\n"
    "The result is {\"spans\": [...]}, each span a dict: trace and span ids as\n"
    "hex, parent (None for a root), name, kind, start and end (nanoseconds\n"
    "since the epoch), status, statusMessage, attributes and events. It reads\n"
    "only the bytes, not the host. Use it in tests and tools that check what\n"
    "an actor's steps traced without a collector or an OTLP library.\n"
    TA_PYTHON_DOC_TYPED_TAIL;

static const char ta_python_doc_chart_ref[] =
    "A chart in a host, from Host.chart(name): the segment that runs its\n"
    "current version and its index in that segment's image. Pass it to\n"
    "Host.spawn. It names one version: after add_image of a new one, ask\n"
    "Host.chart again.";

static const char ta_python_doc_chart_ref_segment[] =
    "The segment (one actor system of the host) that runs this version:\n"
    "the int add_image returned for its image.";

static const char ta_python_doc_chart_ref_chart[] =
    "The chart's index in that segment's image; with the segment, it names\n"
    "one version of one chart.";

static const char ta_python_doc_send[] =
    "A <send> to one of your I/O processors, given to its handler.\n"
    "\n"
    "Read what you need (event, data, target), do the I/O, and answer the\n"
    "sending actor with answer(), or report a failure with fail(). A plain\n"
    "function handler gets a thin view: its fields are read on first use and\n"
    "valid until the host acknowledges the batch, so answer before returning\n"
    "or copy what you need. An async def handler gets a record whose\n"
    "answer() and fail() work later. A send to an invoked service (to\n"
    "#_<invokeid>) reaches the service's receive(host, send) as the same\n"
    "view, its type None.";

static const char ta_python_doc_send_session[] =
    "The sending actor's session id (int).";

static const char ta_python_doc_send_sendid[] =
    "The <send>'s sendid (str or None): answers and failures carry it back.";

static const char ta_python_doc_send_event[] =
    "The event's name (str).";

static const char ta_python_doc_send_target[] =
    "The <send>'s target (str or None): an address for your transport.";

static const char ta_python_doc_send_data[] =
    "The event's data as a Python value: for a \"python\" chart the value\n"
    "itself (no copy), for quickjs decoded on first read.";

static const char ta_python_doc_send_traceparent[] =
    "The sending step's trace as W3C text, None when untraced: put it in\n"
    "your outgoing request's traceparent header to continue the trace.";

static const char ta_python_doc_send_type[] =
    "The I/O processor's type from the config (None for a send to an invoked service).";

static const char ta_python_doc_send_origin[] =
    "The sending actor's address, tinyactors://<systemID>/#_scxml_<session>: what\n"
    "a remote I/O processor hands the other system, whose Host.send takes it\n"
    "as origin so a reply finds its way back.";

static const char ta_python_doc_send_answer[] =
    "answer(event, data=None, *, traceparent=send.traceparent, origin=None,\n"
    "       raw=None)\n"
    "--\n"
    "\n"
    "Give the sending actor an event in reply, with this send's sendid and\n"
    "the processor's type as origintype. It keeps the send's trace; pass\n"
    "traceparent=None to start a new one. `origin` (a str) is where the\n"
    "actor replies to, _event.origin, and `raw` (a str) the message as the\n"
    "transport carried it, _event.raw: an HTTP processor passes the request\n"
    "it read. Answering after the batch's ack needs a record (an async def\n"
    "handler).";

static const char ta_python_doc_send_fail[] =
    "fail()\n"
    "--\n"
    "\n"
    "Report that the send could not be delivered: the sending actor gets\n"
    "error.communication with this send's sendid. A handler that raises\n"
    "before answering is failed this way for you.";

static const char ta_python_doc_entry[] =
    "A <log> line of an actor, given to the log handler.\n"
    "\n"
    "A chart's <log label=\"...\" expr=\"...\"/> is how it reports what it does;\n"
    "the handler decides where that goes: Python's logging module, stdout, a\n"
    "test's list. Read session, label and text inside the handler: it is a\n"
    "thin view, valid until the batch's ack.";

static const char ta_python_doc_entry_session[] =
    "The logging actor's session id (int).";

static const char ta_python_doc_entry_label[] =
    "The <log>'s label (str, empty when none).";

static const char ta_python_doc_entry_text[] =
    "The <log>'s evaluated expression, as text.";

static const char ta_python_doc_migrated[] =
    "An actor that a migration moved to its chart's current version, given\n"
    "to the migrated handler.\n"
    "\n"
    "Use the handler to follow a deploy actor by actor (a progress count, a\n"
    "log of who moved) or to update what your application keeps about an\n"
    "actor's version; migration_done reports the whole migration's end. A\n"
    "thin view, valid until the batch's ack (see Host.migrate).";

static const char ta_python_doc_migrated_session[] =
    "The actor's session id (int); it is the same after the move.";

static const char ta_python_doc_migrated_segment[] =
    "The segment it now runs in: the current version's.";

static const char ta_python_doc_migrated_chart[] =
    "The name of the chart it runs, the same before and after.";

static const char ta_python_doc_migrated_migration[] =
    "The migration (Host.migrate's return value) that moved it.";

static const char ta_python_doc_invocation[] =
    "A service your application runs, as one actor invoked it.\n"
    "\n"
    "An <invoke type=T> in a state starts it: the services= handler for T\n"
    "gets this record and returns the service object, which may have\n"
    "receive(host, send) for the sends to it (to #_<invokeid>, or\n"
    "autoforwarded events) and stop(host) for when the invoker leaves the\n"
    "state. Keep the record to answer: send_back() gives the invoker an\n"
    "event, done() ends the service with done.invoke.<invokeid>. Once it is\n"
    "done, cancelled or failed, answers raise TinyactorsError\n"
    "(TA_ERROR_STALE). A service that fails to start or\n"
    "whose receive raises gives its invoker error.communication.";

static const char ta_python_doc_invocation_session[] =
    "The invoking actor's session id (int).";

static const char ta_python_doc_invocation_invokeid[] =
    "The <invoke>'s id: events from the service carry it as _event.invokeid.";

static const char ta_python_doc_invocation_type[] =
    "The <invoke>'s type: the key of the services= handler that started it.";

static const char ta_python_doc_invocation_src[] =
    "The <invoke>'s src (str or None), e.g. a URL the service should use.";

static const char ta_python_doc_invocation_data[] =
    "The <invoke>'s params (a dict) or content, as a Python value.";

static const char ta_python_doc_invocation_traceparent[] =
    "The invoking step's trace as W3C text, or None: answers keep it by default.";

static const char ta_python_doc_invocation_running[] =
    "True until the service is done, cancelled, failed or the host closed.";

static const char ta_python_doc_invocation_restored[] =
    "True when the host was restored (Host.restore) with this invocation\n"
    "running: the handler starts the service again, with the same invokeid,\n"
    "src and params. A copy that must not reach the real service registers a\n"
    "stand-in for the type.";

static const char ta_python_doc_invocation_send_back[] =
    "send_back(event, data=None, *, traceparent=invocation.traceparent)\n"
    "--\n"
    "\n"
    "Give the invoking actor an event from the service, with its invokeid\n"
    "set. Call it whenever the service has something to say, as long as it\n"
    "runs.";

static const char ta_python_doc_invocation_done[] =
    "done(data=None, *, traceparent=invocation.traceparent)\n"
    "--\n"
    "\n"
    "End the service: the invoker gets done.invoke.<invokeid> with `data`,\n"
    "and the service is dropped (not stopped). Later answers raise.";

static const char ta_python_doc_clock[] =
    "SimulatedClock()\n"
    "--\n"
    "\n"
    "A clock that moves only when told, for tests and simulations.\n"
    "\n"
    "Pass it as Host(..., clock=clock). Delayed sends (<send delay>), watch\n"
    "ends and migration timeouts fall due when increment() moves past them,\n"
    "so a test of a 30-day timeout takes no time and gives the same result\n"
    "every run; a host on a simulated clock never sleeps. To jump to the next\n"
    "thing that happens: clock.increment(host.next_deadline). One clock can\n"
    "drive several hosts, which then share its time.";

static const char ta_python_doc_clock_increment[] =
    "increment(seconds, /)\n"
    "--\n"
    "\n"
    "Move the clock on by `seconds` (an int or a float, as next_deadline\n"
    "gives) and turn every host made with it until idle, so the timers due\n"
    "by then run.";

static const char ta_python_doc_clock_now[] =
    "The simulated time in seconds (a float) since the clock was made (it starts at 0.0).";

static const char ta_python_doc_address[] =
    "Address(text, /)\n"
    "--\n"
    "\n"
    "An actor's address as a value (CBOR tag 32), kept apart from plain text.\n"
    "\n"
    "Charts and other programs pass addresses as data (a reply-to, a\n"
    "subscriber list); when such a value comes from an ECMAScript chart or\n"
    "another process it arrives as an Address, and an Address you put in data\n"
    "reaches them as one. To send to an address, give its text to Host.send.\n"
    "`text` must be an exact str of valid UTF-8; it is not parsed or checked\n"
    "here. Use .text or str(value) to read it. Equality and hashing use the\n"
    "text, among Address values only; subclasses are refused.";

static const char ta_python_doc_address_text[] =
    "The address text (read-only).";

static const char ta_python_doc_undefined[] =
    "The value table's undefined, distinct from None (null): what an\n"
    "ECMAScript chart's undefined becomes in Python, and how to send one.\n"
    "There is one, tinyactors.UNDEFINED; compare with `is`.";

static const char ta_python_doc_event[] =
    "_event in a \"python\" chart: the event being processed.\n"
    "\n"
    "Conditions and actions read it: cond=\"_event.data['amount'] > 100\",\n"
    "expr=\"_event.origin\" to reply. It is immutable and stays bound until the\n"
    "next event, as SCXML requires. Fields that do not apply are None.";

static const char ta_python_doc_event_name[] =
    "The event's name, e.g. \"order.paid\" or \"done.invoke.payment\".";

static const char ta_python_doc_event_type[] =
    "\"platform\" (errors, done events), \"internal\" (<raise>) or \"external\".";

static const char ta_python_doc_event_sendid[] =
    "The id of the <send> that produced it (str), or None: what an answer or\n"
    "an error.communication carries back to match the request it is about.";

static const char ta_python_doc_event_origin[] =
    "The sender's address, or None: <send target=\"...\" targetexpr=\"_event.origin\">\n"
    "replies to it, in this host or in another system.";

static const char ta_python_doc_event_origintype[] =
    "The I/O processor type of the origin, or None: the type a reply uses\n"
    "to go back the way the event came.";

static const char ta_python_doc_event_invokeid[] =
    "The invokeid when the event came from an invoked child or service, or None.";

static const char ta_python_doc_event_data[] =
    "The event's data as a Python value, or None.";

static const char ta_python_doc_event_raw[] =
    "The message as the I/O processor read it (an HTTP request, say), or\n"
    "None: for a chart that needs more than the event's data.";

static const char ta_python_doc_in[] =
    "In(state, /)\n"
    "--\n"
    "\n"
    "True when the actor's state configuration includes the state `state`\n"
    "(its id): SCXML's In() predicate, for conditions in a \"python\" chart.";

#endif
