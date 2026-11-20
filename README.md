# tinyactors for Python

Some programs keep track of many things that each move through steps of
their own: orders that are placed, paid and shipped, devices that connect
and drop, sign-ups that wait days for a confirmation. Written as plain
code, each of these becomes rows in a table, flags, timers and callbacks
spread through the program. Written as a statechart, each is one SCXML
document that says which states the thing can be in and which events move
it between them.

tinyactors runs those statecharts as actors. A **host** holds the actors,
up to millions of them, and runs each one's chart when events reach it.
The host does no I/O. When an actor sends a message out, logs a line or
starts a service, the host calls an object or a function you gave it,
and that does the work. Charts can be written in Python (the `"python"`
datamodel) or in ECMAScript (run by quickjs), and the values actors
exchange with your program are ordinary Python objects.

This package binds the C library: each function and method corresponds
to calls of the library ([docs/api-map.md](docs/api-map.md) lists them).
A few items are built on those calls for applications: `Actor`, the
stores, `compile_dir`, `ask` and `Spawner`.
[GLOSSARY.md](../GLOSSARY.md) defines the terms these pages use.

This page has three parts. The first is the API an application uses:
`Host`, `Actor`, I/O processors, services, the store, and what to do
when an actor is not there. **Operating a host** has the calls for
whoever runs and watches it: images and versions, telemetry and
tracing, hibernation by hand, migration, snapshots, memory. **Wire
format** has what crosses a process boundary: other systems, addresses,
the value table's encoding and decoded traces. An application can be
written with the first part alone; [examples/chat](examples/chat/) is
one.

## Installing

    pip install tinyactors

The wheels are built for CPython 3.13 and later, using the stable ABI.
Wheels exist for these platforms:

- macOS 11 or later, on Apple silicon and Intel;
- Linux on x86-64 and arm64, with glibc 2.17 or later or musl 1.1 or
  later (one wheel serves both).

There is no source distribution, so other platforms and the free-threaded
interpreters (3.13t, 3.14t) cannot install it yet. Development builds of
`main` are published as prereleases; once a release is out,
`pip install --pre tinyactors` installs the newest of them.

## A first program

The chart below counts greetings. Each `hello` event increments a
variable, logs a line, and sends the count out to the application.

```python
import tinyactors

CHART = """<scxml xmlns="http://www.w3.org/2005/07/scxml" version="1.0"
               datamodel="python" name="greeter" initial="idle">
  <datamodel><data id="greeted" expr="0"/></datamodel>
  <state id="idle">
    <transition event="hello">
      <assign location="greeted" expr="greeted + 1"/>
      <log label="greeter" expr="f'hello, {_event.data[&quot;who&quot;]} ({greeted})'"/>
      <send type="app" target="out" event="greeted">
        <content expr="{'count': greeted}"/>
      </send>
    </transition>
  </state>
</scxml>"""

class App:                                # an I/O processor: the actors' output
    def receive(self, send):
        print("app got", send.event, send.data)
        send.answer("thanks")             # an event back to the sender

with tinyactors.compile_charts([("greeter.scxml", CHART)]) as compiled:
    host = tinyactors.Host(compiled, system="demo", io_processors={"app": App()},
                           log=lambda entry: print("log:", entry.text))
with host:
    greeter = host.spawn("greeter")
    greeter.send("hello", {"who": "world"})
    host.run_until_idle()
    host.actor_context(lambda result: print(result["variables"]), session=greeter.id)
    host.run_until_idle()                 # the reply's handler runs at a later drain
```

It prints:

```
log: hello, world (1)
app got greeted {'count': 1}
{'greeted': 1}
```

Step by step:

1. **`compile_charts`** turns the SCXML text into an **image**, the
   compiled form a host runs (`compile_dir(path)` compiles a directory of
   charts). The host copies the image when it is created, so the compile
   result can be closed as soon as `Host` returns.
2. **`Host`** runs the image, under the system id `demo`, the authority
   of its actors' addresses. `io_processors` maps each type of `<send>`
   your program handles to an object whose `receive(send)` gets it.
3. **`spawn`** starts an actor of the chart and returns it as an
   **`Actor`**, whose `id` is its **session**, the number that identifies
   it from then on. **`send`** puts the `hello` event in its mailbox.
4. **Nothing has run yet.** `spawn` and `send` only record their input.
   `run_until_idle()` runs the actors until none has anything left to do,
   and calls your handlers along the way: `log` for the `<log>`, and
   `App.receive` for the `<send>`.
5. **`actor_context`** asks the host for the actor's variables. It returns
   `None` at once. The answer arrives as a reply, and the reply goes to
   its handler during a later run, which is why the program calls
   `run_until_idle()` a second time. Under asyncio, `await
   greeter.context()` gives it.

## How a host runs

A host never runs on its own. Your program drives it, and each run
follows the same cycle:

- **Inputs** are the calls that give the host work: `spawn`, `send`,
  `send_many`, `terminate`, `boot`, `wake`, and an `Actor`'s `send` and
  `terminate`. Each is checked and applied
  when you call it, and raises at once when it is refused. The actors
  react later.
- **A turn** is one pass of the driver. It runs the actors that have
  work, for a bounded amount of computation, and collects what they ask
  of the outside world: sends to I/O processors, `<log>` lines,
  invocations, replies to typed methods. These are the host's
  **effects**.
- **Handlers** receive the effects in batches. When every handler in a
  batch has returned, the host **acknowledges** the batch and moves on. A
  handler gets no host argument: each view and record has `.host`. A
  handler may call the host again: an input it makes applies at once, and
  the effects that input causes arrive in a later batch.

A plain function handler receives a **thin view**, which reads each field
from the host the first time you access it. A view is valid until its
batch is acknowledged. Reading a field afterwards raises
`TinyactorsError` with `status == "TA_ERROR_STALE"`, so read what you need
inside the handler. What you have already read is a normal Python object
and stays valid. An `async def` handler receives a **record** instead:
every field is copied before the handler is scheduled, so it can answer
after the batch is gone.

When a handler raises, the host still finishes the turn and acknowledges
the batch, and then the driver raises the first exception, unless the
host has an `on_handler_error(error, effect)` hook (below, "Actors"). A host is
used from the thread that created it; calls from other threads are
refused.

### Compiling charts

`compile_dir(path, *, resolver=None)` compiles every `*.scxml` file
under a directory into one image, each document named by its path under
the directory, in diagnostics and for a `src` between documents (sorted:
the first is the default boot chart). A `src` (a `.py`
helper a chart reads with `<script src>`, a chart it invokes) is read
from under the directory, relative to the document that names it, and a
path that leaves the directory, symbolic links included, is refused
(`ResolveError`, so `CompileError`). Compile once, and give the result to
any number of hosts:

```python
with tinyactors.compile_dir("charts") as compiled:
    hosts = [tinyactors.Host(compiled, system="shop-%d" % i) for i in range(4)]
```

A chart is spawned by its `name` attribute, not its file:
`host.spawn("order")` starts an actor of the current version of the
chart whose `<scxml name="order">`. Adding images
later, versions and boot actors are operator calls (below, "Operating a
host").

### Drivers and time

| Driver | What it does | Use it for |
|---|---|---|
| `host.run_until_idle()` | turns until no actor can run at the current time | scripts, tests, programs that run the host between other work |
| `SimulatedClock.increment(seconds)` | moves a simulated clock forward and turns every host made with it until idle | tests and simulations: a 30-day timeout takes no time |
| `await host.idle()` | under `async with`, the host turns on the event loop by itself; `idle()` completes when it is idle | programs that also serve other I/O |

Time in the API is in seconds, as an `int` or a `float`. Inside the host
time is counted in ticks, at the config's `system.ticksPerSecond`, and
the binding converts. `host.next_deadline` is the time in seconds until
the host's next timer, or `None` when there is none. With a
`SimulatedClock`, `clock.increment(host.next_deadline)` jumps straight to
the next thing that happens.

Under asyncio, an input made outside a turn schedules one, so you only
await `idle()` when you need the effects before going on:

```python
import asyncio

class App:
    async def receive(self, send):        # async def: runs as a task, gets a record
        await asyncio.sleep(0.01)         # your I/O
        send.answer("thanks")

async def main():
    with tinyactors.compile_charts([("greeter.scxml", CHART)]) as compiled:
        image = compiled.image
    async with tinyactors.Host(image, system="demo", io_processors={"app": App()}) as host:
        host.spawn("greeter").send("hello", {"who": "world"})
        await host.idle()

asyncio.run(main())
```

An `async def` handler needs the asyncio driver. Entering a host that has
one with a plain `with` raises `TypeError` naming the handler.

### Typed methods

Questions about the host and its actors are **typed methods**:
`actor_state`, `actor_context`, `actor_list`, `actor_get`, `host_stats`
and `host_config` here, and the tracing methods `trace_*` an operator
uses (below, "Telemetry"). The params are keywords, passed to the host
as given, and the host checks them.

Under asyncio, await one: `state = await host.actor_state(session=s)`,
or the `Actor`'s shorthand `await actor.state()`,
gives the result as Python values, and an error raises as the
`TinyactorsError` subclass of its code (`StaleError` for a session that
does not exist), whose `code`, `reason` and `path` say what was wrong. A
method still pending when the host closes never completes.

Each also takes `(handler, on_error=None, /, **params)`, which returns
`None`: `handler(result)` runs when the reply is handled, in a
later turn, and an error goes to `on_error(error)`. Without
`on_error`, the driver raises it after the turn. This is the form for the
synchronous driver, which has no event loop to complete an awaitable on:
there, a method given no handler raises `TypeError`. There is no
synchronous answer: a host answers questions in the same cycle as
everything else.

The core's [requests.md](../tinyactors-core/docs/requests.md) describes
each method's params and result. Its `actor.send` has no typed method,
because `host.send` does the same.

Two answers say what an actor will do next. `actor_get`'s `timers` lists
its pending delayed sends (each `<send delay>` not yet due and not
cancelled), earliest first, each `{"sendid", "event", "due"}`: `due` is
in seconds on the host's clock (a `float`; on a `SimulatedClock`, the
clock `SimulatedClock.now` reads), `sendid` is `None` for a send without an id. The
list holds at most 32, and `timersPending` counts them all.
`actor_state`'s `history` gives, for each history state the actor has
recorded, its parent state as it would be entered again, in the shape of
`value`; a history state records when its parent is exited, so it is
absent before that. `actor_state` and `actor_context` refuse a
hibernated actor (reason `"state"`): wake it first.

```python
host.actor_get(lambda actor: print(actor["timers"]), session=session)
# [{'sendid': None, 'event': 'tick', 'due': 1.5}, {'sendid': 'sla', 'event': 'breach', 'due': 60.0}]
host.actor_state(lambda state: print(state["history"]), session=session)
# {'h': {'work': {'a': 'a2'}}}   after work was left from a2: h, a deep history, returns there
```

### Leaving a host

`with`, `async with` and `close()` free the host. Before that, the host
ends the trace spans still open and hands the last telemetry to your
handler (below, "Telemetry"). Handlers of typed methods still waiting for
a reply are dropped without being called, and running services are
dropped without `stop`. The garbage collector frees a host you did not
close, but without the last telemetry, so close it.

## Actors

`host.spawn(chart, name=None, params=None, traceparent=None)` returns an
**`Actor`**, a small immutable value: `id` (its session), `address`
(`tinyactors://<system>/#_scxml_<id>`), `host`, and the calls that talk to
it: `send(event, data=None, *, sendid=None, traceparent=None)`,
`terminate()`, and under asyncio `await ask(event, data=None)` (below), `await state()`, `context()`, `get()`
(the typed methods) and `await done`, its donedata when it reaches a
top-level final state. Two `Actor` values of one session are equal and
hash alike; the host keeps no table of them, so one is made wherever the
API gives one. `host.actor(which)` gives the live actor of a session, a
name it was spawned with, or a local address, and raises `StaleError`
when none answers; `await host.actors(chart=None)` lists the live ones
(of one chart). A `Send` names its sending actor as `send.sender`.

**`await actor.ask(event, data=None)`** is a request and its reply: it
sends the event with an `origin` of the host's own (an address in the
system id `tinyactors-ask`, which the binding reserves), the chart
answers with `<send targetexpr="_event.origin" event="...">`, and the
reply comes back as an `(event, data)` pair, never reaching your
transport. It has no timeout of its own: `await asyncio.wait_for(
actor.ask("price"), 5)` cancels the ask, and an ask pending when the
host closes never completes.

```xml
<transition event="price">
  <send targetexpr="_event.origin" event="quote"><content expr="{'total': total}"/></send>
</transition>
```

```python
event, data = await order.ask("price")    # ("quote", {"total": 42})
```

The host tells you when a top-level actor ends:

- **`on_done(actor, donedata)`**: it reached a top-level final state;
  `donedata` is its `<donedata>`, as a value.
- **`on_error(actor, error)`**: it faulted, and the host reclaimed it;
  `error` is a `TinyactorsError`.

And when one of your handlers raises: **`on_handler_error(error,
effect)`** gets the exception and what the handler was handling (the
`Send`, `LogEntry`, `Invocation` or `Migrated`, or `None` for a
telemetry, store, migration or typed-method handler). A send whose
processor raised is failed for it either way (`error.communication` for
its sender). Without the hook, the driver raises the first exception
after the turn. A `KeyboardInterrupt` or `SystemExit` always reaches the
driver.

## Charts in Python

A `datamodel="python"` chart gets a class of its own, and each actor of
the chart is an instance of it.

- **The variables** are the instance's slots: the chart's `<data>` ids,
  and the names a `<foreach>` (`item`, `index`) or a `<script>` outside
  its functions assigns. The chart declares these names too, as SCXML
  4.6 and a module's top level do. Each is `None` until set.
- **The system variables** `_event`, `_sessionid`, `_name` and
  `_ioprocessors` are read-only. `_event` is an `Event` and stays bound to
  the last event until the next one. `In(state)` is SCXML's `In()`.
- **Expressions** are compiled once per chart, by a restrictive rewrite,
  into functions of the instance.

The rewrite allows:

- *in expressions:* literals, arithmetic, comparisons, boolean operations,
  conditional expressions, indexing and slicing, comprehensions and
  generator expressions, f-strings, calls to the chart's methods and to
  the allowed builtins, and attribute access except to dunder names;
- *in statements* (in `<script>` and in `.py` files a `src=` names):
  assignment, plain or augmented, to a variable or to its items or
  attributes; `if`, `for`, `while`, `pass` and expression statements; and
  `def` at the top level of a script, which makes a method of the chart;
- *these builtins:* `len`, `min`, `max`, `sum`, `abs`, `round`, `sorted`,
  `reversed`, `enumerate`, `zip`, `range`, `any`, `all`, `isinstance`,
  `hasattr`, `str`, `int`, `float`, `bool`, `list`, `dict`, `tuple`,
  `set`, `frozenset`, `map`, `filter`.

It refuses `import`, `global`, `nonlocal`, `lambda`, `await`, `yield`, the
walrus (`:=`), `del`, `try`, `raise`, `with` and `class`.

An undeclared name, a refused construct, or an assignment to a system
variable raises `error.execution` in the chart when the expression is
evaluated, as SCXML requires. In a `.py` file, the same mistakes are
compile errors at their line.

**Write logic in `.py` files.** Inside a chart an expression is XML text,
so `<` is written `&lt;`, `&` is `&amp;`, and a quote inside a quoted
attribute is `&quot;`. Anything longer than a comparison reads better as
a method in a `.py` file that the chart names with `<script src>`. The
file is plain Python, checked by the same rewrite at compile time:

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

The compiler reads `rules.py` through the resolver you give
`compile_charts`. A resolver is any callable `resolver(base, source) ->
str`, for example
`lambda base, source: pathlib.Path("charts", source).read_text()`. It is
called at compile time only, for each static `src=` that is not among
the sources. It refuses a file by raising
`ResolveError(message, line=None, column=None)`, which the compiler
reports at that line and column.

**Values do not leak between actors.** What crosses to another actor
(`<send>` data, `<donedata>`, invoke params, data from the application)
is copied through the value table (below, "Values"), so actors share no
mutable value.

## I/O processors

An I/O processor is how an actor's output reaches your program: one
object per external system your actors talk to (a database, the open
websockets), each with a `receive(send)` method (it may be `async def`)
that switches on `send.event`. `Host(..., io_processors={type:
processor})` gives them to the host; the config's `ioProcessors` are
made from it, in its order, each `{type, location}` with the location the
processor's optional `location` attribute. A plain function is refused:
a processor is an object. Types are plain names (`chat:db`). Every
`<send type=...>` of that type reaches `receive` once, as a `Send`:
`send.event`, `send.data`, `send.target`, `send.sendid`, `send.session`,
`send.sender` (the sending `Actor`), `send.host` and `send.traceparent`.

```python
class Database:
    location = "postgres://orders"        # optional: _ioprocessors' location

    def receive(self, send):
        if send.event == "save":
            rows.append(send.data)
            send.answer("saved")
        else:
            send.fail()
```

**Spawning from a chart** is an I/O processor too: `tinyactors.Spawner()`,
under a type you name (`io_processors={"spawn": tinyactors.Spawner()}`).
A chart sends it `{chart, name, params}` (`<send type="spawn"
event="spawn" id="s1"><content expr="{'chart': 'room', 'name': room,
'params': {...}}"/></send>`); the new actor runs the chart's current
version, and the sender gets `spawned` with `{name, address}` and the
send's sendid, or `error.communication` when the host refuses the spawn
(an unknown chart, data of another shape). It spawns; it does not look
names up. A name has one owner, the collection actor that decides which
actors of a kind exist (a workspace for its rooms), which records the
spawns it asked for, so it never asks twice for a name; spawning a new
version under an existing name is how an application updates
(`docs/guide/missing-actors.md`, "One owner per name" and "Updating
through names").

The processor does the I/O and then does one of two things:

- **`send.answer(event, data=None, *, traceparent=send.traceparent)`**
  gives the sending actor an event in reply, carrying the send's
  `sendid`. It also takes `origin` and `raw`, for a processor that speaks
  a protocol (below, "Wire format").
- **`send.fail()`** reports that the send could not be delivered. The
  sending actor gets `error.communication`.

Both return `True` when delivered and `False` when the sending actor
ended meanwhile: an answer to an actor that is gone is dropped, as
XState drops an event sent to a stopped actor, and nothing raises. Any
other refusal (a second answer, a thin view read after its batch, a
closed host) raises `TinyactorsError`.

A processor may also do neither, when the send needs no reply. The
sending actor waits while `receive` runs, so a plain method's `answer()`
or `fail()` reaches it before its next step, as SCXML has a `<send>`
behave; the other actors go on meanwhile. So hand the message to your
transport and return: SCXML needs the send dispatched, not delivered,
and a processor that waits for delivery (a request's response, a
broker's confirmation) holds its sender that long. An `async def`
`receive` answers after the sending actor has gone on. One that raises
before answering has its send failed for it, and the exception goes to
`on_handler_error`, or the driver raises it after the turn.

Read the fields inside `receive`: a plain method gets a thin view
(above, "How a host runs"). To answer later, write it as `async def`.
Its record answers at any time while the host lives.

## Sending to an actor that is not there

A `<send>` to a name no live actor has is common in an application with
many actors: a room nobody has opened yet, a user who is offline, an
order that ended. What should happen is application logic, chosen per
kind of actor, and the library offers no policy of its own. There are
three ways, often mixed in one application:

- **The actor never ends, and hibernates.** Core wakes a hibernated actor
  when an event comes for its name, so with `store=` and `resident=` an
  idle actor costs a blob in the store and a slot in the host.
- **The application routes it.** The chart sends to an I/O processor,
  which looks the actor up (or has one made) and forwards the event, or
  does something else when there is none.
- **The sender handles `error.communication`.** A send to a local name
  that no actor has fails at once, with the send's `sendid`.

Names of actors are part of their addresses, so use only characters
that need no percent-encoding there: letters, digits, `-`, `.`, `_` and
`~`, for example `room.<workspace>.<name>`.
[docs/guide/missing-actors.md](docs/guide/missing-actors.md) shows each
way with code, and the chat example (below, "Example applications") uses
all three.

## Invoked services

A chart can `<invoke>` a service that your program runs, such as a
connection, a subprocess or a long computation. Register a handler per
type: `Host(..., services={type: handler})`. When an invocation starts,
the handler receives an `Invocation` and returns the service, any
object. The host keeps the service until the invocation ends.

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

    def receive(self, send):                  # a <send target="#_counter">, or autoforwarded
        self.count += 1
        if send.event == "finish":
            self.invocation.done({"total": self.count})   # done.invoke.counter
        else:
            self.invocation.send_back("pong", {"n": self.count})

    def stop(self):                           # the invoker cancelled it (left the state, ended)
        ...

host = tinyactors.Host(compiled, system="shop", services={"urn:example:counter": Counter})
```

- **The invocation** has `session` (the invoker), `invokeid`, `type`,
  `src`, `data` (the params, or the content), `traceparent`, `running`
  and `restored` (below, "Snapshots"), and `host`. `send_back(event, data=None)`
  gives the invoker an event with its `invokeid` set. `done(data=None)`
  ends the service with `done.invoke.<invokeid>`. The invocation is a
  record, so you can keep it and answer while the service runs. After
  `done` or a cancel, these calls raise `TinyactorsError` with
  `TA_ERROR_STALE`.
- **The service** may have `receive(send)`, which gets each send to
  it as a thin view (`type` is `None`), and `stop()`, which gets the
  cancel. Both are optional. A service that ended with `done` is neither
  stopped nor kept, so a handler may call `done` and return at once.
- **A failure** of any kind gives the invoker `error.communication`: no
  handler for the type, a handler that raises, a `receive` that raises
  (the service crashed and is dropped, not stopped), or no handler for a
  restored invocation. Each later send to that invocation gets one too,
  until the invoker leaves the state. The invoker gets none when it
  cancelled the invocation in the same run. The driver raises the
  exception after the turn; a restored invocation with no handler raises
  nothing, since declining to start it again was your choice.
- **Leaving the host** drops the running services without calling `stop`.

An invocation uses one of the core's requests; `system.maxRequests` is
where the pool starts, and a segment grows it when it runs out.

## Hibernation

Most actors spend most of their time waiting. Hibernation lets a host
keep millions of actors while only some of them are in memory: the rest
are blobs in a **store**, and the host loads one back when it has work.
The host does no I/O; the store is an object of yours, or one of the two
this package ships.

```python
store = tinyactors.SQLiteStore("actors.db")      # one row per hibernated actor
host = tinyactors.Host(compiled, system="shop", store=store, resident=1000)
```

- **`store`** is any object with `save(session, blob)`, `load(session)
  -> bytes` and `delete(session)`. `tinyactors.SQLiteStore(path)` keeps
  the blobs in one table of an SQLite file (`close()` it after the host);
  `tinyactors.MemoryStore()` in a dict, for tests.
- **`resident`** is the most actors kept in memory. After a turn that
  left more, the host takes the coldest past the bound (the CLOCK sweep:
  actors that did not run since the previous sweep), hibernates them and
  saves their blobs. An actor that cannot hibernate now stays resident:
  one not at rest (events waiting, in a macrostep, an invoked child, a
  running invocation), or a `"python"` actor holding a value outside the
  value table (a `set`, a `tuple`, an object). A pending delayed send
  stays in the host and wakes its actor when it falls due.
- **When an event comes for a hibernated actor** (or its delayed send
  falls due), the host loads its blob from the store, wakes it, and
  deletes the blob; the events wait in its mailbox meanwhile. Terminating
  a hibernated actor wakes it to end it (its `<onexit>` runs), so its blob is loaded and deleted the same way. A `save` or `load` that raises
  goes to `on_handler_error`, or the driver raises it after the turn; the
  actor stays where it was (resident for a `save`, hibernated for a
  `load`). Without a store, an event for a hibernated actor waits until
  you call `host.wake`.

A blob belongs to the host that made it: each `Host` draws random bits
from `os.urandom` when it is created, so a blob of another host, even for
a session of the same number running the same image, is refused
(`TA_ERROR_INVALID`). A host made by `Host.restore` keeps the snapshot's
identity and wakes the blobs its original hibernated. Only the latest
blob of a session wakes it: one of an earlier hibernation (a store that
fell behind) is refused with `TA_ERROR_STALE`.

The capacity this needs, and the calls under the store for an
application with a policy of its own, are below ("Hibernation by hand").

## Errors

| You see | When | What to do |
|---|---|---|
| `CompileError` | `compile_charts` or `compile_system` found an error | read `error.diagnostics`, each with `file`, `line`, `column`, `severity` and `message`; nothing was compiled |
| `StaleError`, `StateError`, `InvalidError`, `UnsupportedError` (each a `TinyactorsError`) | the host refused a call, a thin view was read after its batch, or a typed method's reply was an error: an actor or handle that is gone, a call the host cannot make now, an argument it refuses, a value it does not take | catch the class you handle; `error.status` is the core's name for the status (`"TA_ERROR_STALE"`, ...), and a typed method's error also has `code`, `reason` and `path` |
| `TinyactorsError` | any other refusal, or a run that faulted | read `error.status` |
| `MemoryError` naming a pool | the host ran out of a pool its config sized | the host stays usable, and the next run goes on where this one stopped; size the config (below, "Sizing the config") |
| your handler's exception | a handler raised | the host's `on_handler_error(error, effect)` gets it; without one, the driver raises the first one after the turn |
| `error.execution`, `error.communication` | inside a chart | handled by the chart, as SCXML defines; when the chart's code failed, `error.execution`'s `_event.data` is `{"reason", "message"}`: `"exception"` with the exception's type and text (`"NameError: name 'x' is not defined"`), `"memory"` for a `MemoryError` (on quickjs, past its memory limit, 1 GiB), and on quickjs `"fuel"` past its `maxOps`; see the core's documentation |

`CompileError` and `ResolveError` are subclasses of `TinyactorsError`.
For example, `host.send(12345, "hello")` to a session that does not exist
raises `StaleError`, whose `status` is `"TA_ERROR_STALE"`.

## Values

The values that cross between your program, the actors and other
processes are those of the value table: `int` (from -2\*\*64 to 2\*\*64-1), `float`, `str`,
`bytes`, `list`, `dict` with `str` keys, `True`, `False`, `None`,
`tinyactors.UNDEFINED` (ECMAScript's undefined, distinct from `None`) and
`tinyactors.Address` (an actor's address kept apart from plain text).
Only exact built-in types are accepted: a subclass of `dict` or `list`,
a tuple or a set is refused.

Inside one process, values cross as Python objects, with no encoding.
Data you give a `"python"` chart is copied (lists and dicts are copied,
immutable values shared); data for an ECMAScript chart is converted for
quickjs. `send.data` from a `"python"` chart is the value itself.
Values that leave the process are encoded by your program (below,
"Wire format"). [docs/values.md](docs/values.md) has the details.

## Limits

- **Charts are trusted code, and the rewrite is not a sandbox.** It keeps
  a chart inside SCXML's scope rules and nothing more. A chart in Python
  runs in your interpreter, cannot be preempted in the middle of an
  expression, and nothing limits its memory. quickjs, by contrast, has an
  operation limit (1,000,000 interrupt checks an evaluation) and its own
  allocator, limited to 1 GiB for the host's engine, every actor
  together: an evaluation that allocates past it ends in
  `error.execution` with reason `"memory"`, and the process goes on.
- **Python values lie outside the host's allocator.** The host, its
  tables and its scratch memory are allocated with `PyMem_RawMalloc`. The
  values of a Python chart's sessions and payloads are Python objects on
  Python's heap.
- **Nesting is limited to 64 levels.** That stops cycles, but not a value
  that shares subtrees (`a = [a, a]` sixty times): another reason charts
  are trusted code.
- **A `"python"` actor hibernates and migrates** only while its
  variables, and the data of its pending delayed sends, are values of the
  value table.

## Example applications

[examples/chat](examples/chat/) is a chat server: workspaces, rooms and
connections as actors, charts with `.py` helpers, one I/O processor per
external system (the sockets, an SQLite database, direct messages) and a
`Spawner`, with hibernated workspaces and rooms in a store. Its README
says where each of the three ways of "Sending to an actor that is not
there" appears. `mise run test` runs its tests.

## Operating a host

The calls in this part are for whoever runs a host and watches it (an
operator, a deploy script, a capacity plan), not for the application's
logic: adding images and versions, telemetry and tracing, hibernation by
hand, migration, snapshots, and sizing the memory. An application that
only spawns actors and handles their effects needs none of them.

### Images, segments and versions

`Host(charts, ...)` runs each image it is given as a **segment**, in
order, and `add_image` runs another one later and returns the segment's
number. A segment is one actor system with its own memory.
A host can run many segments at once.

When an image has a chart whose name the host already runs, that image
holds a new **version** of the chart. `host.chart(name)` returns a
`ChartRef` for the current version, and `host.spawn(name)` spawns on it.
New actors start on it, and running
actors stay on their version until a migration moves them (below,
"Migration").

`compile_system` compiles a `<system>` manifest instead of a list of
charts. The manifest names the charts (`<chart name src>`) and the actors
the application starts with (`<actor name chart>`). `host.boot(segment)`
spawns those actors under their names, and `host.actor(name)` finds one
again. For an image from `compile_charts`, `boot` spawns the
first chart compiled.

### Telemetry

With `"telemetry"` in its config, the host traces its actors and writes
OTLP requests (protobuf): spans of every macrostep, `<log>` lines and
failed actions as log records, and counters as metrics. Traces and logs
are pushed: the host hands each request to
`telemetry=handler(signal, data)` as `bytes`, with `signal`
`"traces"` or `"logs"`, and your handler posts them. Metrics are pulled:
call `host.metrics()` when your exporter wants them.

```python
import urllib.request

def export(signal, data):                 # OTLP/HTTP, to a collector or otel-tui
    request = urllib.request.Request("http://localhost:4318/v1/" + signal, data,
                                     {"Content-Type": "application/x-protobuf"})
    urllib.request.urlopen(request, timeout=2).close()

config = {"telemetry": {"flushInterval": 5_000_000_000,       # host clock: ns by default
                        "resource": {"service.name": "shop"}}}
with tinyactors.Host(compiled, system="shop", config=config, telemetry=export) as host:
    ...
    export("metrics", host.metrics())
```

The handler runs inside the turn, so a slow post holds up the host (and,
under asyncio, the event loop). Give it a timeout, or write it as
`async def`, which runs as a task; `async with` waits for the tasks of
the last requests. Retrying and limiting what waits are up to you. When
a handler raises, its request is lost and the driver raises the first
exception of the turn after it.

`flushInterval` is in the host's clock (`system.ticksPerSecond`). Without
it, a request is ready only when the staging buffer is full, or when the
host closes.

**Application spans above actors.** A `traceparent` given to `send` or
`spawn` makes the macrostep it starts a child of your span, in your
trace. Sends between actors keep the trace, and an I/O processor receives
the sending step's context in `send.traceparent`. With the OpenTelemetry
SDK:

```python
from opentelemetry import propagate, trace

tracer = trace.get_tracer("shop")

class Warehouse:                          # continue the actor's trace in the application
    def receive(self, send):
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

A tracing backend then shows one trace. `POST /orders` is at the top, with
the actors' macrosteps and microsteps below it. `warehouse.reserve` sits
below the step that sent to the warehouse, and the macrostep its answer
started sits below that.

**Each host starts its own trace IDs.** A trace the host starts itself
takes its ID from a generator seeded by `system.traceSeed`. When the
config gives none, the extension draws a random seed for each new host
(`os.urandom`), so two processes, or one process and its restart, do not
collide at the collector. To reproduce a test's traces, set the seed:
`{"system": {"traceSeed": 7}}` gives the same IDs every time.
`Host.restore` keeps the generators of the snapshot.

**Choosing what is traced.** The typed methods `trace_*` (above,
"Typed methods", for how they answer) control tracing while the host
runs. `trace_configure` sets the policy, for
example sampling. `trace_watch` adds rules in front of it for a while, on
the host's clock: with tracing sampled down, it can trace one chart in
full for a minute. `trace_unwatch` ends a watch early, and `trace_end`
ends every open span now (`close()` calls it). To read the spans back, in tests or in a collector of
your own, use `tinyactors.decode_traces(data)` (below, "Decoding
traces").

```python
host.trace_configure(print, sample=0.01)                 # 1% of new traces
host.trace_watch(lambda result: print("watch", result["id"]),
                 rules=[{"chart": "checkout", "sample": "sample", "detail": "full"}],
                 seconds=60)                             # every checkout, for a minute
```

### Hibernation by hand

The host's calls under the store are there for an application that
decides itself what to hibernate and when:
**`host.sweep(max_scan, limit) -> [session, ...]`** returns at most
`limit` actors that have not run since the previous sweep and can be
hibernated now (over at most `max_scan` slots);
**`host.hibernate(session) -> bytes`** takes one out of memory and
returns its blob (save it in the host's store, which loads it when an
event comes); **`host.wake(session, blob)`** brings one back.

**Plan the capacity up front.** A hibernated actor keeps its slot and its
session's record in the host (about 64 B), so `system.maxActors` counts
every actor. Instance and string memory are needed for the resident ones
only. Left to grow, a segment grows the pool that ran out, by a quarter of its
region when that is at most 16 times the pool, and its instance memory
with `maxActors`,
hibernated actors included. For `R` actors resident out of `N`:

```python
config = {"system": {
    "maxActors": N + room, "maxEvents": 4096,
    "instanceBytes": 2048 * (R + room), "stringBytes": 16384 + 64 * (R + room)}}
```

`tools/stress.py` (`mise run stress`) is this pattern at scale: two
million actors, `resident=1000`, the rest in SQLite (below,
"Performance").

### Migration

A new version of a chart (an image added with a chart of the same `name`)
runs the actors spawned from then on. `host.migrate` moves the running
actors to it, each at a stable point, during the next runs. A moved
actor keeps its session id, its state configuration (states are matched
by id) and its delayed sends. It first gets `error.platform.upgrade`,
whose data is the old version's `<datamodel>` bindings, and the new
version takes what it keeps from there:

```xml
<transition event="error.platform.upgrade">
  <assign location="n" expr="_event.data['n']"/>
</transition>
```

- **`host.migrate(*, chart=None, session=None, wake=False, timeout=0) ->
  migration`** starts a migration of every chart, of one `chart` by name,
  or of one `session`, and returns its id. `timeout` is in seconds; 0
  means none. A `chart` the host does not have raises `TinyactorsError`
  with `TA_ERROR_INVALID`, and a `session` that ended `TA_ERROR_STALE`: a
  migration that could move nothing is refused rather than reported done.
- **`migration_done=handler(result)`** gets the end of each
  migration, with `{"migration", "migrated", "failed": [{"session",
  "reason"}, ...], "pending": [session, ...]}`: `failed` the actors it
  could not move and why (core's status, by name), `pending` those its
  timeout left behind. It is a handler of the host, not of the call, so it
  also gets the end of a migration that a restored host goes on with.
- **`migrated=handler(migrated)`** gets each actor moved, as a thin
  `Migrated` view of `session`, `segment`, `chart` (its name) and
  `migration`.

**When a migration ends.** A migration ends when every actor it looks at
has moved, or at its `timeout`. A hibernated actor moves once it is
woken. With `wake=True` the migration asks for it, and the host loads it
from its store; it asks again if it finds the actor hibernated after a
wake. Without `wake`, or without a store, a hibernated actor that nobody
wakes holds the migration until its timeout, or forever without one, and
`migration_done` is never called: give such a migration a `timeout`.
Each run costs the migration one quantum of actors (`migrateQuantum` in
the config), however many it waits for.

**Actors that do not move.** An actor that cannot be written out stays
on its version and is in the result's `failed`, with the reason
`"TA_ERROR_UNSUPPORTED"` (`host.hibernate` of it names the variable). On
the `"python"` datamodel, that is
one whose variables or pending delayed sends hold a value outside the
value table. An actor that is running an invocation waits until the
invocation ends.

**With most actors hibernated, the bound holds.** A migration with
`wake=True` asks for every hibernated actor early. With `resident=R` the
host loads them while fewer than `R` actors are in memory and keeps the
rest for later turns, hibernating the moved actors at rest to make room,
so the population moves `R` at a time. An event's actor is loaded at
once, and the migration takes that one too.

`tools/migration.py` (`mise run migration`) is this pattern at scale, on
SQLite (below, "Performance").

### Snapshots

`host.snapshot() -> bytes` copies the whole host into one blob: every
segment's memory (actors, queues, timers, the hibernated actors'
records), both datamodels' sessions and payloads, the images, the system
id and the host's time. `tinyactors.Host.restore(snapshot, charts, system=..., ...)`
creates a new host running it. Pass what `Host(...)` takes: the system
the snapshot was taken with (its id is checked), its config (its I/O
processors and tick rate are not in the snapshot), and the handlers.
`charts` are the images you run now: those the snapshot holds are kept,
and the others are added as new versions, ready for `host.migrate`. Every session id
stays the same, and the host's clock goes on from the snapshot's time,
whatever clock reads it: a new `SimulatedClock`, or the real clock in
another process or on another machine.

```python
blob = host.snapshot()                    # quiescent: not from a handler
host.close()
...
host = tinyactors.Host.restore(blob, compiled, system="shop", config=config, log=log, store=store)
```

- **A snapshot is valid only for the same build of the library.** It holds
  raw memory, so it is not a backup; durability is your database's job,
  and hibernation is how a running process keeps actors out of memory.
  A snapshot is a copy of the whole application, every chart version at
  once. Use one to rehearse a deploy: add the new image, migrate, run the
  clock ahead, look. Use one to capture a host when something goes wrong
  and step through it on the same build, to start every test from one
  prepared world, or to take a host out of the process for a while.
- **Invoked services are started again.** The services you run are not
  in the snapshot. The restored host's first run announces each
  invocation that was running to the `services=` handler you pass to
  `Host.restore`, with `invocation.restored` true and the same
  `invokeid`, `src` and params. Pass your real handlers to go on, or
  stand-ins for a copy that must not reach the real services. A type with
  no handler gives its invokers `error.communication`.
- **Hibernated actors stay in your store.** The snapshot holds their
  records, not their blobs, and the restored host loads them from the
  store it is given, as before. A copy that hibernates actors needs its own copy of
  the store, since it writes blobs too.
- **The host must be quiescent.** A snapshot is refused with
  `TA_ERROR_STATE` from a handler, while an effect or a typed method's
  reply has not been handled (run the host first), and while a migration
  runs. It is refused with `TA_ERROR_UNSUPPORTED` when a `"python"`
  session or payload holds a value outside the value table. The host
  goes on either way.
- **Values are written through the value table**, so two variables of
  one session that share a list no longer share it after a restore.

### Memory

Measured with `mise run measure`: 10,000 actors of one chart with three
`<data>`, resident and idle, every pool sized for them and tracing off;
then again after one event to each actor, which keeps its `_event` until
the next one (2026-10-15; x86_64 Linux, 4 CPUs, CPython 3.13.16 with the
GIL):

| datamodel | resident per actor | traced per actor | core's region per actor | resident after one event | region after one event |
|---|---|---|---|---|---|
| `"python"` | 387 B | 188 B | 268 B | 571 B | 268 B |
| quickjs | 1066 B | — | 268 B | 1500 B | 268 B |

"Traced" is what tracemalloc sees: the sessions' Python objects and the
binding's rows (including an 8 B array of the measuring harness). After
an event, a `"python"` session keeps one more object, its `_event`
(about 100 B). Its name, invoke id, origin and origin type are strings
that the chart or the host holds once for every session. With the pools
sized, the region does not grow after the events. Left at its default
`maxEvents` (256), quickjs's region grows its event pool as the actors'
last events fill it: an event and a trace context per actor.

#### Sizing the config

Most of a host's memory is what its config reserves, not what its actors
use: the core sets its pools up front and does not allocate after that.
The first configs of this package's benchmarks reserved four to sixteen
times what they used. Sized for the work, the 100,000 resident actors of
the snapshot benchmark went from 362 MB of region to 47 MB, and the
services benchmark's 200,000 actors from 269 MB to 50 MB. The core's
[host-config.md](../tinyactors-core/docs/host-config.md) lists every key;
these matter most:

- **`maxActors`:** every actor, resident or hibernated (a hibernated one
  costs about 58 B of region), with some room.
- **`instanceBytes` and `stringBytes`:** for the resident actors only, at
  what their charts use (128 B and 16 B per actor for the benchmarks'
  charts). Left out, they are derived from `maxActors`, which counts the
  hibernated actors too.
- **`maxEvents`:** the events in flight at once. On quickjs, add one per
  resident actor, because quickjs keeps each actor's last event in the
  core's event pool.
- **`maxRequests`:** the service invocations that run at once.
- **`tracing`:** on by default. It takes a journal of 128 KiB per
  segment and a trace context (48 B) per `maxEvents` slot; turn it off
  where nobody exports traces.
- **Size the pools; do not let them grow.** A segment that runs out of
  a pool grows that pool (doubling it, or up to 16 times it to add a
  quarter of its region), copying the whole region into a new block, and
  never shrinks.

### Performance

Each benchmark below is a program in `tools/`, run with `mise run
<name>`, that checks its own results. Every run here was one run, on
2026-10-15, on x86_64 Linux with 4 CPUs and CPython 3.13.16 with the GIL,
with each config sized for its work ("Sizing the config").

**Hibernation** (`stress`): two million actors of a counter chart, at
most 1000 resident, the rest in SQLite. 200,000 events go to actors drawn
at random, so nearly every event wakes one from the store and the sweep
sends another back. Every actor's count matched the events it was sent,
and after every eviction at most 1000 actors were resident.

| datamodel | populate (spawn and hibernate 2,000,000) | 200,000 events (woken, swept back) | peak resident | core's region | SQLite file |
|---|---|---|---|---|---|
| `"python"` | 15.8 s (126,000 a second) | 14.7 s (13,600 a second; 199,869 wakes) | 164 MB (82 B per actor) | 115 MB (58 B per actor) | 355 MB |
| quickjs | 25.9 s (77,000 a second) | 17.3 s (11,500 a second; 199,869 wakes) | 165 MB (82 B per actor) | 115 MB (58 B per actor) | 409 MB |

**Migration** (`migration`): 2,000,000 actors of a counter chart on
SQLite, at most 1000 resident, moved to version 2 while 200,000 events
go to actors drawn at random. It checks every sampled actor's count and
version, that the migration left no actor behind, that version 1's
segment retired, and the resident bound after every eviction.

| datamodel | populate (2,000,000, 200,000 events) | migrate, live (200,000 events meanwhile) | woken from SQLite | peak resident | core's region (both versions, then after) | SQLite file |
|---|---|---|---|---|---|---|
| `"python"` | 59.9 s | 129.7 s (15,400 actors a second) | 2,218,199 | 473 MB (237 B per actor) | 230.9 MB, then 115.5 MB (58 B per actor) | 556 MB |
| quickjs | 71.1 s | 153.3 s (13,000 actors a second) | 2,218,199 | 475 MB (238 B per actor) | 230.9 MB, then 115.5 MB | 756 MB |

A run costs the migration one quantum of actors however many it waits
for, so its time grows with the population. At 500,000 actors on the
`"python"` datamodel it took 13.6 s, against 74.9 s when the migration
went over everything it waited for in every run. The peak is the migration's: both
versions' regions until version 1 retires, plus the migration's map of
the sessions it waits for (at most a quarter full after it is rebuilt).

**Snapshots** (`snapshot`): a deploy rehearsed on a copy of a live host.
The live host has its population on SQLite as in the hibernation
benchmark, 200,000 events, a delayed send for every resident actor, and
half of those paying through an invoked payment service that has not
answered. Its snapshot is written to a file, its store copied, and a copy
restored from the file with a stand-in payment service, which gets every
payment in flight again (`restored`, with its amount) and declines it.
The copy must snapshot to the same bytes before it runs. Then it adds
version 2 and migrates every actor, waking the hibernated ones a batch at
a time, with 200,000 events meanwhile, while the live host goes on with
200,000 events of its own. Both hosts' sampled actors must hold every
count, delayed send and payment: the copy's on version 2 and declined,
the live host's on version 1 and still paying. The copy must never reach
the real service. Both hosts ran in one process.

| datamodel | actors | resident | snapshot | snapshot time | restore time | migrating the copy | peak resident, both hosts | every actor right |
|---|---|---|---|---|---|---|---|---|
| `"python"` | 2,000,000 | 1000 | 116.1 MB (58 B per actor) | 0.14 s | 0.12 s | 145 s | 652 MB | yes |
| quickjs | 2,000,000 | 1000 | 116.2 MB | 0.14 s | 0.13 s | 172 s | 654 MB | yes |
| `"python"` | 100,000 | all | 56.6 MB (566 B per actor) | 0.66 s | 0.53 s | 22.3 s | 427 MB | yes |
| quickjs | 100,000 | all | 64.0 MB (640 B per actor) | 3.43 s | 1.29 s | 21.6 s | 684 MB | yes |

A snapshot is mostly the region, as the config sized it: before the
benchmark's config was sized for the work, the same 100,000 resident
actors made a 372 MB snapshot and a 1761 MB peak. Migrating 2,000,000
mostly hibernated actors is the store's work: each is woken, moved and
hibernated again (2,018,332 wakes, events included).

**Invoked services** (`services`): 200,000 actors, each with a counter
service, make 1,000,000 round trips (actor, service, actor). Then a third
finish with done data, a third are cancelled, and the rest keep their
service. It checks every pong and every done datum, that every cancelled
service was stopped exactly once and no other was, and that the host
holds only the services still running.

| datamodel | start 200,000 invocations | 1,000,000 round trips | end (done, cancel) | peak resident | core's region |
|---|---|---|---|---|---|
| `"python"` | 3.1 s (64,000 a second) | 38.3 s (26,000 a second) | 4.5 s | 408 MB (2041 B per actor and service) | 50 MB (251 B per actor) |
| quickjs | 5.0 s (40,000 a second) | 62.6 s (16,000 a second) | 5.9 s | 711 MB (3553 B per actor and service) | 62 MB (309 B per actor) |

## Wire format

What crosses a process boundary: a send to another system, an address
written as text, a value encoded for the wire, a traces request decoded
back. An application in one process needs none of it.

### Other systems

How systems talk to each other (threads and a queue, a WebSocket to a
browser, HTTP) is your program's choice. A chart addresses another system
with an ordinary SCXML target, `tinyactors://<systemID>/#_scxml_<n>` or
`tinyactors://<systemID>/@<name>`. The host delivers a target in its own
system itself. Every other target goes to the host's transport, a
handler you pass as `transport=`; it needs nothing in the config.

A send between systems is one message in two halves. The sending host
hands your transport the outgoing half as `send.message`, a dict of the
target, event, data, sendid, origin and traceparent. The receiving host
takes the same dict as its incoming half, `host.send(message)`. Your code
only moves the dict:

```python
links = {}                                   # system id -> a connection of yours

def transport(send):                         # the outgoing half
    link = links.get(send.target.system)
    if link is None:
        send.fail()                          # no connection: error.communication
    else:
        link.write(tinyactors.encode(send.message))

host = tinyactors.Host(compiled, system="a", transport=transport)

def on_frame(link, frame):                   # the incoming half, on the other side
    message = tinyactors.decode(frame)
    if message["origin"].system != link.system:   # trust is yours: who may claim what
        return
    try:
        host.send(message)                   # every field arrives, the sendid included
    except tinyactors.TinyactorsError:       # no such actor here: tell the sender
        link.write(tinyactors.encode({"failed": message["origin"], "sendid": message["sendid"]}))
```

A chart's reply to `_event.origin` goes back the same way. `send.target`
and `send.origin` are `Address` values (below, "Addresses").
`host.send(message)` ignores keys it does not know, so a newer peer can
add one, and takes a `sendid`, `origin` or `traceparent` of `UNDEFINED`
(what another binding's absent field decodes as) as absent, as `None`.

A failure is never part of a message. When the other side reports one,
and you decide to believe it, call `host.fail(address, sendid)`: the
sending actor gets `error.communication` with that sendid. A peer whose
reports you accept without checking can give your actors any error, so
take failures only from the connection the send went out on. A send
the transport accepted needs no answer, and inside the handler
`send.fail()` reports a failure directly.

The data is a Python value; encode the message for the wire as you like.
`tinyactors.encode` gives the value table's CBOR, which every binding
reads, `Address` values included. Without a transport, each send to
another system fails, and its sender gets `error.communication`.

**A processor that speaks a protocol.** An I/O processor that is a
protocol's endpoint (SCXML's BasicHTTP processor, say) passes what the
chart needs to answer on that protocol:
`send.answer(event, data, origin=..., raw=...)` sets the reply's
`_event.origin` (where the chart replies to, a `str`) and `_event.raw`
(the message as the transport read it, a `str`: the HTTP request).
`host.send(..., origin=...)` does the same for an event that is not an
answer.

### Addresses

An address is text, `tinyactors://<system>/#_scxml_<session>` or
`tinyactors://<system>/@<name>` (the core's
[protocol.md](../tinyactors-core/docs/protocol.md), "Addresses").
`tinyactors.Address(text)` holds one as a value, apart from plain text:
`.system`, `.session` and `.name` give its parts, read by the core's
parser, so neither you nor the binding parses an address.
`Address.from_name(system, name)` and `Address.from_session(system,
session)` write one. A name is percent-encoded: a chart that targets the
actor named `room/acme` writes the text `Address.from_name("chat",
"room/acme")` gives, `tinyactors://chat/@room%2Facme`, never the name as
it is. Names that use only letters, digits, `-`, `.`, `_` and `~` are
written as they are (above, "Sending to an actor that is not there").

### Encoding values

`tinyactors.encode(value) -> bytes` and `tinyactors.decode(data)` give
the value table's CBOR form, which every binding reads and writes, for
values that leave the process: a message for another system, a value
another program reads. `Address` is tag 32, `UNDEFINED` CBOR's
undefined. What they cannot read or write raises (`TypeError`,
`ValueError`, `OverflowError`, `UnicodeError`), never turned into
another value. Inside one process values cross as Python objects and
need neither.

### Decoding traces

`tinyactors.decode_traces(data)` decodes an OTLP traces request (what
the telemetry handler gets for `"traces"`, or any exporter's) into a
list of spans without a host: each span's ids, name, times, status,
attributes and events, and its `flags`, `links`, `resource` and
`scope`. Use it in tests and in a collector of your own. The typed
method `trace_spans_decode` reads the same bytes through a host.

## The API at a glance

Every item has a docstring: `help(tinyactors.Host.migrate)` says when and
how to use it.

**The application API**

| Item | What it does |
|---|---|
| `compile_dir(path, *, resolver=None) -> Compiled` | compiles every `*.scxml` under a directory, its `src` files read from under it |
| `compile_charts(sources, resolver=None) -> Compiled` | compiles `(name, text)` pairs into one image; raises `CompileError` |
| `Compiled.image`, `.diagnostics`, `.close()` | the image's bytes, copied at the first read; the warnings and notes; a context manager |
| `Host(charts, *, system, io_processors, config, clock, store, resident, log, telemetry, services, transport, on_done, on_error, on_handler_error, migrated, migration_done)` | a host running the images, with its processors and handlers; a context manager for `with` and `async with` |
| `system` | the host's system id |
| `spawn(chart, name=None, params=None, traceparent=None) -> Actor` | starts an actor of a chart (a name, or a `ChartRef`) |
| `actor(session, name or address) -> Actor`, `await actors(chart=None)` | a live actor; the live actors |
| `Actor.id`, `.address`, `.send(...)`, `.terminate()`, `await .ask(event, data)`, `await .state()`, `.context()`, `.get()`, `await .done` | an actor's handle |
| `Spawner()` | an I/O processor that spawns actors for charts |
| `send(target, event, data=None, *, sendid=None, traceparent=None, origin=None)` | gives an actor an event; `target` is a session, an `Actor` or a local address |
| `send_many([(target, event, data, traceparent), ...]) -> accepted` | many sends in one call; returns how many were accepted |
| `terminate(session)` | ends an actor |
| `run_until_idle()`, `await idle()`, `next_deadline` | the drivers (above, "Drivers and time") |
| `actor_state`, `actor_context`, `actor_list`, `actor_get`, `host_stats`, `host_config` | typed methods (above, "Typed methods") |
| `MemoryStore()`, `SQLiteStore(path)` | stores of hibernated actors' blobs, for `store=` |
| `close()`, `closed` | frees the host |
| `SimulatedClock()`, `.increment(seconds)`, `.now` | a clock for tests |
| `UNDEFINED` | the value table's undefined |
| `TinyactorsError`, `StaleError`, `StateError`, `InvalidError`, `UnsupportedError`, `CompileError`, `ResolveError`, `Diagnostic` | errors |

**Operating a host**

| Item | What it does |
|---|---|
| `add_image(image) -> segment`, `chart(name) -> ChartRef` | runs another image (a new version); the current version of a chart |
| `compile_system(manifest, resolver=None) -> Compiled`, `boot(segment)` | compiles a `<system>` manifest of charts and boot actors; spawns an image's boot actors |
| `telemetry=`, `metrics() -> bytes` | traces and logs pushed as OTLP; the host's counters as OTLP |
| `trace_configure`, `trace_watch`, `trace_unwatch`, `trace_end`, `trace_spans_decode` | typed methods for tracing |
| `sweep`, `hibernate`, `wake` | hibernation by hand |
| `migrate(...) -> migration`, `migrated=`, `migration_done=` | migration |
| `snapshot() -> bytes`, `Host.restore(snapshot, charts, system=..., ...)` | snapshots |

**Wire format**

| Item | What it does |
|---|---|
| `transport=`, `send.message`, `send(message)`, `fail(target, sendid=None)` | sends between systems: the outgoing half, the incoming half, a failure the other side reported |
| `send.answer(..., origin=, raw=)`, `send(..., origin=)` | a protocol's reply address and raw message |
| `Address(text)`, `.system`, `.session`, `.name`, `Address.from_name`, `Address.from_session` | addresses as values, read and written by the core's parser |
| `encode(value) -> bytes`, `decode(data)` | the value table's CBOR |
| `decode_traces(data)` | the spans of an OTLP traces request, without a host |

## Building from source

`mise run build` builds the extension for CPython 3.13 (abi3) and for
the free-threaded test interpreter. `mise run test` lints, runs `tests/`
on both, and runs the conformance suite's milestone set against the
adapter (`python -m conform`) on both datamodels.

`mise run wheel` builds the wheels, one per platform, with zig from any
machine, into `build/wheels/`, and smoke-tests this machine's wheel in
fresh environments for 3.13 and 3.14. `mise run release -- <version>`
checks, builds, verifies and smoke-tests a release, in Linux containers
too (Docker; `--skip-containers` leaves them out). `--publish` tags it,
uploads it to PyPI and creates the GitHub release, and `--dev --publish`
uploads `main` as a prerelease.

## License

MIT, as the repository ([LICENSE](../LICENSE)).
