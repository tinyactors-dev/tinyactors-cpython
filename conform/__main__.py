"""The conformance suite's adapter: tinyactors-test/docs/driver.md over the
public module `tinyactors`, and nothing else (its lint refuses any other
import but the standard library).

Run by ``mise run conform``, which puts the built extension on the import
path. Frames are a four-byte big-endian length and one CBOR message, read
and written with tinyactors.decode and tinyactors.encode. Each driver
method is the binding's own calls, as an application makes them: a host
is a tinyactors.Host, a control message is the typed method of its name,
and what goes to another system leaves through the I/O processor the
config marks remote. Standard output holds frames only; diagnostics go to
standard error.
"""

import math
import sys
import urllib.parse

import tinyactors

# ta_status by the name ta_status_name gives (tinyactors/types.h): what the
# driver reports as "status".
STATUS = {
    "TA_OK": 0, "TA_ERROR_EXECUTION": -1, "TA_ERROR_COMMUNICATION": -2, "TA_ERROR_INVALID": -3,
    "TA_ERROR_NO_MEMORY": -4, "TA_ERROR_STALE": -5, "TA_ERROR_STATE": -6, "TA_ERROR_UNSUPPORTED": -7,
    "TA_ERROR_FATAL": -8, "TA_ERROR_ABORTED": -9,
}
TA_RUN_IDLE, TA_RUN_FAULT, TA_RUN_EXHAUSTED = 0, 4, 3
# The driver's datamodel names, and the vtable names a Host answers for them.
DATAMODELS = {"native": "python", "quickjs": "ecmascript"}
TYPED = {"actor.state", "actor.context", "actor.list", "actor.get", "host.stats", "host.config",
         "trace.configure", "trace.watch", "trace.unwatch", "trace.end", "trace.spans.decode"}


class DriverError(Exception):
    """The protocol's own error (driver.md, "Errors are the protocol's own")."""

    def __init__(self, code, reason, path, message):
        super().__init__(message)
        self.code, self.reason, self.path = code, reason, path


def status_of(error):
    """A ta_status for what the binding raised."""
    if isinstance(error, MemoryError):
        return STATUS["TA_ERROR_NO_MEMORY"]
    if isinstance(error, tinyactors.TinyactorsError) and error.status in STATUS:
        return STATUS[error.status]
    return STATUS["TA_ERROR_INVALID"]


def error_map(error):
    """A TinyactorsError from a reply, as core wrote it."""
    return {"code": error.code, "message": error.message or "",
            "data": {"reason": error.reason, "path": error.path}}


class Documents:
    """The resolver of a call's documents: a `src` names one by its name,
    with a leading file://, file: or ./ removed (driver.md, "src")."""

    def __init__(self, documents):
        self.documents = {document["name"]: document["text"] for document in documents}

    def __call__(self, base, source):
        for prefix in ("file://", "file:", "./"):
            if source.startswith(prefix):
                source = source[len(prefix):]
        if source not in self.documents:
            raise tinyactors.ResolveError("no document named %s" % source)
        return self.documents[source]


def compile_image(host, charts, documents):
    """charts compiled into one image, added to the host: the segment."""
    sources = [(chart["name"], chart["text"]) for chart in charts]
    with tinyactors.compile_charts(sources, Documents(documents)) as compiled:
        return host.add_image(compiled.image)


class Adapter:
    def __init__(self):
        self.host = None
        self.clock = None
        self.sends = []

    def remote(self, host, send):
        """A send to another system: kept for host.run, its data written
        with the binding's own codec (no data reads as None, null's CBOR)."""
        self.sends.append({"target": send.target, "event": send.event, "origin": send.origin,
                           "data": tinyactors.encode(send.data)})

    def need_host(self):
        if self.host is None:
            raise DriverError(-32002, "state", "", "no host: call host.create first")
        return self.host

    # ---- the protocol vectors' methods --------------------------------------

    def host_create(self, params):
        if self.host is not None:
            raise DriverError(-32002, "state", "", "a host already: call host.free first")
        datamodel = params.get("datamodel", "quickjs")
        if datamodel not in DATAMODELS:
            raise DriverError(-32602, "range", "params.datamodel", "no such datamodel")
        config = tinyactors.decode(params["config"])
        self.clock = tinyactors.SimulatedClock()
        self.sends = []
        processors = config.get("ioProcessors") if isinstance(config, dict) else None
        remote = {processor["type"]: self.remote for processor in processors
                  if isinstance(processor, dict) and processor.get("remote")} if isinstance(processors, list) else {}
        try:
            self.host = tinyactors.Host(config, remote, clock=self.clock)
        except tinyactors.TinyactorsError as error:
            result = {"status": status_of(error)}
            if error.code is not None:
                result["error"] = tinyactors.encode({"error": error_map(error)})
            return result
        except MemoryError:
            return {"status": STATUS["TA_ERROR_NO_MEMORY"]}
        return {"status": 0, "datamodel": DATAMODELS[datamodel]}

    def host_free(self, params):
        self.need_host().close()
        self.host = None
        return {}

    def host_post(self, params):
        """The message's method is the typed method of its name; its reply
        is built from what the method's handler or on_error got, with the
        message's id (driver.md, "Kinds")."""
        host = self.need_host()
        message = tinyactors.decode(params["message"])
        if not isinstance(message, dict) or message.get("method") not in TYPED or \
                not isinstance(message.get("params", {}), dict):
            raise DriverError(-32602, "type", "params.message",
                              "a binding sends the typed methods of requests.md only")
        answered = []
        method = getattr(host, message["method"].replace(".", "_"))
        try:
            method(lambda host, result: answered.append({"result": result}),
                   lambda host, error: answered.append({"error": error_map(error)}),
                   **message.get("params", {}))
        except (tinyactors.TinyactorsError, MemoryError) as error:
            return {"status": status_of(error)}
        host.run_until_idle()
        result = {"status": 0, "notifications": []}
        if answered and "id" in message:
            result["reply"] = tinyactors.encode(dict({"id": message["id"]}, **answered[0]))
        return result

    def image_add(self, params):
        host = self.need_host()
        try:
            return {"status": 0, "segment": compile_image(host, params["charts"], params["charts"])}
        except tinyactors.CompileError:
            return {"status": STATUS["TA_ERROR_INVALID"]}
        except (tinyactors.TinyactorsError, MemoryError) as error:
            return {"status": status_of(error)}

    def actor_spawn(self, params):
        host = self.need_host()
        try:
            session = host.spawn(host.chart(params["chart"]), params=params.get("params"))
        except (tinyactors.TinyactorsError, MemoryError) as error:
            return {"status": status_of(error)}
        return {"status": 0, "session": session}

    def actor_send(self, params):
        host = self.need_host()
        data = params.get("data")
        if "cbor" in params:
            try:
                data = tinyactors.decode(params["cbor"])
            except (ValueError, TypeError, tinyactors.TinyactorsError):
                return {"status": STATUS["TA_ERROR_INVALID"]}
        try:
            host.send(params["session"], params["event"], data, origin=params.get("origin"))
        except (tinyactors.TinyactorsError, MemoryError) as error:
            return {"status": status_of(error)}
        return {"status": 0}

    def clock_advance(self, params):
        host = self.need_host()
        milliseconds = params.get("monotonic", 0) * 1000 // host.ticks_per_second
        now = round(self.clock.now * 1000)
        if milliseconds > now:
            self.clock.increment((milliseconds - now) / 1000)
        return {}

    def host_run(self, params):
        host = self.need_host()
        status = TA_RUN_IDLE
        try:
            host.run_until_idle()
        except MemoryError:
            status = TA_RUN_EXHAUSTED
        except tinyactors.TinyactorsError:
            status = TA_RUN_FAULT
        sends, self.sends = self.sends, []
        return {"status": status, "sends": sends, "notifications": []}

    # ---- scenarios --------------------------------------------------------------

    def scenario_run(self, params):
        if params.get("datamodel", "quickjs") not in DATAMODELS:
            raise DriverError(-32602, "range", "params.datamodel", "no such datamodel")
        return {"outcome": Scenario(params["documents"]).run(params["steps"])}

    def w3c_run(self, params):
        if params.get("datamodel", "quickjs") not in DATAMODELS:
            raise DriverError(-32602, "range", "params.datamodel", "no such datamodel")
        return {"outcome": W3C(params["documents"]).run()}


class Refused(Exception):
    """A step the host refused: the scenario fails (driver.md)."""


class Scenario:
    """scenario.run in the scenario environment (driver.md): a host of its
    own on a simulated clock, the steps' calls, the loop until idle after
    each, and the first Outcome log."""

    def __init__(self, documents):
        self.documents = documents
        self.aliases = {}
        self.outcome = None
        self.clock = tinyactors.SimulatedClock()
        self.blobs = {}  # the implementation's store (driver.md): hibernated actors' blobs
        self.snapshots = {}
        self.migrations_done = []  # the results migration_done got
        self.host = None

    def wake(self, host, session, migration):
        """WAKE: the actor's blob from the store."""
        host.wake(session, self.blobs.pop(session))

    def migration_done(self, host, result):
        self.migrations_done.append(result)

    def log(self, host, entry):
        if self.outcome is None and entry.label == "Outcome":
            self.outcome = "pass" if entry.text == "pass" else "fail"

    def value(self, value):
        """"@alias" at any depth: that actor's address."""
        if isinstance(value, str) and value.startswith("@") and value[1:] in self.aliases:
            return "#_scxml_%d" % self.aliases[value[1:]]
        if isinstance(value, list):
            return [self.value(item) for item in value]
        if isinstance(value, dict):
            return {key: self.value(item) for key, item in value.items()}
        return value

    CONFIG = {"systemID": "conformance", "system": {"ticksPerSecond": 1000}}

    def run(self, steps):
        self.host = tinyactors.Host(self.CONFIG, log=self.log, clock=self.clock, wake=self.wake,
                                    migration_done=self.migration_done)
        try:
            for step in steps:
                try:
                    self.step(self.host, step)
                    if self.outcome is None:
                        self.host.run_until_idle()
                except tinyactors.CompileError as error:
                    print("conform: a document does not compile: %s" % error, file=sys.stderr)
                    self.outcome = "compile"
                except (Refused, tinyactors.TinyactorsError, MemoryError) as error:
                    print("conform: the step %r was refused: %r" % (step, error), file=sys.stderr)
                    self.outcome = self.outcome or "fail"
                if self.outcome is not None:
                    break
        finally:
            self.host.close()
        return self.outcome or "timeout"

    def step(self, host, step):
        if "image" in step:
            named = {document["name"]: document for document in self.documents}
            missing = [name for name in step["image"] if name not in named]
            if missing:
                raise Refused("image names documents the call does not have: %r" % missing)
            compile_image(host, [named[name] for name in step["image"]], self.documents)
        elif "spawn" in step:
            self.aliases[step["spawn"]] = host.spawn(host.chart(step["chart"]), name=step["spawn"],
                                                     params=self.value(step.get("params")))
        elif "send" in step:
            host.send(self.session(step["send"]), step["event"], self.value(step.get("data")))
        elif "sendMany" in step:
            items = [(self.session(item["to"]), item["event"], self.value(item.get("data")))
                     for item in step["sendMany"]]
            if host.send_many(items) != len(items):
                raise Refused("send_many took fewer events")
        elif "advance" in step:
            self.clock.increment(step["advance"] / 1000)  # ticksPerSecond 1000: ticks are milliseconds
        elif "hibernate" in step:
            session = self.session(step["hibernate"])
            self.blobs[session] = host.hibernate(session)
        elif "snapshot" in step:
            self.snapshots[step["snapshot"]] = host.snapshot()
        elif "restore" in step:
            if step["restore"] not in self.snapshots:
                raise Refused("a restore of a snapshot no step took: %s" % step["restore"])
            restored = tinyactors.Host.restore(self.snapshots[step["restore"]], self.CONFIG, log=self.log,
                                               clock=self.clock, wake=self.wake, migration_done=self.migration_done)
            host.close()
            self.host = restored  # a restart: the restored host replaces the running one
        elif "migrate" in step:
            ended = len(self.migrations_done)
            host.migrate(wake=bool(step["migrate"].get("wake")), timeout=step["migrate"].get("timeout", 0) / 1000)
            for _ in range(100):  # the loop until its MIGRATION_DONE, bounded as ta-conform's settles
                host.run_until_idle()
                if len(self.migrations_done) > ended:
                    break
            if len(self.migrations_done) == ended:
                raise Refused("the migration did not end")
        else:
            raise Refused("not in the binding's API yet: %s" % ", ".join(step))

    def session(self, alias):
        if alias not in self.aliases:
            raise Refused("a send to an alias no spawn made: %s" % alias)
        return self.aliases[alias]


class W3C:
    """w3c.run in the W3C environment (driver.md): the start chart booted
    on a host of its own, BasicHTTP as a loopback, and the clock moved to
    the next deadline whenever the host is idle."""

    LOCATION = "http://localhost:8080/scxml"
    BASIC_HTTP = "http://www.w3.org/TR/scxml/#BasicHTTPEventProcessor"
    CONFIG = {"systemID": "w3c",
              "system": {"ticksPerSecond": 1000, "maxEvents": 1024, "maxActors": 64},
              "ioProcessors": [{"type": BASIC_HTTP, "location": LOCATION, "sync": True}]}
    ROUNDS = 200

    def __init__(self, documents):
        self.documents = documents
        self.outcome = None

    def log(self, host, entry):
        if self.outcome is None and entry.label == "Outcome":
            self.outcome = "pass" if entry.text == "pass" else "fail"

    def post(self, host, send):
        """A send to BasicHTTP: written as a POST, read back, and delivered to
        its sender, as an incoming request is; a send to another target
        cannot be written."""
        if not (send.target or "").startswith(self.LOCATION):
            send.fail()
            return
        message = post_encode(send.target, send.event or "", send.data)
        event, fields = post_parse(message)
        send.answer(event, fields, origin=self.LOCATION, raw=message)

    def run(self):
        charts = [document for index, document in enumerate(self.documents)
                  if index == 0 or document["name"].endswith(".scxml")]
        clock = tinyactors.SimulatedClock()
        host = tinyactors.Host(self.CONFIG, io_processors={self.BASIC_HTTP: self.post}, log=self.log,
                               clock=clock)
        try:
            try:
                host.boot(compile_image(host, charts, self.documents))
            except tinyactors.CompileError as error:
                print("conform: a document does not compile: %s" % error, file=sys.stderr)
                return "compile"
            for _ in range(self.ROUNDS):
                host.run_until_idle()
                delay = host.next_deadline
                if self.outcome is not None or delay is None:
                    break
                clock.increment(max(1, math.ceil(round(delay * 1000, 6))) / 1000)  # ticksPerSecond 1000
        except (tinyactors.TinyactorsError, MemoryError) as error:
            print("conform: the run ended: %r" % error, file=sys.stderr)
        finally:
            host.close()
        return self.outcome or "timeout"


def diagnostic(value):
    """A value in CBOR diagnostic notation (RFC 8949, 8)."""
    if value is True or value is False:
        return "true" if value else "false"
    if value is None:
        return "null"
    if value is tinyactors.UNDEFINED:
        return "undefined"
    if isinstance(value, str):
        return '"%s"' % value.replace("\\", "\\\\").replace('"', '\\"')
    if isinstance(value, bytes):
        return "h'%s'" % value.hex()
    if isinstance(value, float):
        return repr(value) if math.isfinite(value) else ("NaN" if value != value else
                                                         "Infinity" if value > 0 else "-Infinity")
    if isinstance(value, list):
        return "[%s]" % ", ".join(diagnostic(item) for item in value)
    if isinstance(value, dict):
        return "{%s}" % ", ".join("%s: %s" % (diagnostic(key), diagnostic(item)) for key, item in value.items())
    return str(value)


def post_encode(url, event, data):
    """The POST of a BasicHTTP send (SCXML 1.0, C.2.1): form fields, the
    event name as _scxmleventname and the data's when it is a map (each
    value as text: text as it is, anything else in diagnostic notation), or
    the data as the body when it is text."""
    parts = urllib.parse.urlsplit(url)
    fields = [(key, value if isinstance(value, str) else diagnostic(value))
              for key, value in data.items()] if isinstance(data, dict) else []
    if fields or event:
        named = [("_scxmleventname", event)] if event else []
        body = "&".join("%s=%s" % (urllib.parse.quote(key, safe=""), urllib.parse.quote(value, safe=""))
                        for key, value in named + fields)
    else:
        body = urllib.parse.quote(data, safe="") if isinstance(data, str) else ""
    return ("POST %s HTTP/1.1\r\nHost: %s\r\nContent-Type: application/x-www-form-urlencoded\r\n"
            "Content-Length: %d\r\n\r\n%s" % (parts.path or "/", parts.netloc, len(body.encode()), body))


def post_parse(message):
    """An incoming request (SCXML 1.0, C.2.2): the event, _scxmleventname
    or HTTP.<method>, and the other form fields as a map of text; a body
    without "=" is content, not fields."""
    head, _, body = message.partition("\r\n\r\n")
    method = head.split(" ", 1)[0]
    fields = urllib.parse.parse_qsl(body, keep_blank_values=True) if "=" in body else []
    event = next((value for key, value in fields if key == "_scxmleventname"), "") or "HTTP." + method
    return event, {key: value for key, value in fields if key != "_scxmleventname" and key}


def read_exactly(stream, size):
    """size bytes of a frame; EOF inside a frame raises EOFError."""
    chunks = bytearray()
    while len(chunks) < size:
        chunk = stream.read(size - len(chunks))
        if not chunk:
            raise EOFError("incomplete driver frame")
        chunks.extend(chunk)
    return bytes(chunks)


def answer(adapter, call):
    method = call.get("method")
    function = getattr(adapter, method.replace(".", "_"), None) if isinstance(method, str) else None
    if function is None or method.startswith("_") or method in ("need.host", "remote"):
        return {"id": call["id"], "error": {"code": -32601, "message": "%s is not offered" % method,
                                             "data": {"reason": "unknown", "path": "method"}}}
    try:
        return {"id": call["id"], "result": function(call.get("params") or {})}
    except KeyError as missing:  # a required param left out
        return {"id": call["id"], "error": {"code": -32602, "message": "params.%s: missing" % missing.args[0],
                                             "data": {"reason": "missing", "path": "params.%s" % missing.args[0]}}}
    except DriverError as error:
        return {"id": call["id"], "error": {"code": error.code, "message": str(error),
                                             "data": {"reason": error.reason, "path": error.path}}}


def main():
    source, target = sys.stdin.buffer, sys.stdout.buffer
    adapter = Adapter()
    while True:
        first = source.read(1)
        if not first:
            return
        header = first + read_exactly(source, 3)
        call = tinyactors.decode(read_exactly(source, int.from_bytes(header, "big")))
        encoded = tinyactors.encode(answer(adapter, call))
        target.write(len(encoded).to_bytes(4, "big") + encoded)
        target.flush()


if __name__ == "__main__":
    main()
