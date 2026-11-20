/* The typed methods (AGENTS.md, "The API"; bindings.md, section 10): one
 * per method of tinyactors-core/docs/requests.md, each a few lines over
 * ta_host_post. A method takes its response handler, an optional error
 * handler, and the method's params as keywords, passed through as given
 * (core checks them and answers in the reply). It adds a row
 * {handler, on_error} to the pending table, posts with the row's handle
 * as the message's id, and returns None; the drain that handles the
 * REPLY, a later one, finds the row by the id and calls a handler. */
#include "extension.h"

/* The typed method of `method`: (handler, on_error=None, /, **params), with
 * on_error also taken by keyword, so no param of core's is named on_error. */
typedef struct ta_python_typed {
    const char *method;
    PyObject *arguments;
    PyObject *params;
    int timers; /* its result's timers[].due are converted to seconds (actor.get) */
} ta_python_typed;

static int ta_python_post(ta_python_host *host, const ta_python_typed *typed, uint64_t id) {
    PyObject *params = typed->params ? PyDict_Copy(typed->params) : NULL;
    PyObject *on_error = NULL; /* the error handler, not a param of core's */
    if (typed->params && (!params || PyDict_GetItemStringRef(params, "on_error", &on_error) < 0 ||
                          (on_error && PyDict_DelItemString(params, "on_error") < 0))) {
        Py_XDECREF(on_error);
        Py_XDECREF(params);
        return -1;
    }
    Py_XDECREF(on_error);
    ta_cbor_writer writer;
    ta_python_cbor_init(&writer, &(ta_python_room){{NULL, 0, 0}, &host->arena_allocator});
    int with_params = params && PyDict_Size(params) > 0;
    ta_cbor_write_map(&writer, with_params ? 3 : 2);
    ta_cbor_write_text(&writer, ta_string("method"));
    ta_cbor_write_text(&writer, ta_string(typed->method));
    if (with_params) {
        ta_cbor_write_text(&writer, ta_string("params"));
        int written = ta_python_value_write(host->state, &writer, params);
        Py_DECREF(params);
        if (written < 0) return -1;
    } else Py_XDECREF(params);
    ta_cbor_write_text(&writer, ta_string("id"));
    ta_cbor_write_uint(&writer, id);
    ta_bytes message;
    ta_status status = ta_cbor_writer_bytes(&writer, &message);
    if (status == TA_OK) status = ta_host_post(host->host, message);
    if (status != TA_OK) {
        ta_python_raise_status(host->state, status);
        return -1;
    }
    return 0;
}

static PyObject *ta_python_typed_call(PyObject *self, const ta_python_typed *typed);

/* A typed method given no handler: under asyncio, a future of the host's
 * loop that its reply settles (support.py's settlers make the handler and
 * on_error that resolve and reject it); a method still pending at close()
 * never settles, as its handlers are dropped. Under the synchronous driver
 * there is no loop to settle it on: TypeError. */
static PyObject *ta_python_typed_future(ta_python_host *host, const ta_python_typed *typed) {
    if (typed->params && PyDict_GetItemString(typed->params, "on_error")) {
        PyErr_Format(PyExc_TypeError, "%s: on_error without a handler", typed->method);
        return NULL;
    }
    if (host->loop == Py_None) {
        PyErr_Format(PyExc_TypeError, "%s() with no handler returns an awaitable, which needs the asyncio driver "
                     "(async with Host(...)); under `with`, give it a handler(result)", typed->method);
        return NULL;
    }
    PyObject *future = PyObject_CallMethod(host->loop, "create_future", NULL);
    PyObject *pair = future ? PyObject_CallFunctionObjArgs(host->state->settlers, future, NULL) : NULL;
    PyObject *arguments = pair ? PySequence_Tuple(pair) : NULL;
    Py_XDECREF(pair);
    PyObject *done = NULL;
    if (arguments) {
        ta_python_typed handled = {typed->method, arguments, typed->params, typed->timers};
        done = ta_python_typed_call((PyObject *)host, &handled);
        Py_DECREF(arguments);
    }
    if (!done) {
        Py_XDECREF(future);
        return NULL;
    }
    Py_DECREF(done);
    return future;
}

static PyObject *ta_python_typed_call(PyObject *self, const ta_python_typed *typed) {
    ta_python_host *host = (ta_python_host *)self;
    PyObject *handler = NULL, *on_error = Py_None, *keyword = NULL;
    if (!PyArg_ParseTuple(typed->arguments, "|OO", &handler, &on_error)) return NULL;
    if (!handler) return ta_python_typed_future(host, typed);
    if (typed->params && PyDict_GetItemStringRef(typed->params, "on_error", &keyword) < 0) return NULL;
    if (keyword) {
        int twice = PyTuple_Size(typed->arguments) > 1;
        on_error = keyword;
        Py_DECREF(keyword); /* the params dict holds it for the call */
        if (twice) {
            PyErr_Format(PyExc_TypeError, "%s got on_error twice", typed->method);
            return NULL;
        }
    }
    if (!PyCallable_Check(handler) || (on_error != Py_None && !PyCallable_Check(on_error))) {
        PyErr_Format(PyExc_TypeError, "%s takes a handler(result) and on_error(error) or None", typed->method);
        return NULL;
    }
    ta_python_call call;
    if (ta_python_enter(host, &call) < 0) return NULL;
    uint64_t id = 0;
    ta_python_pending_row *row = ta_table_add(&host->pending, &id);
    int status = row ? 0 : -1;
    if (!row) PyErr_NoMemory();
    else {
        row->handler = Py_NewRef(handler);
        row->on_error = Py_NewRef(on_error);
        row->timers = typed->timers;
        ta_python_advance(host); /* trace.watch's end is a time of the host's clock */
        status = ta_python_post(host, typed, id);
        if (status < 0) { /* refused at the call: nothing was applied, no reply comes */
            row = ta_table_get(&host->pending, id);
            PyObject *dropped_handler = row->handler, *dropped_error = row->on_error;
            ta_table_remove(&host->pending, id);
            Py_DECREF(dropped_handler);
            Py_DECREF(dropped_error);
        } else ta_python_loop_schedule(host);
    }
    ta_python_leave(&call);
    if (status < 0) return NULL;
    Py_RETURN_NONE;
}

#define TA_PYTHON_METHOD(function, name, timers)                                                 \
    PyObject *ta_python_method_##function(PyObject *self, PyObject *arguments, PyObject *keywords) { \
        ta_python_typed typed = {name, arguments, keywords, timers};                             \
        return ta_python_typed_call(self, &typed);                                               \
    }

TA_PYTHON_METHOD(actor_state, "actor.state", 0)
TA_PYTHON_METHOD(actor_context, "actor.context", 0)
TA_PYTHON_METHOD(actor_list, "actor.list", 0)
TA_PYTHON_METHOD(actor_get, "actor.get", 1)
TA_PYTHON_METHOD(host_stats, "host.stats", 0)
TA_PYTHON_METHOD(host_config, "host.config", 0)
TA_PYTHON_METHOD(trace_configure, "trace.configure", 0)
TA_PYTHON_METHOD(trace_watch, "trace.watch", 0)
TA_PYTHON_METHOD(trace_unwatch, "trace.unwatch", 0)
TA_PYTHON_METHOD(trace_end, "trace.end", 0)
TA_PYTHON_METHOD(trace_spans_decode, "trace.spans.decode", 0)

/* actor.get's result: each of its timers' "due", the host's ticks, as
 * seconds on the host's clock (a float, the API's unit of time;
 * bindings.md, section 3). -1 with an exception. */
static int ta_python_timers_in_seconds(ta_python_host *host, PyObject *result) {
    PyObject *timers = NULL;
    if (!PyDict_Check(result) || PyDict_GetItemStringRef(result, "timers", &timers) <= 0) return 0;
    int status = 0;
    Py_ssize_t count = PyList_Check(timers) ? PyList_Size(timers) : 0;
    for (Py_ssize_t i = 0; i < count && status == 0; i++) {
        PyObject *timer = PyList_GetItemRef(timers, i), *due = NULL, *seconds = NULL;
        if (!timer) status = -1;
        else if (PyDict_Check(timer) && PyDict_GetItemStringRef(timer, "due", &due) > 0 && PyLong_Check(due)) {
            uint64_t ticks = PyLong_AsUnsignedLongLong(due);
            if (PyErr_Occurred()) status = -1;
            else if (!(seconds = PyFloat_FromDouble((double)ticks / (double)host->ticks_per_second)) ||
                     PyDict_SetItemString(timer, "due", seconds) < 0)
                status = -1;
        } else if (PyErr_Occurred()) status = -1;
        Py_XDECREF(seconds);
        Py_XDECREF(due);
        Py_XDECREF(timer);
    }
    Py_DECREF(timers);
    return status;
}

/* A REPLY: its id names a pending row (a stale or unknown one is
 * dropped); the result, decoded once, goes to the handler, an error to
 * on_error, or to the driver after the turn. */
void ta_python_reply(ta_python_host *host, uint32_t index) {
    PyObject *reply = ta_python_value_read(host->state, ta_host_effect_bytes(host->host, index, TA_EFFECT_FIELD_WIRE));
    if (!reply) {
        ta_python_host_keep_error(host);
        return;
    }
    PyObject *id = NULL, *result = NULL, *error = NULL;
    if (PyDict_Check(reply)) {
        PyDict_GetItemStringRef(reply, "id", &id);
        PyDict_GetItemStringRef(reply, "result", &result);
        PyDict_GetItemStringRef(reply, "error", &error);
    }
    uint64_t handle = id && PyLong_Check(id) ? PyLong_AsUnsignedLongLong(id) : 0;
    PyErr_Clear();
    ta_python_pending_row *row = handle ? ta_table_get(&host->pending, handle) : NULL;
    if (row) {
        PyObject *handler = row->handler, *on_error = row->on_error;
        int row_timers = row->timers;
        ta_table_remove(&host->pending, handle);
        PyObject *called = NULL;
        if (error) {
            PyObject *exception = ta_python_error_from_reply(host->state, error);
            if (exception && on_error != Py_None)
                called = PyObject_CallFunctionObjArgs(on_error, exception, NULL);
            else if (exception) PyErr_SetObject(host->state->error, exception);
            Py_XDECREF(exception);
        } else if (!(row_timers && result && ta_python_timers_in_seconds(host, result) < 0))
            called = PyObject_CallFunctionObjArgs(handler, result ? result : Py_None, NULL);
        if (!called && error && on_error == Py_None) ta_python_host_keep_error(host); /* the reply's error, nobody to take it */
        else if (!called) ta_python_handler_failed(host, NULL);
        Py_XDECREF(called);
        Py_DECREF(handler);
        Py_DECREF(on_error);
    }
    Py_XDECREF(id);
    Py_XDECREF(result);
    Py_XDECREF(error);
    Py_DECREF(reply);
}

void ta_python_pending_drop(ta_python_host *host) {
    for (uint32_t slot = 0; slot < host->pending.capacity; slot++) {
        ta_python_pending_row *row = ta_table_at(&host->pending, slot);
        if (!row) continue;
        PyObject *handler = row->handler, *on_error = row->on_error;
        ta_table_remove(&host->pending, ta_table_handle(&host->pending, slot));
        Py_XDECREF(handler); /* never called */
        Py_XDECREF(on_error);
    }
}
