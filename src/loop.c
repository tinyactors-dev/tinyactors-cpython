/* The loop (AGENTS.md, "The loop"): embedding.md's take, handle, ack,
 * run as ta_python_drain and ta_python_turn, and the drivers over them:
 * synchronous (run_until_idle, run, SimulatedClock.increment) and asyncio
 * (idle). The one timer lives here: the synchronous driver's sleep, or one
 * asyncio TimerHandle (tools/lint/rules/one-timer-in-loop.yml). */
#include "extension.h"
#include "docs.h"
#include <string.h>

/* Fuel per ta_host_run: a turn is bounded, and the drivers let the rest
 * of the program run between turns. */
#define TA_PYTHON_BUDGET 10000u
/* Batches a drain takes, and runs a turn makes, before it yields: a
 * handler that answers every effect with an input (a ping-pong with an
 * actor, a typed method that posts again) never holds the turn, so the
 * event loop and the signals get theirs (bindings.md, section 3: "Bound
 * each turn"). */
#define TA_PYTHON_BATCHES 64u
#define TA_PYTHON_RUNS 64u

static const uint64_t ta_python_billion = 1000000000u;

/* ---- one drain at a time ------------------------------------------------------- */

/* 1 when it stopped at its bound (the turn yields; effects may be pending).
 * It checks the bound before a take: a take opens a batch, and one left
 * open between turns would take in an output the application pulls then
 * (hibernate, metrics), whose ack would end effects nobody dispatched. */
int ta_python_drain(ta_python_host *host) {
    const ta_host_effect *effects;
    uint32_t count;
    for (uint32_t batches = 0;; batches++) {
        if (batches == TA_PYTHON_BATCHES) return 1;
        if ((count = ta_host_effects(host->host, &effects)) == 0) return 0;
        host->batch = 1;
        for (uint32_t i = 0; i < count; i++) ta_python_dispatch(host, i); /* handlers may call the host:
                                                                             inputs apply now, effects
                                                                             come with the next take */
        ta_host_effects_ack(host->host);  /* everything handed out for the batch ends */
        host->batch = 0;
        host->acks++;                     /* its thin views are stale from here: core keeps no batch counter */
        if (host->depth <= 1) ta_python_datamodel_flush(&host->python); /* outside core, not nested */
    }
}

/* ---- the clocks ---------------------------------------------------------------- */

/* `amount` of a unit with `per_second` of them in a second, as ticks. */
typedef struct ta_python_rate {
    uint64_t amount;
    uint64_t per_second;
} ta_python_rate;

static uint64_t ta_python_ticks(const ta_python_host *host, ta_python_rate rate) {
    uint64_t seconds = rate.amount / rate.per_second, rest = rate.amount % rate.per_second;
    return seconds * host->ticks_per_second + rest * host->ticks_per_second / rate.per_second;
}

/* A duration in seconds (an int or a float, not negative) as nanoseconds:
 * every time the API takes is in seconds, converted with the host's rate. */
int ta_python_nanoseconds(PyObject *seconds, uint64_t *nanoseconds) {
    if (PyLong_Check(seconds)) {
        int overflow = 0;
        long long sign = PyLong_AsLongLongAndOverflow(seconds, &overflow);
        if (sign == -1 && PyErr_Occurred()) return -1;
        if (sign < 0 || overflow < 0) {
            PyErr_SetString(PyExc_ValueError, "a duration is a number of seconds, not negative");
            return -1;
        }
        unsigned long long whole = PyLong_AsUnsignedLongLong(seconds);
        if (PyErr_Occurred()) return -1;
        if (whole > UINT64_MAX / ta_python_billion) {
            PyErr_SetString(PyExc_OverflowError, "a duration of more than 2^64 nanoseconds");
            return -1;
        }
        *nanoseconds = whole * ta_python_billion;
        return 0;
    }
    double value = PyFloat_AsDouble(seconds);
    if (value == -1.0 && PyErr_Occurred()) return -1;
    if (value != value || value < 0) { /* NaN, or negative */
        PyErr_SetString(PyExc_ValueError, "a duration is a number of seconds, not negative");
        return -1;
    }
    double scaled = value * (double)ta_python_billion + 0.5;
    if (scaled >= 18446744073709551616.0) { /* 2^64, and infinity */
        PyErr_SetString(PyExc_OverflowError, "a duration of more than 2^64 nanoseconds");
        return -1;
    }
    *nanoseconds = (uint64_t)scaled;
    return 0;
}

/* Nanoseconds as the host's ticks, rounded up: a timeout shorter than a
 * tick is one tick, never none. */
uint64_t ta_python_ticks_after(const ta_python_host *host, uint64_t nanoseconds) {
    uint64_t ticks = ta_python_ticks(host, (ta_python_rate){nanoseconds, ta_python_billion});
    uint64_t back = ticks / host->ticks_per_second * ta_python_billion +
                    ticks % host->ticks_per_second * ta_python_billion / host->ticks_per_second;
    return back < nanoseconds ? ticks + 1 : ticks;
}

static ta_host_time ta_python_now(ta_python_host *host) {
    if (host->clock != Py_None) { /* the simulated time is the wall clock too */
        uint64_t nanoseconds = ta_python_clock_now(host->clock);
        return (ta_host_time){ta_python_ticks(host, (ta_python_rate){nanoseconds, ta_python_billion}) + host->ticks_offset,
                              nanoseconds};
    }
    uint64_t wall = 0;
    PyObject *now = PyObject_CallNoArgs(host->state->time_ns);
    if (now) {
        wall = PyLong_AsUnsignedLongLong(now);
        Py_DECREF(now);
    }
    if (PyErr_Occurred()) {
        PyErr_Clear(); /* 0: unknown (host.h) */
        wall = 0;
    }
    return (ta_host_time){ta_python_ticks(host, (ta_python_rate){ta_runtime_now_ns(), ta_python_billion}) + host->ticks_offset,
                          wall};
}

/* A restored host's clock goes on from `time`, its snapshot's: the ticks
 * the clock reads are offset to read `time` now (unsigned, so an offset
 * "down" wraps and adds up right), whatever the clock: a new
 * SimulatedClock, or the real one after a reboot, on another machine, or
 * in another process of this one. */
void ta_python_clock_resume(ta_python_host *host, ta_time time) {
    host->ticks_offset = 0;
    host->ticks_offset = time - ta_python_now(host).monotonic;
}

/* Both clocks, before an input that depends on them (bindings.md section
 * 7: spawn, deliver, a posted message such as trace.watch, whose end is a
 * time of the host's clock) and once at creation; a turn does it before
 * every run. */
void ta_python_advance(ta_python_host *host) {
    ta_host_advance_time(host->host, ta_python_now(host));
}

/* Seconds from now to the host's deadline (0 when it is due). */
double ta_python_delay(ta_python_host *host) {
    ta_time now = ta_python_now(host).monotonic;
    if (host->deadline <= now) return 0.0;
    return (double)(host->deadline - now) / (double)host->ticks_per_second;
}

/* ---- one turn ------------------------------------------------------------------- */

static ta_python_next ta_python_turn(ta_python_host *host) {
    if (ta_python_drain(host)) return TA_PYTHON_NEXT_YIELD; /* what inputs and posts appended:
                                                               a pending reply needs no run */
    if (host->retake) { /* a take the allocator refused: again, in a batch of its own */
        host->batch = 1;
        if (host->retake & 1u) ta_python_take(host, 1);
        if (host->retake & 2u) ta_python_take(host, 0);
        ta_host_effects_ack(host->host);
        host->batch = 0;
        host->acks++;
    }
    ta_host_advance_time(host->host, ta_python_now(host)); /* ticks at host.config's rate, wall ns */
    for (uint32_t runs = 0;; runs++) {
        host->inputs = 0;
        ta_run run = ta_host_run(host->host, TA_PYTHON_BUDGET); /* bounded: fuel per call */
        if (host->encoder) ta_python_advance(host); /* the run's span records are stamped between the
                                                       clocks around it, not stretched over the handlers
                                                       that come after it */
        int bounded = ta_python_drain(host) || runs + 1 == TA_PYTHON_RUNS;
        if (bounded && run.status != TA_RUN_EXHAUSTED && run.status != TA_RUN_FAULT)
            return TA_PYTHON_NEXT_YIELD; /* the turn's bound: let the program run, then turn again */
        switch (run.status) {
        case TA_RUN_EFFECTS:      /* a sync I/O processor's send: answered in the drain, or its sender waits */
        case TA_RUN_EFFECTS_FULL: continue; /* run again now */
        case TA_RUN_YIELDED:      return TA_PYTHON_NEXT_YIELD; /* budget spent, actors still runnable */
        case TA_RUN_IDLE:         if (host->inputs) return TA_PYTHON_NEXT_YIELD; /* handlers fed inputs in the
                                                                                    drain: turn again for them */
                                  host->deadline = run.next_deadline; /* UINT64_MAX: none */
                                  return TA_PYTHON_NEXT_IDLE;
        case TA_RUN_EXHAUSTED:    host->pool = run.detail; /* a TA_POOL_* */
                                  return TA_PYTHON_NEXT_EXHAUSTED;
        case TA_RUN_FAULT:        host->fault = (ta_status)run.detail; /* negative: ta_status_name */
                                  return TA_PYTHON_NEXT_FAULT;
        default:                  host->fault = TA_ERROR_UNSUPPORTED; /* a status unknown here */
                                  return TA_PYTHON_NEXT_FAULT;
        }
    }
}

int ta_python_turn_raise(ta_python_host *host, ta_python_next next) {
    if (host->python.raised) { /* a KeyboardInterrupt in chart code: the same object, after the run */
        PyErr_SetRaisedException(host->python.raised);
        host->python.raised = NULL;
        return -1;
    }
    if (host->error) {
        PyErr_SetRaisedException(host->error);
        host->error = NULL;
        return -1;
    }
    if (next == TA_PYTHON_NEXT_EXHAUSTED) {
        PyErr_Format(PyExc_MemoryError, "the host's allocator refused memory for pool %u (TA_POOL_*)", host->pool);
        return -1;
    }
    if (next == TA_PYTHON_NEXT_FAULT) {
        ta_python_raise_status(host->state, host->fault);
        return -1;
    }
    return 0;
}

/* A driver may run: no turn already (a nested drain), no async handler
 * without an event loop. */
static int ta_python_driver_enter(ta_python_host *host, ta_python_call *call) {
    if (ta_python_enter(host, call) < 0) return -1;
    if (host->turning || host->depth > 1) { /* inside another call: its turn or leave runs the loop */
        ta_python_leave(call);
        PyErr_SetString(host->state->error, "the host is already turning: a handler or chart code cannot run the loop");
        return -1;
    }
    return 0;
}

/* Turns until idle at the current time: `forever` sleeps to each deadline
 * and goes on, while there is one (a simulated clock sleeps never). */
static PyObject *ta_python_drive(ta_python_host *host, int forever) {
    ta_python_call call;
    if (ta_python_driver_enter(host, &call) < 0) return NULL;
    if (host->has_async && host->loop == Py_None) {
        ta_python_leave(&call);
        PyErr_SetString(PyExc_TypeError, "a handler is an async def (an I/O processor, wake or telemetry): drive the host with await host.idle()");
        return NULL;
    }
    PyObject *sleep = NULL;
    int status = 0;
    for (;;) {
        host->turning = 1;
        ta_python_next next = ta_python_turn(host);
        host->turning = 0;
        ta_python_datamodel_flush(&host->python);
        if (ta_python_turn_raise(host, next) < 0 || host->closing) {
            status = PyErr_Occurred() ? -1 : 0;
            break;
        }
        if (next == TA_PYTHON_NEXT_YIELD) {
            if (PyErr_CheckSignals() < 0) { /* a KeyboardInterrupt, between turns */
                status = -1;
                break;
            }
            continue;
        }
        if (!forever || host->deadline == UINT64_MAX || host->clock != Py_None) break;
        if (!sleep) {
            PyObject *time = PyImport_ImportModule("time");
            sleep = time ? PyObject_GetAttrString(time, "sleep") : NULL;
            Py_XDECREF(time);
            if (!sleep) {
                status = -1;
                break;
            }
        }
        PyObject *slept = PyObject_CallFunction(sleep, "d", ta_python_delay(host));
        if (!slept) {
            status = -1;
            break;
        }
        Py_DECREF(slept);
    }
    Py_XDECREF(sleep);
    ta_python_leave(&call);
    if (status < 0) return NULL;
    if (host->error && !host->host) { /* a close a handler asked for ran at the leave, and its last take raised */
        PyErr_SetRaisedException(host->error);
        host->error = NULL;
        return NULL;
    }
    Py_RETURN_NONE;
}

PyObject *ta_python_host_run_until_idle(PyObject *self, PyObject *unused) {
    (void)unused;
    return ta_python_drive((ta_python_host *)self, 0);
}

PyObject *ta_python_host_run_forever(PyObject *self, PyObject *unused) {
    (void)unused;
    return ta_python_drive((ta_python_host *)self, 1);
}

/* ---- asyncio ----------------------------------------------------------------------- */

static PyObject *ta_python_loop_turn(PyObject *self, PyObject *unused);

static PyMethodDef ta_python_loop_turn_def = {"turn", ta_python_loop_turn, METH_NOARGS, "One turn of a host."};

static void ta_python_cancel(PyObject **handle) {
    if (*handle && *handle != Py_None) {
        PyObject *done = PyObject_CallMethod(*handle, "cancel", NULL);
        if (!done) PyErr_Clear();
        Py_XDECREF(done);
    }
    ta_python_replace(handle, Py_NewRef(Py_None));
}

/* call_soon(turn) (`delay` < 0) or call_later(delay, turn), on the host's
 * loop: the handle, or NULL with an exception. */
static PyObject *ta_python_loop_call(ta_python_host *host, double delay) {
    PyObject *turn = PyCFunction_New(&ta_python_loop_turn_def, (PyObject *)host);
    if (!turn) return NULL;
    PyObject *handle = delay < 0 ? PyObject_CallMethod(host->loop, "call_soon", "O", turn)
                                 : PyObject_CallMethod(host->loop, "call_later", "dO", delay, turn);
    Py_DECREF(turn);
    return handle;
}

void ta_python_loop_schedule(ta_python_host *host) {
    host->inputs = 1;
    if (host->loop == Py_None || host->turning || host->scheduled != Py_None || !host->host) return;
    PyObject *handle = ta_python_loop_call(host, -1.0);
    if (!handle) {
        ta_python_host_keep_error(host);
        return;
    }
    ta_python_replace(&host->scheduled, handle);
}

/* Settle the waiters of idle(): with the turn's exception if one is set,
 * else with None. */
static void ta_python_loop_settle(ta_python_host *host, int failed) {
    PyObject *exception = failed ? PyErr_GetRaisedException() : NULL;
    PyObject *waiters = host->waiters;
    host->waiters = PyList_New(0);
    if (!host->waiters) {
        host->waiters = waiters;
        Py_XDECREF(exception);
        PyErr_Clear();
        return;
    }
    if (failed && PyList_Size(waiters) == 0) { /* nobody waits: the next idle() raises it */
        if (!host->error) host->error = exception;
        else Py_XDECREF(exception);
        exception = NULL;
    }
    for (Py_ssize_t i = 0; i < PyList_Size(waiters); i++) {
        PyObject *future = PyList_GetItem(waiters, i);
        PyObject *done = PyObject_CallMethod(future, "done", NULL);
        int is_done = done ? PyObject_IsTrue(done) : 1;
        Py_XDECREF(done);
        PyObject *set = is_done ? NULL
                                : exception ? PyObject_CallMethod(future, "set_exception", "O", exception)
                                            : PyObject_CallMethod(future, "set_result", "O", Py_None);
        if (!set) PyErr_Clear();
        Py_XDECREF(set);
    }
    Py_DECREF(waiters);
    Py_XDECREF(exception);
}

static PyObject *ta_python_loop_turn(PyObject *self, PyObject *unused) {
    (void)unused;
    ta_python_host *host = (ta_python_host *)self;
    ta_python_replace(&host->scheduled, Py_NewRef(Py_None));
    ta_python_call call;
    if (!host->host || host->turning || ta_python_enter(host, &call) < 0) {
        PyErr_Clear();
        Py_RETURN_NONE;
    }
    host->turning = 1;
    ta_python_next next = ta_python_turn(host);
    host->turning = 0;
    ta_python_datamodel_flush(&host->python);
    int failed = ta_python_turn_raise(host, next) < 0;
    if (next == TA_PYTHON_NEXT_YIELD && !host->closing) {
        ta_python_loop_schedule(host); /* yield to the loop, then turn again */
        if (failed) ta_python_loop_settle(host, 1);
    } else {
        ta_python_cancel(&host->timer); /* one timer, replacing the last */
        if (next == TA_PYTHON_NEXT_IDLE && host->deadline != UINT64_MAX && host->clock == Py_None && !host->closing) {
            PyObject *handle = ta_python_loop_call(host, ta_python_delay(host));
            if (handle) ta_python_replace(&host->timer, handle);
            else if (!failed) failed = 1;
        }
        ta_python_loop_settle(host, failed);
    }
    ta_python_leave(&call);
    Py_RETURN_NONE;
}

PyObject *ta_python_host_idle(PyObject *self, PyObject *unused) {
    (void)unused;
    ta_python_host *host = (ta_python_host *)self;
    ta_python_call call;
    if (ta_python_enter(host, &call) < 0) return NULL;
    PyObject *future = NULL;
    if (host->loop == Py_None) {
        PyObject *asyncio = PyImport_ImportModule("asyncio");
        PyObject *loop = asyncio ? PyObject_CallMethod(asyncio, "get_running_loop", NULL) : NULL;
        Py_XDECREF(asyncio);
        if (loop) ta_python_replace(&host->loop, loop);
    }
    if (host->loop != Py_None) future = PyObject_CallMethod(host->loop, "create_future", NULL);
    if (future && host->error) { /* an error nobody waited for */
        PyObject *set = PyObject_CallMethod(future, "set_exception", "O", host->error);
        Py_CLEAR(host->error);
        Py_XDECREF(set);
    } else if (future && PyList_Append(host->waiters, future) == 0) {
        host->scheduled == Py_None ? ta_python_loop_schedule(host) : (void)0;
    } else Py_CLEAR(future);
    ta_python_leave(&call);
    return future;
}

void ta_python_loop_stop(ta_python_host *host) {
    if (host->timer) ta_python_cancel(&host->timer);
    if (host->scheduled) ta_python_cancel(&host->scheduled);
    if (!host->waiters) return;
    for (Py_ssize_t i = 0; i < PyList_Size(host->waiters); i++) {
        PyObject *cancelled = PyObject_CallMethod(PyList_GetItem(host->waiters, i), "cancel", NULL);
        if (!cancelled) PyErr_Clear();
        Py_XDECREF(cancelled);
    }
    PyList_SetSlice(host->waiters, 0, PyList_Size(host->waiters), NULL);
}

/* ---- SimulatedClock ------------------------------------------------------------------ */

/* A caller's own clock (bindings.md, section 3): time moves only with
 * increment(seconds), which advances every host made with the clock and turns
 * each until idle. A host made with one arms no timer. */
typedef struct ta_python_clock {
    PyObject_HEAD
    uint64_t nanoseconds;
    PyObject *hosts; /* weak references to the hosts made with it (a list): no cycle through a host */
} ta_python_clock;

uint64_t ta_python_clock_now(PyObject *clock) {
    return ((ta_python_clock *)clock)->nanoseconds;
}

int ta_python_clock_register(PyObject *clock, PyObject *host) {
    PyObject *reference = PyWeakref_NewRef(host, NULL);
    int status = reference ? PyList_Append(((ta_python_clock *)clock)->hosts, reference) : -1;
    Py_XDECREF(reference);
    return status;
}

void ta_python_clock_forget(PyObject *clock, PyObject *host) {
    PyObject *hosts = ((ta_python_clock *)clock)->hosts;
    if (!hosts) return;
    for (Py_ssize_t i = PyList_Size(hosts) - 1; i >= 0; i--) {
        PyObject *referent = NULL;
        if (PyWeakref_GetRef(PyList_GetItem(hosts, i), &referent) < 0) PyErr_Clear();
        if (!referent || referent == host) PyList_SetSlice(hosts, i, i + 1, NULL);
        Py_XDECREF(referent);
    }
}

static PyObject *ta_python_clock_new(PyTypeObject *type, PyObject *arguments, PyObject *keywords) {
    static char *const names[] = {NULL};
    if (!PyArg_ParseTupleAndKeywords(arguments, keywords, ":SimulatedClock", names)) return NULL;
    ta_python_clock *clock = (ta_python_clock *)PyType_GenericAlloc(type, 0);
    if (!clock) return NULL;
    clock->hosts = PyList_New(0);
    if (!clock->hosts) {
        Py_DECREF(clock);
        return NULL;
    }
    return (PyObject *)clock;
}

static PyObject *ta_python_clock_increment(PyObject *self, PyObject *amount) {
    ta_python_clock *clock = (ta_python_clock *)self;
    uint64_t nanoseconds = 0;
    if (ta_python_nanoseconds(amount, &nanoseconds) < 0) return NULL;
    clock->nanoseconds += nanoseconds;
    PyObject *hosts = PyList_GetSlice(clock->hosts, 0, PyList_Size(clock->hosts));
    if (!hosts) return NULL;
    for (Py_ssize_t i = 0; i < PyList_Size(hosts); i++) {
        PyObject *referent = NULL;
        if (PyWeakref_GetRef(PyList_GetItem(hosts, i), &referent) < 0) {
            Py_DECREF(hosts);
            return NULL;
        }
        ta_python_host *host = (ta_python_host *)referent;
        PyObject *done = host && host->host ? ta_python_drive(host, 0) : Py_NewRef(Py_None);
        Py_XDECREF(referent);
        if (!done) {
            Py_DECREF(hosts);
            return NULL;
        }
        Py_DECREF(done);
    }
    Py_DECREF(hosts);
    Py_RETURN_NONE;
}

static PyObject *ta_python_clock_seconds(PyObject *self, void *closure) {
    (void)closure;
    return PyFloat_FromDouble((double)((ta_python_clock *)self)->nanoseconds / (double)ta_python_billion);
}

static int ta_python_clock_traverse(PyObject *self, visitproc visit, void *arg) {
    Py_VISIT(Py_TYPE(self));
    Py_VISIT(((ta_python_clock *)self)->hosts);
    return 0;
}

static int ta_python_clock_clear(PyObject *self) {
    Py_CLEAR(((ta_python_clock *)self)->hosts);
    return 0;
}

static void ta_python_clock_dealloc(PyObject *self) {
    PyTypeObject *type = Py_TYPE(self);
    PyObject_GC_UnTrack(self);
    ta_python_clock_clear(self);
    freefunc ta_python_release = (freefunc)PyType_GetSlot(type, Py_tp_free);
    ta_python_release(self);
    Py_DECREF(type);
}

static PyMethodDef ta_python_clock_methods[] = {
    {"increment", ta_python_clock_increment, METH_O,
     ta_python_doc_clock_increment},
    {NULL, NULL, 0, NULL}
};

static PyGetSetDef ta_python_clock_properties[] = {
    {"now", ta_python_clock_seconds, NULL, ta_python_doc_clock_now, NULL},
    {NULL, NULL, NULL, NULL, NULL}
};

static PyType_Slot ta_python_clock_slots[] = {
    {Py_tp_doc, (void *)ta_python_doc_clock},
    {Py_tp_new, ta_python_clock_new},
    {Py_tp_dealloc, ta_python_clock_dealloc},
    {Py_tp_traverse, ta_python_clock_traverse},
    {Py_tp_clear, ta_python_clock_clear},
    {Py_tp_methods, ta_python_clock_methods},
    {Py_tp_getset, ta_python_clock_properties},
    {0, NULL}
};

static PyType_Spec ta_python_clock_spec = {
    "tinyactors.SimulatedClock", sizeof(ta_python_clock), 0,
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC | Py_TPFLAGS_IMMUTABLETYPE, ta_python_clock_slots
};

int ta_python_clock_init_type(PyObject *module, ta_python_module_state *state) {
    state->clock_type = PyType_FromModuleAndSpec(module, &ta_python_clock_spec, NULL);
    if (!state->clock_type) return -1;
    return PyModule_AddObjectRef(module, "SimulatedClock", state->clock_type);
}
