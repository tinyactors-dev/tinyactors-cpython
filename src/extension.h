/* The extension's declarations, shared by its C files (AGENTS.md,
 * "Layout"). Nothing here is a C API of its own: Python calls the module.
 *
 * Every entry point runs with an attached CPython thread state. Python
 * arguments are borrowed for the call; a returned object is a new
 * reference, or NULL with a Python exception set. */
#ifndef TA_PYTHON_EXTENSION_H
#define TA_PYTHON_EXTENSION_H

#include <Python.h>
#include "tinyactors.h"
#include "tinyactors_datamodel_ecmascript.h"
#include "tinyactors_runtime.h"

/* Py_SETREF of the full API: put `value` (stolen) in *slot, then drop
 * what was there. */
static inline void ta_python_replace(PyObject **slot, PyObject *value) {
    PyObject *old = *slot;
    *slot = value;
    Py_XDECREF(old);
}

/* ---- the module ----------------------------------------------------------- */

/* One module instance's types and objects, isolated between interpreters
 * (PEP 489): the module owns these references, its traverse and clear
 * slots visit and release them. Never kept in a global. */
typedef struct ta_python_module_state {
    PyObject *address_type;
    PyObject *undefined_type;
    PyObject *undefined;
    PyObject *host_type;
    PyObject *clock_type;
    PyObject *compiled_type;
    PyObject *chart_type;     /* ChartRef */
    PyObject *send_type;      /* a send to an I/O processor: a thin view or a record */
    PyObject *entry_type;     /* a <log> line: a thin view */
    PyObject *migrated_type; /* a MIGRATED effect: a thin view */
    PyObject *invocation_type; /* an invoked service's invocation: a record (it outlives the ack) */
    PyObject *event_type;     /* _event in a "python" chart */
    PyObject *support;        /* the namespace of src/datamodel/support.py */
    PyObject *error;          /* TinyactorsError, CompileError, ResolveError, Diagnostic */
    PyObject *compile_error;
    PyObject *resolve_error;
    PyObject *diagnostic;
    PyObject *builtins;       /* what a "python" chart's code may call */
    PyObject *attach;         /* support.attach: a chart's class and functions */
    PyObject *check_file;     /* support.check_file: a .py file a resolver read */
    PyObject *event_kinds;    /* ("platform", "internal", "external") */
    PyObject *event_key;      /* "_event", interned: bind_event makes no key per event */
    PyObject *time_ns;        /* time.time_ns: the wall clock */
    PyObject *coroutine_function; /* inspect.iscoroutinefunction */
} ta_python_module_state;

/* The state of the module that defined `type` (a method's defining class,
 * METH_METHOD), or of `module` itself; NULL with an exception before the
 * module was executed. */
ta_python_module_state *ta_python_state_of_type(PyTypeObject *type);
ta_python_module_state *ta_python_codec_state(PyObject *module);

/* ---- errors ---------------------------------------------------------------- */

/* A TinyactorsError for a bare status: `status` named by ta_status_name. */
void ta_python_raise_status(ta_python_module_state *state, ta_status status);
/* A TinyactorsError for a reply's error map ({"code", "message", "data":
 * {"reason", "path"}}): a new reference. */
PyObject *ta_python_error_from_reply(ta_python_module_state *state, PyObject *error);

/* ---- values (values.c) ----------------------------------------------------- */

/* Start a writer in `room` that grows through `allocator`, or through the
 * host's raw allocator when it is NULL (the only place a writer starts,
 * tools/lint/rules/allocation-in-tables-and-host.yml). */
typedef struct ta_python_room {
    ta_buffer room;
    const ta_allocator *allocator;
} ta_python_room;
void ta_python_cbor_init(ta_cbor_writer *writer, const ta_python_room *room);

/* Write one Python value of the value table (docs/values.md) into a
 * started writer: 0, or -1 with an exception. */
int ta_python_value_write(ta_python_module_state *state, ta_cbor_writer *writer, PyObject *value);
/* One whole CBOR item as a Python value: a new reference or NULL. */
PyObject *ta_python_value_read(ta_python_module_state *state, ta_bytes cbor);
/* A copy of a value for another actor: mutable containers (list, dict)
 * copied, immutable values shared, anything outside the table refused
 * (TypeError) and nesting past 64 refused (ValueError). */
PyObject *ta_python_value_copy(ta_python_module_state *state, PyObject *value);

PyObject *ta_python_encode(PyObject *module, PyObject *value);
PyObject *ta_python_decode(PyObject *module, PyObject *value);

/* An immutable actor reference (tag 32). */
typedef struct ta_python_address {
    PyObject_HEAD
    PyObject *text;
} ta_python_address;
int ta_python_value_types_init(PyObject *module);
PyObject *ta_python_address_from_text(ta_python_module_state *state, PyObject *text);

/* ---- the "python" datamodel (src/datamodel/) -------------------------------- */

typedef struct ta_python_host ta_python_host;

/* The datamodel of one host: its tables and its last outputs. It lives in
 * the Host object, and core holds a pointer to it (the binding's
 * `datamodel`) for as long as the host lives. */
typedef struct ta_python_datamodel {
    ta_python_host *host;       /* the Host it is part of */
    ta_python_module_state *state;
    ta_table charts;            /* ta_python_chart_row */
    ta_table sessions;          /* ta_python_session_row */
    ta_table payloads;          /* ta_python_payload_row */
    ta_table loops;             /* ta_python_loop_row: a <foreach> in progress */
    ta_datamodel_environment *environment; /* the evaluation's, while one runs */
    PyObject *namespace;        /* the globals of the chart code: builtins and In() */
    PyObject *capsule;          /* In()'s self: this datamodel, by pointer, no reference to the Host */
    PyObject *drops;            /* values whose last reference was dropped (a list) */
    PyObject *raised;           /* a BaseException an evaluation raised, raised after it */
    PyObject *output;           /* the str eval_string and log answered */
    PyObject *origintypes;      /* the session's I/O processors' types (system variables, the same for
                                   every session of the host), by an event's origintype_id; NULL until
                                   the first create */
    ta_buffer wire;             /* what to_wire, serialize and payload_serialize wrote, from the host's allocator */
} ta_python_datamodel;

const ta_datamodel_vtable *ta_python_datamodel_vtable(void);
/* Tables and namespace of a new host's datamodel: 0, or -1 with an
 * exception. Freed after ta_host_free (the host calls into it until then). */
int ta_python_datamodel_init(ta_python_datamodel *datamodel, ta_python_host *host);
void ta_python_datamodel_free(ta_python_datamodel *datamodel);
int ta_python_datamodel_traverse(ta_python_datamodel *datamodel, visitproc visit, void *arg);
/* Drop the values whose last reference went (bindings.md, section 8):
 * called after the outer call into core returns, never inside it. */
void ta_python_datamodel_flush(ta_python_datamodel *datamodel);
/* The datamodel's state for a host snapshot, as CBOR bytes: the session,
 * payload and loop tables' handle namespaces and every row's values; NULL
 * with TinyactorsError (TA_ERROR_UNSUPPORTED) for a value outside the
 * value table. Restored into a fresh datamodel whose charts a host restore
 * attached: 0, or -1 with an exception. */
PyObject *ta_python_datamodel_snapshot(ta_python_datamodel *datamodel);
int ta_python_datamodel_restore(ta_python_datamodel *datamodel, ta_bytes state);

/* A payload of the datamodel from a Python value the application gives
 * (copied through the value table): 0 and *out, or -1 with an exception. */
int ta_python_payload_new(ta_python_datamodel *datamodel, PyObject *value, ta_payload *out);
/* The value of a payload: a borrowed reference, NULL if there is none. */
PyObject *ta_python_payload_value(ta_python_datamodel *datamodel, ta_payload payload);
void ta_python_payload_release(ta_python_datamodel *datamodel, ta_payload payload);
/* The datamodel's `.py` check of a resolved file (support.check_file). */
int ta_python_check_file(ta_python_module_state *state, PyObject *file, PyObject *source);

/* ---- the host (host.c) ------------------------------------------------------ */

/* The datamodels of every host: the native one first, quickjs beside it
 * (AGENTS.md, "The "python" datamodel"). */
enum { TA_PYTHON_DATAMODEL_PYTHON = 0, TA_PYTHON_DATAMODEL_ECMASCRIPT = 1, TA_PYTHON_DATAMODELS = 2 };

/* A typed method's handlers: a row of the pending table, whose handle is
 * the message's id. */
typedef struct ta_python_pending_row {
    PyObject *handler;
    PyObject *on_error;
} ta_python_pending_row;

struct ta_python_host {
    PyObject_HEAD
    ta_python_module_state *state;
    ta_host *host;                     /* NULL once freed */
    ta_allocator allocator;            /* PyMem_RawMalloc and PyMem_RawFree */
    ta_growable_arena arena;           /* a call's scratch, a mark per call */
    ta_allocator arena_allocator;      /* the arena's view, for a call's writer */
    ta_python_datamodel python;
    ta_datamodel_ecmascript *ecmascript;
    ta_datamodel_binding datamodels[TA_PYTHON_DATAMODELS];
    ta_table pending;                  /* ta_python_pending_row */
    uint64_t acks;                     /* batches acknowledged: a view's batch */
    uint64_t ticks_per_second;         /* host.config's system.ticksPerSecond */
    unsigned long thread;              /* the creating thread */
    ta_time deadline;                  /* the last idle run's next_deadline */
    ta_time ticks_offset;              /* added to the clock's ticks (mod 2^64): a restored host goes on
                                          from its snapshot's time (0 for a new host) */
    size_t ecmascript_room;            /* the last ECMAScript state's length: the next snapshot's guess */
    uint32_t pool;                     /* the last exhausted run's TA_POOL_* */
    ta_status fault;                   /* the last faulted run's status */
    uint32_t depth;                    /* API calls in progress on the stack */
    int turning;                       /* a turn is running: no second one */
    int batch;                         /* a batch of effects is taken and not yet acked: an output taken
                                          now joins it; else it opens a batch of its own, acked at once */
    int closing;                       /* close() asked during a turn */
    int has_async;                     /* an async def I/O processor */
    int inputs;                        /* an input was made since the last run */
    PyObject *processors;              /* per ioProcessors index: (type, handler, async) */
    PyObject *log;                     /* log handler or None */
    PyObject *telemetry;               /* telemetry handler(host, signal, data) or None */
    PyObject *wake;                    /* wake handler(host, session, migration) or None */
    PyObject *migrated;                /* migrated handler(host, migration) or None */
    PyObject *services;                /* dict: an invoke's type -> handler(host, invocation) */
    PyObject *invocations;             /* dict: segment << 32 | token -> (invocation, service), while it runs
                                          (core's keys: no table of ours) */
    PyObject *migration_done;          /* migration_done handler(host, result) or None: every migration's end */
    int wake_async;                    /* it is an async def */
    int telemetry_async;               /* it is an async def */
    int encoder;                       /* the config has "telemetry": the host stamps spans */
    int ending;                        /* the last take at close is running */
    uint32_t retake;                   /* 1: traces, 2: logs: a take the allocator refused, retried next turn */
    PyObject *ending_tasks;            /* async telemetry tasks of the last take, for __aexit__ (a list) */
    PyObject *clock;                   /* a SimulatedClock or None */
    PyObject *loop;                    /* the asyncio loop of async with, or None */
    PyObject *timer;                   /* the one TimerHandle, or None */
    PyObject *scheduled;               /* the call_soon handle of the next turn, or None */
    PyObject *waiters;                 /* futures of idle() (a list) */
    PyObject *error;                   /* the first exception of the turn, raised after it */
    PyObject *weakreferences;          /* CPython's list of weak references to the Host */
};

/* A call into the host: refuses another thread and a freed host, takes
 * the arena's mark; leave releases it and, when the outermost call ends,
 * drops what the datamodel deferred. */
typedef struct ta_python_call {
    ta_python_host *host;
    ta_growable_arena_mark mark;
} ta_python_call;
int ta_python_enter(ta_python_host *host, ta_python_call *call);
void ta_python_leave(ta_python_call *call);

/* A payload for session `session` of its datamodel, or for a new actor
 * of `chart` (session 0): "python", a copy of `value` in a row; quickjs,
 * its CBOR in the call's arena, read by quickjs. None: payload 0. Fills
 * `payload` and `datamodel` (its index, to release it on refusal). */
typedef struct ta_python_payload_for {
    uint64_t session;
    ta_host_chart_ref chart;
    PyObject *value;
    ta_payload payload;   /* out */
    uint32_t datamodel;   /* out */
} ta_python_payload_for;
int ta_python_host_payload(ta_python_host *host, ta_python_payload_for *payload);
/* Give back a payload a call refused: it stayed ours. */
void ta_python_host_payload_release(ta_python_host *host, ta_payload payload, uint32_t datamodel);
/* The datamodel index of a payload a session gets (TA_NIL: none known). */
uint32_t ta_python_host_session_datamodel(ta_python_host *host, uint64_t session);
/* A Python value of a payload of datamodel `datamodel` (an effect's):
 * the "python" row's value itself, or quickjs's decoded. New reference. */
typedef struct ta_python_payload_of {
    ta_payload payload;
    uint32_t datamodel;
} ta_python_payload_of;
PyObject *ta_python_host_payload_value(ta_python_host *host, const ta_python_payload_of *payload);
/* A view of a str's UTF-8 for a call (None: empty): 0, or -1. */
int ta_python_text(PyObject *text, ta_string_view *out);
/* Record the first exception of a turn (the one set now), clearing it. */
void ta_python_host_keep_error(ta_python_host *host);
/* Free the host (ta_host_free) and drop every Python reference it holds;
 * `quiet`: no trace.end and no handler runs (the collector's path). */
void ta_python_host_close(ta_python_host *host, int quiet);
int ta_python_host_init_type(PyObject *module, ta_python_module_state *state);

/* ---- the loop (loop.c) ------------------------------------------------------ */

typedef enum ta_python_next {
    TA_PYTHON_NEXT_YIELD,
    TA_PYTHON_NEXT_IDLE,
    TA_PYTHON_NEXT_EXHAUSTED,
    TA_PYTHON_NEXT_FAULT
} ta_python_next;
int ta_python_drain(ta_python_host *host);
/* Both clocks now: before an input that depends on them, and at creation. */
void ta_python_advance(ta_python_host *host);
double ta_python_delay(ta_python_host *host);
void ta_python_clock_resume(ta_python_host *host, ta_time time);
/* After a driver's turns: raise what the turn kept or what the next step
 * says (MemoryError naming the pool, TinyactorsError for a fault): 0 or -1. */
int ta_python_turn_raise(ta_python_host *host, ta_python_next next);
PyObject *ta_python_host_run_until_idle(PyObject *self, PyObject *unused);
PyObject *ta_python_host_run_forever(PyObject *self, PyObject *unused);
PyObject *ta_python_host_idle(PyObject *self, PyObject *unused);
/* An input made outside a turn under asyncio: schedule one turn. */
void ta_python_loop_schedule(ta_python_host *host);
/* Stop the loop's timer and pending turn (the host is closing). */
void ta_python_loop_stop(ta_python_host *host);
int ta_python_clock_init_type(PyObject *module, ta_python_module_state *state);
/* The SimulatedClock's time in nanoseconds. */
uint64_t ta_python_clock_now(PyObject *clock);
/* A duration in seconds (int or float, not negative) as nanoseconds; -1 with an exception. */
int ta_python_nanoseconds(PyObject *seconds, uint64_t *nanoseconds);
/* Nanoseconds as the host's ticks, rounded up. */
uint64_t ta_python_ticks_after(const ta_python_host *host, uint64_t nanoseconds);
void ta_python_clock_forget(PyObject *clock, PyObject *host);
int ta_python_clock_register(PyObject *clock, PyObject *host);

/* ---- effects (effects.c) ---------------------------------------------------- */

void ta_python_dispatch(ta_python_host *host, uint32_t index);
int ta_python_effects_init_types(PyObject *module, ta_python_module_state *state);
/* Hand an OTLP request the host wrote to the telemetry handler: the
 * view copied once into bytes. `signal` is "traces" or "logs". */
typedef struct ta_python_export {
    const char *signal;
    ta_bytes data;
} ta_python_export;
void ta_python_telemetry(ta_python_host *host, const ta_python_export *export);
/* Take a traces (`traces` 1) or logs request now and hand it over; a take
 * the allocator refuses is kept for a retry (core gives no second effect)
 * and reported. */
void ta_python_take(ta_python_host *host, int traces);
/* A coroutine an async def handler returned (stolen), as a task on the
 * host's loop whose exception is kept for the driver: the task, or NULL
 * with an exception. */
PyObject *ta_python_host_task(ta_python_host *host, PyObject *coroutine);
/* Answer a send's async task that ended in an exception. */
PyObject *ta_python_send_task_done(PyObject *send, PyObject *task);

/* ---- typed methods (methods.c) --------------------------------------------- */

void ta_python_reply(ta_python_host *host, uint32_t index);
/* Drop every pending row: their handlers are never called. */
void ta_python_pending_drop(ta_python_host *host);

/* ---- the compiler (compile.c) ---------------------------------------------- */

/* What to compile (host.c, ta_python_compile): `charts`, or with
 * `manifest` the one system manifest in charts.pointer[0]. */
typedef struct ta_python_compilation {
    ta_source_list charts;
    int manifest;
} ta_python_compilation;
ta_status ta_python_compile(ta_compile_options *options, const ta_python_compilation *compilation,
                            ta_compile_result **out);
PyObject *ta_python_compile_charts(PyObject *module, PyObject *const *args, Py_ssize_t count, PyObject *names);
PyObject *ta_python_compile_system(PyObject *module, PyObject *const *args, Py_ssize_t count, PyObject *names);
int ta_python_compile_init_type(PyObject *module, ta_python_module_state *state);

/* ---- the support namespace (src/datamodel/expressions.c) ------------------- */

int ta_python_support_init(PyObject *module, ta_python_module_state *state);

#endif
