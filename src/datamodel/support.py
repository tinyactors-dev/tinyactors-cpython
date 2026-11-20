# The Python half of the extension: the exceptions, a diagnostic record,
# and the "python" datamodel's restrictive rewrite and chart classes.
# It is not a module of the wheel: expressions.c embeds this text (the
# build writes it into build/<interpreter>/support.inc) and runs it once
# per interpreter when the module is executed; C reads the names below
# from its namespace. The rewrite runs once per chart, at chart_attach,
# and once per `.py` file a resolver reads; nothing here runs per event.
# AGENTS.md, "The "python" datamodel", holds the allow-list.
import ast
import copy


class TinyactorsError(Exception):
    """An error the core reported, the one exception type the host raises
    for its own reasons.

    It comes from a call the host refused (a full pool, a closed host, a
    session that does not exist), from a thin view read after its batch was
    acknowledged, from a fault of a run, or as a typed method's error
    (given to on_error, or raised by the driver without one). Branch on
    `status`, the core's name for a bare status ("TA_ERROR_STALE",
    "TA_ERROR_STATE", ...); a typed method's error also has `code`,
    `reason`, `path` and `message` as the host's reply wrote them.
    MemoryError is raised instead when the host ran out of a pool its
    config sized."""

    def __new__(cls, message="", code=None, reason=None, path=None, status=None, *rest, **named):
        if cls is TinyactorsError:  # the subclass of its status, or of a typed method's code
            cls = _BY_STATUS.get(status) or (None if code is None else _by_code(code)) or cls
        return super().__new__(cls, message)

    def __init__(self, message="", code=None, reason=None, path=None, status=None):
        super().__init__(message)
        self.message = message
        self.code = code
        self.reason = reason
        self.path = path
        self.status = status if status is not None or code is not None else self._status


class StaleError(TinyactorsError):
    """An actor, session, view or handle that does not exist (any more): a
    send to a session or name that ended, a thin view read after its batch
    was acknowledged, a typed method's "stale" error. Its status is
    "TA_ERROR_STALE"."""
    _status = "TA_ERROR_STALE"


class StateError(TinyactorsError):
    """A call the host cannot make now: a closed host, a snapshot from a
    handler or with a reply pending, a typed method's "state" error. Its
    status is "TA_ERROR_STATE"."""
    _status = "TA_ERROR_STATE"


class InvalidError(TinyactorsError):
    """An argument the host refused: a malformed config, image or address,
    a typed method's param of the wrong type or out of range. Its status is
    "TA_ERROR_INVALID"."""
    _status = "TA_ERROR_INVALID"


class UnsupportedError(TinyactorsError):
    """A value or a feature the host does not take: a value outside the
    value table, a session that cannot hibernate. Its status is
    "TA_ERROR_UNSUPPORTED"."""
    _status = "TA_ERROR_UNSUPPORTED"


TinyactorsError._status = None
_BY_STATUS = {"TA_ERROR_STALE": StaleError, "TA_ERROR_STATE": StateError,
              "TA_ERROR_INVALID": InvalidError, "TA_ERROR_UNSUPPORTED": UnsupportedError}


def _by_code(code):
    return StaleError if code == -32001 else StateError if code == -32002 else InvalidError


def settlers(future):
    """The handler and on_error of a typed method given no handler: they
    settle `future`, unless it was cancelled."""
    def result(value):
        if not future.done():
            future.set_result(value)

    def error(exception):
        if not future.done():
            future.set_exception(exception)
    return result, error


# ---- the host's construction ----------------------------------------------

# The system id the binding reserves for ask()'s reply addresses: a host
# may not take it as its own.
ASK_SYSTEM = "tinyactors-ask"


def host_config(system, io_processors, config):
    """The config ta_host_create gets, and the I/O processors' receive
    methods by type: `system` as "systemID" (None: none, which core
    refuses), `config` the rest, and "ioProcessors" made from
    `io_processors` in its order, each {type, location} with the location
    the processor's optional `location` attribute."""
    if config is None:
        full = {}
    elif type(config) is not dict:
        raise TypeError("config is a dict of the host config's system, migrateQuantum and telemetry")
    else:
        for key in ("systemID", "ioProcessors"):
            if key in config:
                raise TypeError("config has no %r: Host takes it as %s=" % (
                    key, "system" if key == "systemID" else "io_processors"))
        full = dict(config)
    if system == ASK_SYSTEM:
        raise ValueError("the system id %r is reserved for ask()'s replies" % ASK_SYSTEM)
    if system is not None:
        full = dict({"systemID": system}, **full)
    receivers = {}
    if io_processors is not None:
        if type(io_processors) is not dict:
            raise TypeError("io_processors is a dict of type -> processor")
        entries = []
        for kind, processor in io_processors.items():
            receive = getattr(processor, "receive", None)
            if isinstance(processor, type) or not callable(receive):
                raise TypeError("the I/O processor %r is not an object with receive(send)" % (kind,))
            entry = {"type": kind}
            location = getattr(processor, "location", None)
            if location is not None:
                entry["location"] = location
            entries.append(entry)
            receivers[kind] = receive
        if entries:
            full["ioProcessors"] = entries
    return full, receivers


# ---- actors ------------------------------------------------------------------

class Actor:
    """An actor of a host: what Host.spawn and Host.actor return, what
    send.sender and on_done get. It is a small immutable value made where
    the API returns one, compared and hashed by its session; keeping one
    keeps nothing alive in the host, and one whose actor ended refuses
    what needs the actor (StaleError).

    Use it to talk to the actor: send() an event, ask() a question and
    await the reply, await state(), context() or get() to inspect it,
    terminate() it, or await done for its donedata. The awaitable ones
    need the asyncio driver (async with Host(...))."""

    __slots__ = {
        "host": "The Host the actor runs in.",
        "id": "The actor's session id, an int (core's _sessionid).",
    }

    def __init__(self, host, id):
        object.__setattr__(self, "host", host)
        object.__setattr__(self, "id", id)

    def __setattr__(self, name, value):
        raise AttributeError("an Actor is immutable")

    def __eq__(self, other):
        if type(other) is not Actor:
            return NotImplemented
        return self.id == other.id and self.host is other.host

    def __hash__(self):
        return hash(self.id)

    def __repr__(self):
        return "Actor(%d)" % self.id

    @property
    def address(self):
        """The actor's address, tinyactors://<system>/#_scxml_<id>, the
        full form: what another system sends to. A chart sees this actor's
        sends' _event.origin either in it or as its fragment,
        #_scxml_<id>, so compare by the fragment."""
        return _tinyactors.Address.from_session(self.host.system, self.id)

    def send(self, event, data=None, *, sendid=None, traceparent=None):
        """Give the actor an event (Host.send to its session): applied at
        the call, run at the host's next turn. `data` becomes _event.data;
        `traceparent` continues a W3C trace."""
        self.host.send(self.id, event, data, sendid=sendid, traceparent=traceparent)

    def terminate(self):
        """End the actor (Host.terminate): it leaves its states, whose
        <onexit> runs, in the next turn, and sends no done event; a
        hibernated actor is woken from the host's store to end, and its
        blob deleted."""
        self.host.terminate(self.id)

    def state(self):
        """An awaitable of the actor's active states (the typed method
        actor_state): {"configuration": [...], ...} as core answers it."""
        return self.host.actor_state(session=self.id)

    def context(self):
        """An awaitable of the actor's datamodel variables (the typed
        method actor_context), as values of the value table."""
        return self.host.actor_context(session=self.id)

    def get(self):
        """An awaitable of what the host knows about the actor (the typed
        method actor_get): its chart, its state, its mailbox and timers."""
        return self.host.actor_get(session=self.id)

    def ask(self, event, data=None):
        """Send the actor `event` with `data` and return an awaitable of its
        reply, an (event, data) pair: the chart answers with <send
        targetexpr="_event.origin" event="..."/>. The event's origin is an
        address of the host's own (system tinyactors-ask), so the reply
        comes back here and never reaches your transport. There is no
        timeout of its own: use asyncio.wait_for, whose cancel drops the
        ask; asks pending when the host closes never complete. Needs the
        asyncio driver (TypeError under `with`)."""
        return self.host._ask(self.id, event, data)

    @property
    def done(self):
        """An awaitable of the actor's donedata, settled when it reaches a
        top-level final state (or raising the error it ended with). Await
        it under asyncio; an actor that ended already raises StaleError."""
        return self.host._done(self.id)


# ---- a directory of charts ----------------------------------------------------

def compile_dir(path, *, resolver=None):
    """Compile every *.scxml file under the directory `path` into one image,
    and return the Compiled, as compile_charts does.

    Keep an application's charts and their helper files (a .py file a
    chart reads with <script src>, a chart it invokes with <invoke src>)
    in one directory, compile it once at startup or in a build step, and
    give the result to any number of hosts. The charts are named by their
    path under `path` (sorted; the first is the image's default boot
    chart), and a src is read from under `path`, relative to the document
    that names it: a path that leaves `path`, symbolic links included, is
    refused (ResolveError, so CompileError). Give `resolver` to read the
    src files yourself instead. A chart file whose real path leaves `path`
    raises ValueError; an error in a chart raises CompileError."""
    import pathlib
    root = pathlib.Path(path).resolve(strict=True)
    if not root.is_dir():
        raise NotADirectoryError("compile_dir takes a directory: %s" % path)
    sources = []
    for file in sorted(root.rglob("*.scxml")):
        if not file.is_file():
            continue
        if not file.resolve().is_relative_to(root):
            raise ValueError("the chart %s leaves %s" % (file, root))
        sources.append((file.relative_to(root).as_posix(), file.read_text(encoding="utf-8")))
    if not sources:
        raise ValueError("no *.scxml file under %s" % root)
    return _tinyactors.compile_charts(sources, resolver if resolver is not None else _Directory(root))


class _Directory:
    """compile_dir's resolver: `source` read under `root`, relative to the
    document `base` that names it, refused when it leaves `root`."""

    def __init__(self, root):
        self.root = root

    def __call__(self, base, source):
        import pathlib
        target = (self.root / pathlib.PurePosixPath(base).parent / source).resolve()
        if not target.is_relative_to(self.root):
            raise ResolveError("%s leaves the charts' directory" % source)
        try:
            return target.read_text(encoding="utf-8")
        except FileNotFoundError:
            raise ResolveError("no file %s under the charts' directory" % source) from None
        except (IsADirectoryError, UnicodeDecodeError) as error:
            raise ResolveError("%s cannot be read: %s" % (source, error)) from None


# ---- stores: where hibernated actors' blobs go ---------------------------------

class MemoryStore:
    """A store of hibernated actors' blobs in a dict: Host(store=...)
    saves an actor's blob here when it hibernates the actor, loads it when
    an event comes for it, and deletes it once the actor is back or ended.

    Use it in tests, and where the host's memory, not the process's, is
    what must stay small: the blobs are bytes in this process. For blobs
    that leave memory use SQLiteStore, or your own object with the same
    three methods."""

    __slots__ = {"blobs": "The blobs by session, a dict: what a test reads to see what was stored."}

    def __init__(self):
        self.blobs = {}

    def save(self, session, blob):
        """Keep `blob`, the hibernated actor `session`'s bytes, replacing an
        earlier one."""
        self.blobs[session] = blob

    def load(self, session):
        """The blob saved for `session`; KeyError when there is none (the
        host reports it, and the actor stays hibernated)."""
        return self.blobs[session]

    def delete(self, session):
        """Forget `session`'s blob: the actor was woken, or it ended. One
        that is not there is nothing to do."""
        self.blobs.pop(session, None)


class SQLiteStore:
    """A store of hibernated actors' blobs in an SQLite database file, one
    row per actor in the table tinyactors_blobs (session, blob): what
    Host(store=..., resident=N) uses to keep at most N actors in memory
    and the rest on disk.

    `path` is the database file (":memory:" for none). It is a cache of
    the host's actors, not their durability: a host's blobs wake only in
    that host or a host restored from its snapshot, so keep the file with
    the snapshot when you keep one. Call close() when the host is closed."""

    __slots__ = {"connection": "The sqlite3 connection, for an application that reads the table itself."}

    def __init__(self, path):
        import sqlite3
        self.connection = sqlite3.connect(path, isolation_level=None)
        self.connection.execute("PRAGMA journal_mode=WAL")
        self.connection.execute("PRAGMA synchronous=NORMAL")
        self.connection.execute("CREATE TABLE IF NOT EXISTS tinyactors_blobs "
                                "(session INTEGER PRIMARY KEY, blob BLOB NOT NULL)")

    def save(self, session, blob):
        """Keep `blob`, the hibernated actor `session`'s bytes, replacing an
        earlier one (one row per session)."""
        self.connection.execute("INSERT OR REPLACE INTO tinyactors_blobs VALUES (?, ?)", (_signed(session), blob))

    def load(self, session):
        """The blob saved for `session`; KeyError when there is none (the
        host reports it, and the actor stays hibernated)."""
        row = self.connection.execute("SELECT blob FROM tinyactors_blobs WHERE session = ?",
                                      (_signed(session),)).fetchone()
        if row is None:
            raise KeyError(session)
        return row[0]

    def delete(self, session):
        """Forget `session`'s blob: the actor was woken, or it ended. One
        that is not there is nothing to do."""
        self.connection.execute("DELETE FROM tinyactors_blobs WHERE session = ?", (_signed(session),))

    def close(self):
        """Close the database connection; the file keeps the blobs saved."""
        self.connection.close()


def _signed(session):
    """A session id (to 2^64 - 1) as SQLite's signed 64-bit integer."""
    return session - (1 << 64) if session >= 1 << 63 else session


class Spawner:
    """An I/O processor that spawns actors for charts: give it to the host
    under a type you name, io_processors={"spawn": tinyactors.Spawner()},
    and a chart spawns with <send type="spawn" event="spawn"> whose data is
    {"chart": name, "name": name or None, "params": dict or None}.

    The new actor runs the chart's current version (Host.spawn) under the
    name. The sender gets `spawned` with {"name", "address"} (the new
    actor's session address) and the send's sendid, so a <send id> matches
    its answer; an unknown chart, data of another shape or a spawn the host
    refuses gives the sender error.communication instead. A type no
    processor serves stays error.execution, as SCXML has it.

    It spawns; it does not look names up. A name has one owner, the
    collection actor that decides which actors of a kind exist, and that
    actor records the spawns it asked for, so it never asks twice for a
    name. Spawning under a name an older version's actor holds, after
    add_image of a new version, is how an application updates."""

    __slots__ = ()

    def receive(self, send):
        """Spawn what send.data asks for and answer `spawned`, or fail the
        send (error.communication) when the host cannot."""
        data = send.data
        try:
            if type(data) is not dict or type(data.get("chart")) is not str:
                raise TypeError("a Spawner's data is {chart, name, params}")
            actor = send.host.spawn(data["chart"], name=data.get("name"), params=data.get("params"))
        except (TinyactorsError, TypeError, ValueError, MemoryError):
            send.fail()
            return
        send.answer("spawned", {"name": data.get("name"), "address": actor.address})


async def actors(host, chart):
    """Every live actor of the host (actor.list, a page at a time), or
    those of the chart named `chart`."""
    found, cursor = [], 0
    while True:
        page = await host.actor_list(cursor=cursor, limit=512)
        for session in page["sessions"]:
            if chart is None or host._chart_of(session) == chart:
                found.append(Actor(host, session))
        cursor = page["cursor"]
        if cursor is None:
            return found


class CompileError(TinyactorsError):
    """compile_charts or compile_system found an error in the documents.

    `diagnostics` is every Diagnostic of the compilation (errors, warnings
    and notes, each with its file, line and column), so an editor
    integration, a build step or a test can show them all at once; str()
    of the error names the first. Nothing was compiled: fix the documents
    and compile again. Catch it where charts are compiled (at startup, in
    a build or deploy step), not around the host, which never raises
    it."""

    def __init__(self, message, diagnostics):
        super().__init__(message, status="TA_ERROR_INVALID")
        self.diagnostics = diagnostics


class ResolveError(TinyactorsError):
    """Raise it from a resolver, resolver(base, source), to refuse a
    document a chart references with src=: not found, not allowed (a path
    outside the directory your charts live in), not valid.

    A resolver is how charts that are split over files get compiled: the
    compiler calls it for each static src= it does not have among the
    sources. Raising ResolveError makes the refusal a diagnostic like any
    other, at `line` and `column` when given (a resolver that checks a
    file's content knows where it is wrong), so compile_charts or
    compile_system raises CompileError with it among the others. Any other
    exception a resolver raises is a bug in the resolver and propagates
    from the compile call."""

    def __init__(self, message, line=None, column=None):
        super().__init__(message, status="TA_ERROR_INVALID")
        self.line = line
        self.column = column


class Diagnostic:
    """One message of the compiler about a document: in
    Compiled.diagnostics (warnings and notes of a compilation that
    succeeded) and CompileError.diagnostics. Show them where a person
    edits charts (a terminal, an editor, a CI log) or assert on them in a
    test.

    `severity` is "error", "warning" or "note", `code` the compiler's
    code for the kind of problem, `file`, `line` and `column` where it is
    (the source's name as given to compile_charts), `message` the text.
    repr() reads like a compiler's output line."""

    __slots__ = {
        "severity": "\"error\", \"warning\" or \"note\".",
        "code": "The compiler's code for the kind of problem: match on it rather than on the message.",
        "file": "The document's name, as given to compile_charts or returned by a resolver.",
        "line": "The line in that document, from 1.",
        "column": "The column in that line, from 1.",
        "message": "What the compiler says, in one sentence.",
    }

    def __init__(self, severity, code, file, line, column, message):
        self.severity = severity
        self.code = code
        self.file = file
        self.line = line
        self.column = column
        self.message = message

    def __repr__(self):
        return "%s:%d:%d: %s: %s" % (self.file, self.line, self.column, self.severity, self.message)


for _type in (TinyactorsError, CompileError, ResolveError, Diagnostic, Actor, MemoryStore, SQLiteStore, Spawner, compile_dir):
    _type.__module__ = "tinyactors"

# ---- the rewrite ---------------------------------------------------------

BUILTINS = ("len", "min", "max", "sum", "abs", "round", "sorted", "reversed", "enumerate", "zip",
            "range", "any", "all", "isinstance", "hasattr", "str", "int", "float", "bool", "list", "dict",
            "tuple", "set", "frozenset", "map", "filter")
# The session's read-only system variables, properties of its class.
SYSTEM = ("_sessionid", "_name", "_ioprocessors")
# The hidden parameter every compiled function takes: the session.
SESSION = "_ta_s"
VALUE = "_ta_v"

_EXPRESSIONS = (ast.Expression, ast.Constant, ast.BinOp, ast.UnaryOp, ast.BoolOp, ast.Compare,
                ast.IfExp, ast.Subscript, ast.Slice, ast.ListComp, ast.SetComp, ast.DictComp,
                ast.GeneratorExp, ast.comprehension, ast.JoinedStr, ast.FormattedValue, ast.Call,
                ast.keyword, ast.Attribute, ast.Name, ast.List, ast.Tuple, ast.Dict, ast.Set,
                ast.Starred, ast.Load, ast.Store, ast.operator, ast.unaryop, ast.boolop, ast.cmpop)
_STATEMENTS = (ast.Module, ast.Assign, ast.AugAssign, ast.If, ast.For, ast.While, ast.Pass,
               ast.Expr, ast.Break, ast.Continue, ast.Return, ast.FunctionDef, ast.arguments,
               ast.arg)
_NAMES = {ast.Import: "import", ast.ImportFrom: "import", ast.Global: "global",
          ast.Nonlocal: "nonlocal", ast.Lambda: "lambda", ast.Await: "await", ast.Yield: "yield",
          ast.YieldFrom: "yield", ast.NamedExpr: "the walrus (:=)", ast.Delete: "del",
          ast.Try: "try", ast.Raise: "raise", ast.With: "with", ast.ClassDef: "class",
          ast.AsyncFunctionDef: "async def", ast.AsyncFor: "async for", ast.AsyncWith: "async with"}


class Rewrite:
    """Checks one source against the allow-list and rewrites the names of
    the session into attributes of the hidden parameter: `tries < 3`
    becomes `_ta_s.tries < 3`. A refusal is a SyntaxError at the node,
    with the document's line (`line` is where the source starts)."""

    def __init__(self, names, file, line):
        self.names = names  # the session's: <data> ids, _event, methods
        self.file = file
        self.line = line
        self.system = set()  # the system variables the source reads
        self.methods = set()  # the chart's methods: names, not locations

    def refuse(self, node, message):
        line = getattr(node, "lineno", 1)
        error = SyntaxError(message, (self.file, line + self.line - 1, getattr(node, "col_offset", 0) + 1, None))
        raise error

    def check(self, node):
        if not isinstance(node, _EXPRESSIONS + _STATEMENTS):
            self.refuse(node, "%s is not allowed in a chart" % _NAMES.get(type(node), type(node).__name__))

    def session(self, node, name, context):
        return ast.copy_location(ast.Attribute(ast.copy_location(ast.Name(SESSION, ast.Load()), node), name, context), node)

    # expressions, in the scope `local` (names bound in a def or comprehension)
    def expression(self, node, local):
        self.check(node)
        if isinstance(node, ast.Name):
            if node.id in local:
                return node
            if node.id.startswith("_ta_"):
                self.refuse(node, "the name %s is reserved" % node.id)
            if node.id in self.names or node.id in SYSTEM:
                if node.id in SYSTEM:
                    self.system.add(node.id)
                return self.session(node, node.id, ast.Load())
            if node.id == "In" or node.id in BUILTINS:
                return node
            self.refuse(node, "the name %s is not declared" % node.id)
        if isinstance(node, ast.Attribute):
            if node.attr.startswith("__") and node.attr.endswith("__") or node.attr.startswith("_ta_"):
                self.refuse(node, "the attribute %s is not allowed" % node.attr)
            node.value = self.expression(node.value, local)
            return node
        if isinstance(node, (ast.ListComp, ast.SetComp, ast.GeneratorExp, ast.DictComp)):
            inner = set(local)
            for generator in node.generators:
                self.check(generator)
                generator.iter = self.expression(generator.iter, inner)
                self.bind(generator.target, inner)
                generator.ifs = [self.expression(test, inner) for test in generator.ifs]
            if isinstance(node, ast.DictComp):
                node.key = self.expression(node.key, inner)
                node.value = self.expression(node.value, inner)
            else:
                node.elt = self.expression(node.elt, inner)
            return node
        for field, value in ast.iter_fields(node):
            if isinstance(value, ast.AST):
                setattr(node, field, self.expression(value, local))
            elif isinstance(value, list):
                setattr(node, field, [self.expression(item, local) if isinstance(item, ast.AST) else item for item in value])
        return node

    # a comprehension's or a for loop's local target in a def
    def bind(self, target, local):
        self.check(target)
        if isinstance(target, ast.Name):
            local.add(target.id)
        elif isinstance(target, (ast.Tuple, ast.List)):
            for element in target.elts:
                self.bind(element, local)
        else:
            self.refuse(target, "a comprehension binds names only")

    # an assignment's target: a <data> id, its items or attributes; in a
    # def, a local name too
    def target(self, node, local, in_def):
        self.check(node)
        if isinstance(node, ast.Name):
            if node.id in local:
                return node
            if node.id in SYSTEM or node.id == "_event":
                self.refuse(node, "the system variable %s is read-only" % node.id)
            if node.id in self.names and not node.id in self.methods:
                return self.session(node, node.id, ast.Store())
            if in_def and not node.id.startswith("_ta_"):
                local.add(node.id)
                return node
            self.refuse(node, "assignment to %s, which is not declared" % node.id)
        if isinstance(node, (ast.Attribute, ast.Subscript)):
            if isinstance(node, ast.Attribute) and (node.attr.startswith("__") and node.attr.endswith("__") or node.attr.startswith("_ta_")):
                self.refuse(node, "the attribute %s is not allowed" % node.attr)
            node.value = self.expression(node.value, local)
            if isinstance(node, ast.Subscript):
                node.slice = self.expression(node.slice, local)
            node.ctx = ast.Store()
            return node
        if isinstance(node, (ast.Tuple, ast.List)):
            node.elts = [self.target(element, local, in_def) for element in node.elts]
            node.ctx = ast.Store()
            return node
        self.refuse(node, "this is not a location")

    def statements(self, body, local, in_def):
        return [self.statement(node, local, in_def) for node in body]

    def statement(self, node, local, in_def):
        self.check(node)
        if isinstance(node, ast.Assign):
            node.value = self.expression(node.value, local)
            node.targets = [self.target(target, local, in_def) for target in node.targets]
        elif isinstance(node, ast.AugAssign):
            node.value = self.expression(node.value, local)
            node.target = self.target(node.target, local, in_def)
        elif isinstance(node, (ast.If, ast.While)):
            node.test = self.expression(node.test, local)
            node.body = self.statements(node.body, local, in_def)
            node.orelse = self.statements(node.orelse, local, in_def)
        elif isinstance(node, ast.For):
            node.iter = self.expression(node.iter, local)
            node.target = self.target(node.target, local, in_def)
            node.body = self.statements(node.body, local, in_def)
            node.orelse = self.statements(node.orelse, local, in_def)
        elif isinstance(node, ast.Expr):
            node.value = self.expression(node.value, local)
        elif isinstance(node, ast.Return):
            if not in_def:
                self.refuse(node, "return outside a def")
            if node.value is not None:
                node.value = self.expression(node.value, local)
        elif isinstance(node, ast.FunctionDef):
            self.refuse(node, "def is allowed only at the top level of a script")
        return node

    # a top-level def of a script: a method of the chart's class, its
    # first parameter the session
    def method(self, node):
        if node.decorator_list or node.returns:
            self.refuse(node, "a def has no decorators or annotations")
        arguments = node.args
        if arguments.posonlyargs or arguments.vararg or arguments.kwonlyargs or arguments.kwarg:
            self.refuse(node, "a def takes plain parameters")
        local = set()
        for argument in arguments.args:
            if argument.annotation is not None:
                self.refuse(argument, "a def has no decorators or annotations")
            local.add(argument.arg)
        arguments.defaults = [self.expression(value, set()) for value in arguments.defaults]
        arguments.args.insert(0, ast.copy_location(ast.arg(SESSION), node))
        node.body = self.statements(node.body, local, True)
        return node


def _parse(source, mode, file, line):
    try:
        tree = ast.parse(source.strip() if mode == "eval" else source, file, mode)
    except SyntaxError as error:
        raise SyntaxError(error.msg, (file, (error.lineno or 1) + line - 1, error.offset or 0, None)) from None
    return tree


def methods_of(source, file, line):
    """The top-level defs of a script, by name (checked when attached)."""
    return [node.name for node in _parse(source, "exec", file, line).body if isinstance(node, ast.FunctionDef)]


def check_file(source, file, names):
    """A `.py` file a resolver read: every construct against the
    allow-list; names are checked when its chart is attached."""
    tree = _parse(source, "exec", file, 1)
    rewrite = Rewrite(_Anything(), file, 1)
    for node in tree.body:
        if isinstance(node, ast.FunctionDef):
            rewrite.method(node)
        else:
            rewrite.statement(node, set(), False)


class _Anything:
    def __contains__(self, name):
        return True


def _function(body, parameters, namespace, file):
    module = ast.parse("def _ta_f(%s):\n    pass\n" % ", ".join(parameters))
    function = module.body[0]
    function.body = body or [ast.Pass()]
    ast.fix_missing_locations(module)
    code = compile(module, file, "exec")
    local = {}
    exec(code, namespace, local)
    return local["_ta_f"]


CONDITION, VALUE_KIND, LOCATION, SCRIPT, STRING, CONTENT, ITEM, GLOBAL_SCRIPT = 1, 2, 3, 4, 5, 6, 7, 8
SCRIPTS = (SCRIPT, GLOBAL_SCRIPT)  # in executable content, and the chart's top-level ones: the same here


def _names_bound(target, names):
    if isinstance(target, ast.Name):
        names.add(target.id)
    elif isinstance(target, (ast.Tuple, ast.List)):
        for element in target.elts:
            _names_bound(element, names)


def _declared_by(body, names):
    """The names a script's statements assign or loop over outside its
    defs: the variables it declares, as a module's top level does."""
    for node in body:
        if isinstance(node, ast.Assign):
            for target in node.targets:
                _names_bound(target, names)
        elif isinstance(node, ast.For):
            _names_bound(node.target, names)
        if isinstance(node, (ast.If, ast.While, ast.For)):
            _declared_by(node.body, names)
            _declared_by(node.orelse, names)
    return names


def attach(name, data, expressions, namespace, file):
    """A chart's class and its compiled expressions. `data` holds the
    <data> ids, `expressions` (kind, source, line) per expression id.
    Returns (class, functions, system, variables): functions[e] is a
    function of the session (CONDITION, VALUE, STRING, SCRIPT, CONTENT),
    a pair (getter, setter) for a LOCATION or an ITEM, or the SyntaxError
    that refused it (an evaluation of it is error.execution); system the
    set of system variables the chart reads; variables the session's
    variables: the <data> ids, then the names a <foreach> item or index
    or a <script> outside its defs binds, which the chart declares too
    (SCXML 4.6, and as a module's top level does)."""
    for identifier in data:
        if not identifier.isidentifier() or identifier.startswith("_"):
            raise SyntaxError("the <data> id %r is not a Python name the chart may declare" % identifier)
    methods = {}
    scripts = []
    for index, (kind, source, line) in enumerate(expressions):
        if kind in SCRIPTS:
            try:
                tree = _parse(source, "exec", file, line)
            except SyntaxError:
                continue
            ast.increment_lineno(tree, line - 1)
            scripts.append((index, tree))
            for node in tree.body:
                if isinstance(node, ast.FunctionDef):
                    methods[node.name] = node
    declared = set()
    for index, tree in scripts:
        _declared_by(tree.body, declared)
    for kind, source, line in expressions:
        if kind == ITEM and source.strip().isidentifier():
            declared.add(source.strip())
    declared = {identifier for identifier in declared - set(data) - set(methods)
                if not identifier.startswith("_")}
    variables = tuple(data) + tuple(sorted(declared))
    names = set(variables) | {"_event"} | set(methods)
    system = set()
    compiled_methods = {}
    for method_name, node in methods.items():
        if method_name in variables or method_name in SYSTEM or method_name.startswith("_"):
            raise SyntaxError("the def %s has the name of a variable" % method_name)
        rewrite = Rewrite(names, file, 1)
        rewrite.methods = set(methods)
        compiled_methods[method_name] = _function_from_def(rewrite.method(node), namespace, file)
        system |= rewrite.system
    functions = []
    script_trees = dict(scripts)
    for index, (kind, source, line) in enumerate(expressions):
        rewrite = Rewrite(names, file, 1 if kind in SCRIPTS else line)
        rewrite.methods = set(methods)
        try:
            if kind in SCRIPTS:
                tree = script_trees.get(index) or _parse(source, "exec", file, line)
                body = [rewrite.statement(node, set(), False) for node in tree.body
                        if not isinstance(node, ast.FunctionDef)]
                functions.append(_function(body, [SESSION], namespace, file))
            elif kind in (LOCATION, ITEM):
                tree = _parse(source, "eval", file, line).body
                getter = _expression(_moved(rewrite.expression(copy.deepcopy(tree), set()), line), namespace, file)
                setter = ast.copy_location(ast.Assign([_moved(rewrite.target(tree, set(), False), line)],
                                                      ast.Name(VALUE, ast.Load())), tree)
                functions.append((getter, _function([setter], [SESSION, VALUE], namespace, file)))
            else:
                try:
                    tree = _parse(source, "eval", file, line).body
                    functions.append(_expression(_moved(rewrite.expression(tree, set()), line), namespace, file))
                except SyntaxError:
                    if kind != CONTENT:
                        raise
                    functions.append(_constant(source.strip()))
            system |= rewrite.system
        except Exception as error:  # refused, or not compiled: error.execution when evaluated
            functions.append(error)
    slots = variables + ("_event", "_ta_sys")
    body = {"__slots__": slots, "__module__": "tinyactors.charts"}
    body["_sessionid"] = property(lambda session: session._ta_sys[0])
    body["_name"] = property(lambda session: session._ta_sys[1])
    body["_ioprocessors"] = property(lambda session: session._ta_sys[2])
    body.update(compiled_methods)
    chart = type(name if name.isidentifier() else "Chart", (), body)
    return chart, tuple(functions), frozenset(system), variables


def _moved(tree, line):
    """The tree at the document's line (the source starts at `line`)."""
    return ast.increment_lineno(tree, line - 1)


def _expression(tree, namespace, file):
    body = [ast.copy_location(ast.Return(tree), tree)]
    return _function(body, [SESSION], namespace, file)


def _constant(text):
    return lambda session: text


def _function_from_def(node, namespace, file):
    module = ast.Module([node], [])
    ast.fix_missing_locations(module)
    local = {}
    exec(compile(module, file, "exec"), namespace, local)
    return local[node.name]
