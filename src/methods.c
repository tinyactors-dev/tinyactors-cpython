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

static PyObject *ta_python_typed_call(PyObject *self, const ta_python_typed *typed) {
    ta_python_host *host = (ta_python_host *)self;
    PyObject *handler, *on_error = Py_None, *keyword = NULL;
    if (!PyArg_ParseTuple(typed->arguments, "O|O", &handler, &on_error)) return NULL;
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
        PyErr_Format(PyExc_TypeError, "%s takes a handler(host, result) and on_error(host, error) or None", typed->method);
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

#define TA_PYTHON_METHOD(function, name)                                                         \
    PyObject *ta_python_method_##function(PyObject *self, PyObject *arguments, PyObject *keywords) { \
        ta_python_typed typed = {name, arguments, keywords};                                     \
        return ta_python_typed_call(self, &typed);                                               \
    }

TA_PYTHON_METHOD(actor_state, "actor.state")
TA_PYTHON_METHOD(actor_context, "actor.context")
TA_PYTHON_METHOD(actor_list, "actor.list")
TA_PYTHON_METHOD(actor_get, "actor.get")
TA_PYTHON_METHOD(host_stats, "host.stats")
TA_PYTHON_METHOD(host_config, "host.config")
TA_PYTHON_METHOD(trace_configure, "trace.configure")
TA_PYTHON_METHOD(trace_watch, "trace.watch")
TA_PYTHON_METHOD(trace_unwatch, "trace.unwatch")
TA_PYTHON_METHOD(trace_end, "trace.end")
TA_PYTHON_METHOD(trace_spans_decode, "trace.spans.decode")

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
        ta_table_remove(&host->pending, handle);
        PyObject *called = NULL;
        if (error) {
            PyObject *exception = ta_python_error_from_reply(host->state, error);
            if (exception && on_error != Py_None)
                called = PyObject_CallFunctionObjArgs(on_error, (PyObject *)host, exception, NULL);
            else if (exception) PyErr_SetObject(host->state->error, exception);
            Py_XDECREF(exception);
        } else called = PyObject_CallFunctionObjArgs(handler, (PyObject *)host, result ? result : Py_None, NULL);
        if (!called) ta_python_host_keep_error(host);
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
