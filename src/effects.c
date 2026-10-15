/* The effects (AGENTS.md, "Lifetimes and thin views"): dispatch of one
 * effect of the open batch, and the objects Python sees of them. A thin
 * view holds the Host, the ack count of its batch and the effect's index,
 * and converts a field on first read; after the ack a read raises
 * TA_ERROR_STALE. A record (an async def processor's send) holds every
 * field, made before the ack. */
#include "extension.h"
#include "docs.h"
#include <string.h>

/* ---- the fields of a view -------------------------------------------------------- */

enum {
    TA_PYTHON_SEND_SESSION, TA_PYTHON_SEND_SENDID, TA_PYTHON_SEND_EVENT, TA_PYTHON_SEND_TARGET,
    TA_PYTHON_SEND_DATA, TA_PYTHON_SEND_TRACEPARENT, TA_PYTHON_SEND_TYPE, TA_PYTHON_SEND_ORIGIN, TA_PYTHON_SEND_FIELDS
};
enum { TA_PYTHON_ENTRY_SESSION, TA_PYTHON_ENTRY_LABEL, TA_PYTHON_ENTRY_TEXT, TA_PYTHON_ENTRY_FIELDS };
enum { TA_PYTHON_MIGRATED_SESSION, TA_PYTHON_MIGRATED_SEGMENT, TA_PYTHON_MIGRATED_CHART, TA_PYTHON_MIGRATED_MIGRATION,
       TA_PYTHON_MIGRATION_FIELDS };
/* every view's fields fit the send's array */
_Static_assert(TA_PYTHON_ENTRY_FIELDS <= TA_PYTHON_SEND_FIELDS && TA_PYTHON_MIGRATION_FIELDS <= TA_PYTHON_SEND_FIELDS,
               "a view's fields outgrow ta_python_view.fields");

/* What every view holds: where its effect is. */
typedef struct ta_python_view {
    PyObject_HEAD
    ta_python_host *host; /* a reference */
    uint64_t acks;        /* the batch: host->acks when it was made */
    uint32_t index;       /* in the batch */
    uint32_t record;      /* 1: every field is held, no batch to go stale */
    uint32_t answered;    /* a send: answered or failed */
    uint32_t processor;   /* a send: its I/O processor's index */
    PyObject *fields[TA_PYTHON_SEND_FIELDS];
} ta_python_view;

/* The view's batch is still open: 0, or -1 with TA_ERROR_STALE. */
static int ta_python_view_live(ta_python_view *view) {
    if (view->record) return 0;
    if (!view->host->host || view->host->acks != view->acks) {
        ta_python_raise_status(view->host->state, TA_ERROR_STALE);
        return -1;
    }
    return 0;
}

/* A text field as a str; empty text as None when `empty_none`. */
typedef struct ta_python_text_field {
    uint32_t field;
    int empty_none;
} ta_python_text_field;

static PyObject *ta_python_effect_text(ta_python_view *view, ta_python_text_field field) {
    ta_string_view text = ta_host_effect_text(view->host->host, view->index, field.field);
    if (!text.length && field.empty_none) return Py_NewRef(Py_None);
    return PyUnicode_DecodeUTF8(text.pointer ? text.pointer : "", text.length, "strict");
}

static PyObject *ta_python_effect_payload(ta_python_view *view) {
    ta_host *core = view->host->host;
    ta_python_payload_of payload = {ta_host_effect_number(core, view->index, TA_EFFECT_FIELD_PAYLOAD),
                                    (uint32_t)ta_host_effect_number(core, view->index, TA_EFFECT_FIELD_DATAMODEL)};
    return ta_python_host_payload_value(view->host, &payload);
}

/* A send's field `which`, converted on first read and cached. */
static PyObject *ta_python_send_read(ta_python_view *view, int which) {
    if (view->fields[which]) return Py_NewRef(view->fields[which]);
    if (ta_python_view_live(view) < 0) return NULL;
    PyObject *value = NULL;
    switch (which) {
    case TA_PYTHON_SEND_SESSION:
        value = PyLong_FromUnsignedLongLong(ta_host_effect_number(view->host->host, view->index, TA_EFFECT_FIELD_SESSION));
        break;
    case TA_PYTHON_SEND_SENDID: value = ta_python_effect_text(view, (ta_python_text_field){TA_EFFECT_FIELD_SENDID, 1}); break;
    case TA_PYTHON_SEND_EVENT: value = ta_python_effect_text(view, (ta_python_text_field){TA_EFFECT_FIELD_EVENT, 0}); break;
    case TA_PYTHON_SEND_TARGET: value = ta_python_effect_text(view, (ta_python_text_field){TA_EFFECT_FIELD_TARGET, 1}); break;
    case TA_PYTHON_SEND_DATA: value = ta_python_effect_payload(view); break;
    case TA_PYTHON_SEND_TRACEPARENT: /* formatted by core on first ask; empty when untraced */
        value = ta_python_effect_text(view, (ta_python_text_field){TA_EFFECT_FIELD_TRACEPARENT, 1});
        break;
    case TA_PYTHON_SEND_ORIGIN: /* the sender's address, formatted by core on first ask */
        value = ta_python_effect_text(view, (ta_python_text_field){TA_EFFECT_FIELD_ORIGIN, 1});
        break;
    default: { /* the I/O processor's type; None for a send to an invoked service */
        PyObject *row = view->processor < (uint32_t)PyList_Size(view->host->processors)
                            ? PyList_GetItem(view->host->processors, view->processor)
                            : NULL;
        value = Py_NewRef(row ? PyTuple_GetItem(row, 0) : Py_None);
    }
    }
    if (value) view->fields[which] = Py_NewRef(value);
    return value;
}

static PyObject *ta_python_entry_read(ta_python_view *view, int which) {
    if (view->fields[which]) return Py_NewRef(view->fields[which]);
    if (ta_python_view_live(view) < 0) return NULL;
    PyObject *value =
        which == TA_PYTHON_ENTRY_SESSION
            ? PyLong_FromUnsignedLongLong(ta_host_effect_number(view->host->host, view->index, TA_EFFECT_FIELD_SESSION))
        : which == TA_PYTHON_ENTRY_LABEL ? ta_python_effect_text(view, (ta_python_text_field){TA_EFFECT_FIELD_AUXILIARY, 0})
                                         : ta_python_effect_text(view, (ta_python_text_field){TA_EFFECT_FIELD_EVENT, 0});
    if (value) view->fields[which] = Py_NewRef(value);
    return value;
}

/* A MIGRATED effect's field: the session, the segment and chart name it
 * moved to (the chart is EVENT's text), its migration. */
static PyObject *ta_python_migrated_read(ta_python_view *view, int which) {
    if (view->fields[which]) return Py_NewRef(view->fields[which]);
    if (ta_python_view_live(view) < 0) return NULL;
    static const uint32_t numbers[] = {TA_EFFECT_FIELD_SESSION, TA_EFFECT_FIELD_SEGMENT, 0, TA_EFFECT_FIELD_MIGRATION};
    PyObject *value = which == TA_PYTHON_MIGRATED_CHART
                          ? ta_python_effect_text(view, (ta_python_text_field){TA_EFFECT_FIELD_EVENT, 0})
                          : PyLong_FromUnsignedLongLong(ta_host_effect_number(view->host->host, view->index, numbers[which]));
    if (value) view->fields[which] = Py_NewRef(value);
    return value;
}

/* ---- the view types ----------------------------------------------------------------- */

static int ta_python_view_traverse(PyObject *self, visitproc visit, void *arg) {
    ta_python_view *view = (ta_python_view *)self;
    Py_VISIT(Py_TYPE(self));
    Py_VISIT(view->host);
    for (int i = 0; i < TA_PYTHON_SEND_FIELDS; i++) Py_VISIT(view->fields[i]);
    return 0;
}

static int ta_python_view_clear(PyObject *self) {
    ta_python_view *view = (ta_python_view *)self;
    Py_CLEAR(view->host);
    for (int i = 0; i < TA_PYTHON_SEND_FIELDS; i++) Py_CLEAR(view->fields[i]);
    return 0;
}

static void ta_python_view_dealloc(PyObject *self) {
    PyTypeObject *type = Py_TYPE(self);
    PyObject_GC_UnTrack(self);
    ta_python_view_clear(self);
    freefunc ta_python_release = (freefunc)PyType_GetSlot(type, Py_tp_free);
    ta_python_release(self);
    Py_DECREF(type);
}

static ta_python_view *ta_python_view_new(ta_python_host *host, PyObject *type, uint32_t index) {
    ta_python_view *view = (ta_python_view *)PyType_GenericAlloc((PyTypeObject *)type, 0);
    if (!view) return NULL;
    view->host = (ta_python_host *)Py_NewRef((PyObject *)host);
    view->acks = host->acks;
    view->index = index;
    return view;
}

#define TA_PYTHON_GETTER(kind, name, which)                                                                  \
    static PyObject *ta_python_##kind##_##name(PyObject *self, void *closure) {                             \
        (void)closure;                                                                                       \
        return ta_python_##kind##_read((ta_python_view *)self, which);                                        \
    }

TA_PYTHON_GETTER(send, session, TA_PYTHON_SEND_SESSION)
TA_PYTHON_GETTER(send, sendid, TA_PYTHON_SEND_SENDID)
TA_PYTHON_GETTER(send, event, TA_PYTHON_SEND_EVENT)
TA_PYTHON_GETTER(send, target, TA_PYTHON_SEND_TARGET)
TA_PYTHON_GETTER(send, data, TA_PYTHON_SEND_DATA)
TA_PYTHON_GETTER(send, traceparent, TA_PYTHON_SEND_TRACEPARENT)
TA_PYTHON_GETTER(send, type, TA_PYTHON_SEND_TYPE)
TA_PYTHON_GETTER(send, origin, TA_PYTHON_SEND_ORIGIN)
TA_PYTHON_GETTER(entry, session, TA_PYTHON_ENTRY_SESSION)
TA_PYTHON_GETTER(entry, label, TA_PYTHON_ENTRY_LABEL)
TA_PYTHON_GETTER(entry, text, TA_PYTHON_ENTRY_TEXT)
TA_PYTHON_GETTER(migrated, session, TA_PYTHON_MIGRATED_SESSION)
TA_PYTHON_GETTER(migrated, segment, TA_PYTHON_MIGRATED_SEGMENT)
TA_PYTHON_GETTER(migrated, chart, TA_PYTHON_MIGRATED_CHART)
TA_PYTHON_GETTER(migrated, migration, TA_PYTHON_MIGRATED_MIGRATION)

/* ---- answering a send ------------------------------------------------------------------ */

/* The answer or the failure of a send: ta_host_send to its session with
 * its sendid (bindings.md, section 4). `name` NULL: the failure. */
typedef struct ta_python_answer {
    PyObject *name, *data, *traceparent;
    PyObject *origin, *raw; /* where to reply, the transport's message: NULL for none */
} ta_python_answer;

static int ta_python_send_answer_with(ta_python_view *view, const ta_python_answer *answer) {
    if (ta_python_view_live(view) < 0) return -1; /* a thin view answers before its ack, whatever was read */
    if (view->answered) {
        PyErr_SetString(view->host->state->error, "the send was answered already");
        return -1;
    }
    PyObject *session = ta_python_send_read(view, TA_PYTHON_SEND_SESSION);
    PyObject *sendid = session ? ta_python_send_read(view, TA_PYTHON_SEND_SENDID) : NULL;
    PyObject *type = sendid ? ta_python_send_read(view, TA_PYTHON_SEND_TYPE) : NULL;
    int status = type ? 0 : -1;
    ta_python_call call;
    if (status == 0) status = ta_python_enter(view->host, &call);
    if (status == 0) {
        ta_python_host *host = view->host;
        ta_event_in event;
        memset(&event, 0, sizeof event);
        event.size = sizeof event;
        event.session = PyLong_AsUnsignedLongLong(session);
        ta_python_payload_for payload = {event.session, {0, 0}, answer->data, 0, TA_NIL};
        if (ta_python_text(sendid, &event.sendid) < 0 || ta_python_text(answer->traceparent, &event.traceparent) < 0)
            status = -1;
        else if (!answer->name) event.flags = TA_EVENT_IN_ERROR_COMMUNICATION; /* the name is not repeated */
        else if (ta_python_text(answer->name, &event.name) < 0 || ta_python_text(type, &event.origintype) < 0 ||
                 ta_python_text(answer->origin, &event.origin) < 0 || ta_python_text(answer->raw, &event.raw) < 0 ||
                 ta_python_host_payload(host, &payload) < 0)
            status = -1;
        if (status == 0) {
            event.data = payload.payload;
            ta_status sent = ta_host_send(host->host, &event);
            if (sent != TA_OK) {
                ta_python_host_payload_release(host, payload.payload, payload.datamodel);
                ta_python_raise_status(host->state, sent);
                status = -1;
            } else {
                view->answered = 1;
                ta_python_loop_schedule(host);
            }
        }
        ta_python_leave(&call);
    }
    Py_XDECREF(session);
    Py_XDECREF(sendid);
    Py_XDECREF(type);
    return status;
}

static PyObject *ta_python_send_answer(PyObject *self, PyObject *arguments, PyObject *keywords) {
    ta_python_view *view = (ta_python_view *)self;
    PyObject *name, *data = Py_None, *traceparent = NULL, *origin = NULL, *raw = NULL;
    static char *const names[] = {"event", "data", "traceparent", "origin", "raw", NULL};
    if (!PyArg_ParseTupleAndKeywords(arguments, keywords, "U|O$OOO:answer", names, &name, &data, &traceparent, &origin,
                                     &raw))
        return NULL;
    PyObject *kept = traceparent ? Py_NewRef(traceparent) : ta_python_send_read(view, TA_PYTHON_SEND_TRACEPARENT);
    if (!kept) return NULL;
    ta_python_answer answer = {name, data, kept, origin, raw};
    int status = ta_python_send_answer_with(view, &answer);
    Py_DECREF(kept);
    if (status < 0) return NULL;
    Py_RETURN_NONE;
}

static PyObject *ta_python_send_fail(PyObject *self, PyObject *unused) {
    (void)unused;
    ta_python_view *view = (ta_python_view *)self;
    PyObject *traceparent = ta_python_send_read(view, TA_PYTHON_SEND_TRACEPARENT);
    if (!traceparent) return NULL;
    ta_python_answer answer = {NULL, Py_None, traceparent, NULL, NULL};
    int status = ta_python_send_answer_with(view, &answer);
    Py_DECREF(traceparent);
    if (status < 0) return NULL;
    Py_RETURN_NONE;
}

/* A handler raised before answering: fail the send for it (ADR 0009: a
 * runtime that cannot do what an effect asks answers with a failure); the
 * exception the handler raised stays set. */
static void ta_python_send_failed(ta_python_view *view) {
    if (view->answered) return;
    PyObject *raised = PyErr_GetRaisedException();
    PyObject *done = ta_python_send_fail((PyObject *)view, NULL);
    Py_XDECREF(done);
    PyErr_Clear();
    PyErr_SetRaisedException(raised);
}

static PyObject *ta_python_view_repr(PyObject *self) {
    ta_python_view *view = (ta_python_view *)self;
    return PyUnicode_FromFormat("<%s %s, effect %u of batch %llu>", Py_TYPE(self) == (PyTypeObject *)view->host->state->send_type ? "send" : "view",
                                view->record ? "record" : "view", view->index, (unsigned long long)view->acks);
}

static PyGetSetDef ta_python_send_properties[] = {
    {"session", ta_python_send_session, NULL, ta_python_doc_send_session, NULL},
    {"sendid", ta_python_send_sendid, NULL, ta_python_doc_send_sendid, NULL},
    {"event", ta_python_send_event, NULL, ta_python_doc_send_event, NULL},
    {"target", ta_python_send_target, NULL, ta_python_doc_send_target, NULL},
    {"data", ta_python_send_data, NULL, ta_python_doc_send_data, NULL},
    {"traceparent", ta_python_send_traceparent, NULL, ta_python_doc_send_traceparent, NULL},
    {"type", ta_python_send_type, NULL, ta_python_doc_send_type, NULL},
    {"origin", ta_python_send_origin, NULL, ta_python_doc_send_origin, NULL},
    {NULL, NULL, NULL, NULL, NULL}
};

static PyMethodDef ta_python_send_methods[] = {
    {"answer", (PyCFunction)(void (*)(void))ta_python_send_answer, METH_VARARGS | METH_KEYWORDS,
     ta_python_doc_send_answer},
    {"fail", ta_python_send_fail, METH_NOARGS,
     ta_python_doc_send_fail},
    {NULL, NULL, 0, NULL}
};

static PyType_Slot ta_python_send_slots[] = {
    {Py_tp_doc, (void *)ta_python_doc_send},
    {Py_tp_dealloc, ta_python_view_dealloc},
    {Py_tp_traverse, ta_python_view_traverse},
    {Py_tp_clear, ta_python_view_clear},
    {Py_tp_getset, ta_python_send_properties},
    {Py_tp_methods, ta_python_send_methods},
    {Py_tp_repr, ta_python_view_repr},
    {0, NULL}
};

static PyGetSetDef ta_python_entry_properties[] = {
    {"session", ta_python_entry_session, NULL, ta_python_doc_entry_session, NULL},
    {"label", ta_python_entry_label, NULL, ta_python_doc_entry_label, NULL},
    {"text", ta_python_entry_text, NULL, ta_python_doc_entry_text, NULL},
    {NULL, NULL, NULL, NULL, NULL}
};

static PyType_Slot ta_python_entry_slots[] = {
    {Py_tp_doc, (void *)ta_python_doc_entry},
    {Py_tp_dealloc, ta_python_view_dealloc},
    {Py_tp_traverse, ta_python_view_traverse},
    {Py_tp_clear, ta_python_view_clear},
    {Py_tp_getset, ta_python_entry_properties},
    {0, NULL}
};


static PyGetSetDef ta_python_migrated_properties[] = {
    {"session", ta_python_migrated_session, NULL, ta_python_doc_migrated_session, NULL},
    {"segment", ta_python_migrated_segment, NULL, ta_python_doc_migrated_segment, NULL},
    {"chart", ta_python_migrated_chart, NULL, ta_python_doc_migrated_chart, NULL},
    {"migration", ta_python_migrated_migration, NULL, ta_python_doc_migrated_migration, NULL},
    {NULL, NULL, NULL, NULL, NULL}
};

static PyType_Slot ta_python_migrated_slots[] = {
    {Py_tp_doc, (void *)ta_python_doc_migrated},
    {Py_tp_dealloc, ta_python_view_dealloc},
    {Py_tp_traverse, ta_python_view_traverse},
    {Py_tp_clear, ta_python_view_clear},
    {Py_tp_getset, ta_python_migrated_properties},
    {0, NULL}
};

#define TA_PYTHON_VIEW_FLAGS (Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC | Py_TPFLAGS_IMMUTABLETYPE | Py_TPFLAGS_DISALLOW_INSTANTIATION)
static PyType_Spec ta_python_send_spec = {"tinyactors.Send", sizeof(ta_python_view), 0, TA_PYTHON_VIEW_FLAGS, ta_python_send_slots};
static PyType_Spec ta_python_entry_spec = {"tinyactors.LogEntry", sizeof(ta_python_view), 0, TA_PYTHON_VIEW_FLAGS, ta_python_entry_slots};
static PyType_Spec ta_python_migrated_spec = {"tinyactors.Migrated", sizeof(ta_python_view), 0, TA_PYTHON_VIEW_FLAGS,
                                               ta_python_migrated_slots};

/* ---- dispatch -------------------------------------------------------------------------- */

static PyMethodDef ta_python_task_done_def = {"done", ta_python_send_task_done, METH_O, "An async processor's task ended."};

/* An async def processor's task ended: one that raised before answering
 * has its send failed, and the exception is kept for the driver. */
PyObject *ta_python_send_task_done(PyObject *send, PyObject *task) {
    ta_python_view *view = (ta_python_view *)send;
    PyObject *cancelled = PyObject_CallMethod(task, "cancelled", NULL);
    int was_cancelled = cancelled ? PyObject_IsTrue(cancelled) : 1;
    Py_XDECREF(cancelled);
    PyObject *exception = was_cancelled ? NULL : PyObject_CallMethod(task, "exception", NULL);
    if (exception && exception != Py_None && view->host->host) {
        if (!view->answered) {
            PyObject *done = ta_python_send_fail(send, NULL);
            Py_XDECREF(done);
            PyErr_Clear();
        }
        if (!view->host->error) view->host->error = Py_NewRef(exception);
        ta_python_loop_schedule(view->host); /* the turn hands it to idle() */
    }
    Py_XDECREF(exception);
    PyErr_Clear();
    Py_RETURN_NONE;
}

static PyObject *ta_python_telemetry_done(PyObject *host, PyObject *task);

static PyMethodDef ta_python_telemetry_done_def = {"done", ta_python_telemetry_done, METH_O,
                                                   "An async telemetry handler's task ended."};

/* An async handler's task ended (telemetry, wake): its exception is kept
 * for the driver, as a plain handler's is. */
static PyObject *ta_python_telemetry_done(PyObject *self, PyObject *task) {
    ta_python_host *host = (ta_python_host *)self;
    PyObject *cancelled = PyObject_CallMethod(task, "cancelled", NULL);
    int was_cancelled = cancelled ? PyObject_IsTrue(cancelled) : 1;
    Py_XDECREF(cancelled);
    PyObject *exception = was_cancelled ? NULL : PyObject_CallMethod(task, "exception", NULL);
    if (exception && exception != Py_None && host->host) {
        if (!host->error) host->error = Py_NewRef(exception);
        ta_python_loop_schedule(host);
    }
    Py_XDECREF(exception);
    PyErr_Clear();
    Py_RETURN_NONE;
}

PyObject *ta_python_host_task(ta_python_host *host, PyObject *coroutine) {
    PyObject *task = host->loop != Py_None ? PyObject_CallMethod(host->loop, "create_task", "O", coroutine) : NULL;
    PyObject *done = task ? PyCFunction_New(&ta_python_telemetry_done_def, (PyObject *)host) : NULL;
    PyObject *added = done ? PyObject_CallMethod(task, "add_done_callback", "O", done) : NULL;
    if (!task && !PyErr_Occurred()) {
        PyObject *closed = PyObject_CallMethod(coroutine, "close", NULL); /* no "never awaited" warning */
        Py_XDECREF(closed);
        PyErr_SetString(PyExc_RuntimeError, "an async def handler needs async with");
    }
    Py_XDECREF(done);
    Py_DECREF(coroutine);
    if (!added) Py_CLEAR(task);
    Py_XDECREF(added);
    return task;
}

/* A WAKE: an event or a due delayed send came for a hibernated session
 * (one effect per hibernation, however many come), or a migration
 * asks for it (MIGRATION: its id; None to Python otherwise). The application's handler
 * loads the session's blob and calls host.wake(session, blob), now or
 * later (an async def handler runs as a task); the events wait in its
 * mailbox. */
static void ta_python_dispatch_wake(ta_python_host *host, uint32_t index) {
    if (host->wake == Py_None) return;
    PyObject *session = PyLong_FromUnsignedLongLong(ta_host_effect_number(host->host, index, TA_EFFECT_FIELD_SESSION));
    uint64_t id = ta_host_effect_number(host->host, index, TA_EFFECT_FIELD_MIGRATION); /* 0: no migration asked */
    PyObject *migration = !session ? NULL : id ? PyLong_FromUnsignedLongLong(id) : Py_NewRef(Py_None);
    PyObject *result = migration ? PyObject_CallFunctionObjArgs(host->wake, (PyObject *)host, session, migration, NULL) : NULL;
    if (result && host->wake_async) result = ta_python_host_task(host, result);
    if (!result) ta_python_host_keep_error(host);
    Py_XDECREF(result);
    Py_XDECREF(migration);
    Py_XDECREF(session);
}

/* The application's exporter gets the request as bytes it keeps (one copy,
 * boundary.md section 7); posting it, retrying and capping what waits are
 * the application's. Without a handler the request is dropped. */
void ta_python_telemetry(ta_python_host *host, const ta_python_export *export) {
    if (!host->telemetry || host->telemetry == Py_None) return;
    PyObject *signal = PyUnicode_InternFromString(export->signal);
    PyObject *data = signal ? PyBytes_FromStringAndSize(export->data.pointer, (Py_ssize_t)export->data.length) : NULL;
    PyObject *result = data ? PyObject_CallFunctionObjArgs(host->telemetry, (PyObject *)host, signal, data, NULL) : NULL;
    if (result && host->telemetry_async) { /* a coroutine: a task on the host's loop */
        PyObject *task = ta_python_host_task(host, result);
        if (task && host->ending && host->ending_tasks) PyList_Append(host->ending_tasks, task); /* __aexit__ awaits */
        result = task;
    }
    if (!result) ta_python_host_keep_error(host);
    Py_XDECREF(result);
    Py_XDECREF(data);
    Py_XDECREF(signal);
}

void ta_python_take(ta_python_host *host, int traces) {
    ta_bytes taken; /* the host's view, until the ack */
    ta_status status = traces ? ta_host_traces(host->host, &taken) : ta_host_logs(host->host, &taken);
    uint32_t bit = traces ? 1u : 2u;
    if (status == TA_OK) {
        host->retake &= ~bit;
        if (taken.length) ta_python_telemetry(host, &(ta_python_export){traces ? "traces" : "logs", taken});
    } else if (status == TA_ERROR_NO_MEMORY && !(host->retake & bit)) {
        host->retake |= bit; /* the request stays staged; no other effect comes until a take */
        ta_python_raise_status(host->state, status);
        ta_python_host_keep_error(host);
    }
}

static void ta_python_dispatch_service_send(ta_python_host *host, uint32_t index);

static void ta_python_dispatch_send(ta_python_host *host, uint32_t index) {
    uint32_t processor = (uint32_t)ta_host_effect_number(host->host, index, TA_EFFECT_FIELD_IO_PROCESSOR);
    if (processor == TA_NIL) { /* a send to an invoked service: by (segment, token) */
        ta_python_dispatch_service_send(host, index);
        return;
    }
    PyObject *row = processor < (uint32_t)PyList_Size(host->processors) ? PyList_GetItem(host->processors, processor) : NULL;
    if (!row) return;
    ta_python_view *view = ta_python_view_new(host, host->state->send_type, index);
    if (!view) {
        ta_python_host_keep_error(host);
        return;
    }
    view->processor = processor;
    PyObject *handler = PyTuple_GetItem(row, 1);
    if (handler == Py_None) { /* no handler: a failed completion */
        PyObject *done = ta_python_send_fail((PyObject *)view, NULL);
        if (!done) ta_python_host_keep_error(host);
        Py_XDECREF(done);
    } else if (PyTuple_GetItem(row, 2) == Py_True) { /* async def: a record, then a task */
        int status = 0;
        for (int which = 0; which < TA_PYTHON_SEND_FIELDS && status == 0; which++) {
            PyObject *value = ta_python_send_read(view, which);
            if (!value) status = -1;
            Py_XDECREF(value);
        }
        view->record = 1;
        PyObject *coroutine = status == 0 ? PyObject_CallFunctionObjArgs(handler, (PyObject *)host, (PyObject *)view, NULL) : NULL;
        PyObject *task = coroutine ? PyObject_CallMethod(host->loop, "create_task", "O", coroutine) : NULL;
        PyObject *done = task ? PyCFunction_New(&ta_python_task_done_def, (PyObject *)view) : NULL;
        PyObject *added = done ? PyObject_CallMethod(task, "add_done_callback", "O", done) : NULL;
        if (!added) {
            ta_python_send_failed(view);
            ta_python_host_keep_error(host);
        }
        Py_XDECREF(coroutine);
        Py_XDECREF(task);
        Py_XDECREF(done);
        Py_XDECREF(added);
    } else {
        PyObject *result = PyObject_CallFunctionObjArgs(handler, (PyObject *)host, (PyObject *)view, NULL);
        if (!result) {
            ta_python_send_failed(view);
            ta_python_host_keep_error(host);
        }
        Py_XDECREF(result);
    }
    Py_DECREF(view);
}

/* A log line to its handler, as a thin view. */
typedef struct ta_python_handler {
    PyObject *handler; /* None: the effect is dropped */
    PyObject *type;    /* the view's */
} ta_python_handler;

static void ta_python_dispatch_view(ta_python_host *host, ta_python_handler handler, uint32_t index) {
    if (handler.handler == Py_None) return;
    ta_python_view *view = ta_python_view_new(host, handler.type, index);
    PyObject *result = view ? PyObject_CallFunctionObjArgs(handler.handler, (PyObject *)host, (PyObject *)view, NULL) : NULL;
    if (!result) ta_python_host_keep_error(host);
    Py_XDECREF(result);
    Py_XDECREF((PyObject *)view);
}

/* ---- invoked services: the application runs them -------------------------------------- */

/* An invocation: what its INVOKE said, read once (it outlives the ack: the
 * service runs until done or cancelled), and how to answer the invoker. */
typedef struct ta_python_invocation {
    PyObject_HEAD
    ta_python_host *host; /* a reference */
    uint64_t session;     /* the invoker */
    uint32_t segment, token;
    uint32_t ended;       /* done, cancelled or failed: answers refused */
    uint32_t failed;      /* it never ran: sends to it are answered with error.communication */
    uint32_t restored;    /* started again after a restore (TA_EFFECT_FLAG_RESTORED) */
    uint32_t pad;
    PyObject *invokeid, *type, *src, *data, *traceparent;
} ta_python_invocation;

/* The key of an invocation: core's (segment, token). */
static PyObject *ta_python_invocation_key(uint32_t segment, uint32_t token) {
    return PyLong_FromUnsignedLongLong((uint64_t)segment << 32 | token);
}

/* An event from the service to its invoker (TA_EVENT_IN_FROM_INVOKE), its
 * done (TA_EVENT_IN_INVOKE_DONE), or its failure to start
 * (TA_EVENT_IN_ERROR_COMMUNICATION). */
typedef struct ta_python_service_event {
    uint32_t flags;
    PyObject *name; /* NULL: none (done, failure) */
    PyObject *data;
    PyObject *traceparent;
    ta_string_view sendid; /* error.communication: the failed send's */
} ta_python_service_event;

/* The invocation failed (it could not start, or crashed): error.communication
 * for the invoker, and core marks the invocation over, so a restore does not
 * announce it again. */
#define TA_PYTHON_INVOCATION_FAILED (TA_EVENT_IN_ERROR_COMMUNICATION | TA_EVENT_IN_FROM_INVOKE)

static int ta_python_invocation_post(ta_python_invocation *invocation, const ta_python_service_event *what) {
    ta_python_host *host = invocation->host;
    if ((invocation->ended && !(what->flags & TA_EVENT_IN_ERROR_COMMUNICATION)) || !host->host) {
        ta_python_raise_status(host->state, TA_ERROR_STALE); /* done, cancelled, or the host is gone */
        return -1;
    }
    ta_python_call call;
    if (ta_python_enter(host, &call) < 0) return -1;
    ta_event_in event;
    memset(&event, 0, sizeof event);
    event.size = sizeof event;
    event.session = invocation->session;
    event.flags = what->flags;
    event.token = invocation->token;
    event.sendid = what->sendid;
    ta_python_payload_for payload = {event.session, {0, 0}, what->data, 0, TA_NIL};
    int status = 0;
    if ((what->name && ta_python_text(what->name, &event.name) < 0) || ta_python_text(what->traceparent, &event.traceparent) < 0 ||
        (what->data != Py_None && ta_python_host_payload(host, &payload) < 0))
        status = -1;
    if (status == 0) {
        event.data = payload.payload;
        ta_status sent = ta_host_send(host->host, &event);
        if (sent != TA_OK) {
            ta_python_host_payload_release(host, payload.payload, payload.datamodel);
            ta_python_raise_status(host->state, sent);
            status = -1;
        } else ta_python_loop_schedule(host);
    }
    ta_python_leave(&call);
    return status;
}

/* The invocation ended (done, cancelled, failed): its entry goes, and with
 * it the host's reference on the service. */
static void ta_python_invocation_end(ta_python_invocation *invocation) {
    invocation->ended = 1;
    ta_python_host *host = invocation->host;
    PyObject *key = host->invocations ? ta_python_invocation_key(invocation->segment, invocation->token) : NULL;
    if (key && PyDict_DelItem(host->invocations, key) < 0) PyErr_Clear(); /* not there: never stored */
    Py_XDECREF(key);
}

static PyObject *ta_python_invocation_send_back(PyObject *self, PyObject *arguments, PyObject *keywords) {
    static char *const names[] = {"event", "data", "traceparent", NULL};
    ta_python_invocation *invocation = (ta_python_invocation *)self;
    PyObject *name, *data = Py_None, *traceparent = NULL;
    if (!PyArg_ParseTupleAndKeywords(arguments, keywords, "U|O$O:send_back", names, &name, &data, &traceparent)) return NULL;
    ta_python_service_event what = {TA_EVENT_IN_FROM_INVOKE, name, data, traceparent ? traceparent : invocation->traceparent, {0, 0}};
    if (ta_python_invocation_post(invocation, &what) < 0) return NULL;
    Py_RETURN_NONE;
}

static PyObject *ta_python_invocation_done(PyObject *self, PyObject *arguments, PyObject *keywords) {
    static char *const names[] = {"data", "traceparent", NULL};
    ta_python_invocation *invocation = (ta_python_invocation *)self;
    PyObject *data = Py_None, *traceparent = NULL;
    if (!PyArg_ParseTupleAndKeywords(arguments, keywords, "|O$O:done", names, &data, &traceparent)) return NULL;
    ta_python_service_event what = {TA_EVENT_IN_INVOKE_DONE, NULL, data, traceparent ? traceparent : invocation->traceparent, {0, 0}};
    if (ta_python_invocation_post(invocation, &what) < 0) return NULL;
    ta_python_invocation_end(invocation);
    Py_RETURN_NONE;
}

static PyObject *ta_python_invocation_session(PyObject *self, void *closure) {
    (void)closure;
    return PyLong_FromUnsignedLongLong(((ta_python_invocation *)self)->session);
}

/* An object field of the record; the getset's closure is its offset. */
static PyObject *ta_python_invocation_field(PyObject *self, void *closure) {
    return Py_NewRef(*(PyObject **)((char *)self + (size_t)closure));
}

static PyObject *ta_python_invocation_restored(PyObject *self, void *closure) {
    (void)closure;
    return PyBool_FromLong(((ta_python_invocation *)self)->restored);
}

static PyObject *ta_python_invocation_running(PyObject *self, void *closure) {
    (void)closure;
    ta_python_invocation *invocation = (ta_python_invocation *)self;
    return PyBool_FromLong(!invocation->ended && invocation->host->host);
}

static int ta_python_invocation_traverse(PyObject *self, visitproc visit, void *arg) {
    ta_python_invocation *invocation = (ta_python_invocation *)self;
    Py_VISIT(Py_TYPE(self));
    Py_VISIT((PyObject *)invocation->host);
    Py_VISIT(invocation->data);
    return 0;
}

static int ta_python_invocation_clear(PyObject *self) {
    ta_python_invocation *invocation = (ta_python_invocation *)self;
    Py_CLEAR(invocation->host);
    Py_CLEAR(invocation->invokeid);
    Py_CLEAR(invocation->type);
    Py_CLEAR(invocation->src);
    Py_CLEAR(invocation->data);
    Py_CLEAR(invocation->traceparent);
    return 0;
}

static void ta_python_invocation_dealloc(PyObject *self) {
    PyTypeObject *type = Py_TYPE(self);
    PyObject_GC_UnTrack(self);
    ta_python_invocation_clear(self);
    freefunc ta_python_release = (freefunc)PyType_GetSlot(type, Py_tp_free);
    ta_python_release(self);
    Py_DECREF(type);
}

static PyGetSetDef ta_python_invocation_properties[] = {
    {"session", ta_python_invocation_session, NULL, ta_python_doc_invocation_session, NULL},
    {"invokeid", ta_python_invocation_field, NULL, ta_python_doc_invocation_invokeid, (void *)offsetof(ta_python_invocation, invokeid)},
    {"type", ta_python_invocation_field, NULL, ta_python_doc_invocation_type, (void *)offsetof(ta_python_invocation, type)},
    {"src", ta_python_invocation_field, NULL, ta_python_doc_invocation_src, (void *)offsetof(ta_python_invocation, src)},
    {"data", ta_python_invocation_field, NULL, ta_python_doc_invocation_data, (void *)offsetof(ta_python_invocation, data)},
    {"traceparent", ta_python_invocation_field, NULL, ta_python_doc_invocation_traceparent, (void *)offsetof(ta_python_invocation, traceparent)},
    {"running", ta_python_invocation_running, NULL, ta_python_doc_invocation_running, NULL},
    {"restored", ta_python_invocation_restored, NULL, ta_python_doc_invocation_restored, NULL},
    {NULL, NULL, NULL, NULL, NULL}
};

static PyMethodDef ta_python_invocation_methods[] = {
    {"send_back", (PyCFunction)(void (*)(void))ta_python_invocation_send_back, METH_VARARGS | METH_KEYWORDS,
     ta_python_doc_invocation_send_back},
    {"done", (PyCFunction)(void (*)(void))ta_python_invocation_done, METH_VARARGS | METH_KEYWORDS,
     ta_python_doc_invocation_done},
    {NULL, NULL, 0, NULL}
};

static PyType_Slot ta_python_invocation_slots[] = {
    {Py_tp_doc, (void *)ta_python_doc_invocation},
    {Py_tp_dealloc, ta_python_invocation_dealloc},
    {Py_tp_traverse, ta_python_invocation_traverse},
    {Py_tp_clear, ta_python_invocation_clear},
    {Py_tp_getset, ta_python_invocation_properties},
    {Py_tp_methods, ta_python_invocation_methods},
    {0, NULL}
};

static PyType_Spec ta_python_invocation_spec = {
    "tinyactors.Invocation", sizeof(ta_python_invocation), 0,
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC | Py_TPFLAGS_IMMUTABLETYPE | Py_TPFLAGS_DISALLOW_INSTANTIATION,
    ta_python_invocation_slots};

/* A text field of an effect as str, or None when empty; interned when it
 * repeats across invocations (the type, the src, the invokeid), so a
 * population of invocations shares one str of each. */
typedef struct ta_python_effect_text_of {
    uint32_t index, field;
    int intern;
} ta_python_effect_text_of;

static PyObject *ta_python_effect_str(ta_python_host *host, const ta_python_effect_text_of *of) {
    ta_string_view text = ta_host_effect_text(host->host, of->index, of->field);
    if (!text.length) return Py_NewRef(Py_None);
    PyObject *value = PyUnicode_DecodeUTF8(text.pointer, (Py_ssize_t)text.length, "strict");
    if (value && of->intern) PyUnicode_InternInPlace(&value);
    return value;
}

/* An UNINVOKE of the same (segment, token) later in the open batch: the
 * invoker cancelled it in the run that started it, so a failure to start
 * is nobody's news. */
static int ta_python_uninvoked_in_batch(ta_python_host *host, uint32_t index) {
    ta_host *core = host->host;
    const ta_host_effect *effects;
    uint32_t count = ta_host_effects(core, &effects);
    uint64_t segment = ta_host_effect_number(core, index, TA_EFFECT_FIELD_SEGMENT);
    uint64_t token = ta_host_effect_number(core, index, TA_EFFECT_FIELD_TOKEN);
    for (uint32_t i = index + 1; i < count; i++)
        if (ta_host_effect_number(core, i, TA_EFFECT_FIELD_KIND) == TA_EFFECT_UNINVOKE &&
            ta_host_effect_number(core, i, TA_EFFECT_FIELD_SEGMENT) == segment &&
            ta_host_effect_number(core, i, TA_EFFECT_FIELD_TOKEN) == token)
            return 1;
    return 0;
}

/* TA_EFFECT_INVOKE: the handler registered for the type starts the service;
 * what it returns is kept, keyed by (segment, token), until done or
 * UNINVOKE. No handler, or one that raises: error.communication for the
 * invoker (a service it cannot have, unless it cancelled it already), the
 * exception raised after the turn, and the entry kept as failed. */
static void ta_python_dispatch_invoke(ta_python_host *host, uint32_t index) {
    ta_host *core = host->host;
    ta_python_invocation *invocation = PyObject_GC_New(ta_python_invocation, (PyTypeObject *)host->state->invocation_type);
    if (!invocation) {
        ta_python_host_keep_error(host);
        return;
    }
    invocation->host = (ta_python_host *)Py_NewRef((PyObject *)host);
    invocation->session = ta_host_effect_number(core, index, TA_EFFECT_FIELD_SESSION);
    invocation->segment = (uint32_t)ta_host_effect_number(core, index, TA_EFFECT_FIELD_SEGMENT);
    invocation->token = (uint32_t)ta_host_effect_number(core, index, TA_EFFECT_FIELD_TOKEN);
    invocation->ended = 0;
    invocation->failed = 0;
    invocation->restored = (ta_host_effect_number(core, index, TA_EFFECT_FIELD_FLAGS) & TA_EFFECT_FLAG_RESTORED) != 0;
    invocation->pad = 0;
    invocation->invokeid = ta_python_effect_str(host, &(ta_python_effect_text_of){index, TA_EFFECT_FIELD_SENDID, 1});
    invocation->type = invocation->invokeid ? ta_python_effect_str(host, &(ta_python_effect_text_of){index, TA_EFFECT_FIELD_EVENT, 1}) : NULL;
    invocation->src = invocation->type ? ta_python_effect_str(host, &(ta_python_effect_text_of){index, TA_EFFECT_FIELD_TARGET, 1}) : NULL;
    invocation->traceparent =
        invocation->src ? ta_python_effect_str(host, &(ta_python_effect_text_of){index, TA_EFFECT_FIELD_TRACEPARENT, 0}) : NULL;
    ta_python_payload_of payload = {ta_host_effect_number(core, index, TA_EFFECT_FIELD_PAYLOAD),
                                    (uint32_t)ta_host_effect_number(core, index, TA_EFFECT_FIELD_DATAMODEL)};
    invocation->data = !invocation->traceparent ? NULL : payload.payload ? ta_python_host_payload_value(host, &payload) : Py_NewRef(Py_None);
    PyObject_GC_Track((PyObject *)invocation);
    PyObject *handler = NULL, *service = NULL, *key = NULL, *entry = NULL;
    int found = invocation->data ? PyDict_GetItemRef(host->services, invocation->type, &handler) : -1;
    /* none for a restored invocation: the application chose not to start it again, and the invoker hears so */
    if (found == 0 && !invocation->restored)
        PyErr_Format(host->state->error, "no service is registered for the invoke type %R", invocation->type);
    if (found > 0) service = PyObject_CallFunctionObjArgs(handler, (PyObject *)host, (PyObject *)invocation, NULL);
    if (service && invocation->ended) goto done; /* the handler called done(): nothing to keep */
    if (!service) { /* it does not run: kept as failed, so sends to it fail too, until the UNINVOKE */
        PyObject *raised = PyErr_GetRaisedException();
        invocation->ended = invocation->failed = 1;
        service = Py_NewRef(Py_None);
        if (invocation->data && !ta_python_uninvoked_in_batch(host, index)) {
            ta_python_service_event failed = {TA_PYTHON_INVOCATION_FAILED, NULL, Py_None, invocation->traceparent, {0, 0}};
            if (ta_python_invocation_post(invocation, &failed) < 0) PyErr_Clear();
        }
        PyErr_SetRaisedException(raised);
        if (raised) ta_python_host_keep_error(host);
    }
    key = ta_python_invocation_key(invocation->segment, invocation->token);
    entry = key ? PyTuple_Pack(2, (PyObject *)invocation, service) : NULL;
    if (!entry || PyDict_SetItem(host->invocations, key, entry) < 0) ta_python_host_keep_error(host);
done:
    Py_XDECREF(entry);
    Py_XDECREF(key);
    Py_XDECREF(service);
    Py_XDECREF(handler);
    Py_DECREF((PyObject *)invocation);
}

/* The (invocation, service) of an effect's (segment, token), or NULL. */
static PyObject *ta_python_invocation_entry(ta_python_host *host, uint32_t index) {
    PyObject *key = ta_python_invocation_key((uint32_t)ta_host_effect_number(host->host, index, TA_EFFECT_FIELD_SEGMENT),
                                             (uint32_t)ta_host_effect_number(host->host, index, TA_EFFECT_FIELD_TOKEN));
    PyObject *entry = NULL;
    if (key && PyDict_GetItemRef(host->invocations, key, &entry) < 0) PyErr_Clear();
    Py_XDECREF(key);
    return entry;
}

/* The service's receive raised: it crashed. Unless it ended meanwhile, the
 * invoker gets error.communication (with the send's sendid), and the entry
 * stays as failed without the service (dropped, not stopped), so later
 * sends fail too, until the UNINVOKE. The exception stays set. */
static void ta_python_invocation_crashed(ta_python_host *host, uint32_t index) {
    PyObject *raised = PyErr_GetRaisedException();
    PyObject *entry = ta_python_invocation_entry(host, index);
    ta_python_invocation *invocation = entry ? (ta_python_invocation *)PyTuple_GetItem(entry, 0) : NULL;
    if (invocation && !invocation->ended) {
        invocation->ended = invocation->failed = 1;
        PyObject *key = ta_python_invocation_key(invocation->segment, invocation->token);
        PyObject *failed_entry = PyTuple_Pack(2, (PyObject *)invocation, Py_None);
        if (!key || !failed_entry || PyDict_SetItem(host->invocations, key, failed_entry) < 0) PyErr_Clear();
        Py_XDECREF(failed_entry);
        Py_XDECREF(key);
        ta_python_service_event failed = {TA_PYTHON_INVOCATION_FAILED, NULL, Py_None, invocation->traceparent,
                                          ta_host_effect_text(host->host, index, TA_EFFECT_FIELD_SENDID)};
        if (ta_python_invocation_post(invocation, &failed) < 0) PyErr_Clear();
    }
    Py_XDECREF(entry);
    PyErr_SetRaisedException(raised);
}

/* A send to an invoked service (IO_PROCESSOR TA_NIL): its service's
 * receive(host, send), with the send as a thin view; a service without
 * receive, or none (done): dropped; one that failed to start or crashed:
 * the send fails with error.communication; a receive that raises: the
 * service crashed. */
static void ta_python_dispatch_service_send(ta_python_host *host, uint32_t index) {
    PyObject *entry = ta_python_invocation_entry(host, index);
    ta_python_invocation *invocation = entry ? (ta_python_invocation *)PyTuple_GetItem(entry, 0) : NULL;
    if (invocation && invocation->failed) { /* a service that never ran: the send fails, as core's do */
        ta_python_service_event failed = {TA_PYTHON_INVOCATION_FAILED, NULL, Py_None, invocation->traceparent,
                                          ta_host_effect_text(host->host, index, TA_EFFECT_FIELD_SENDID)};
        if (ta_python_invocation_post(invocation, &failed) < 0) ta_python_host_keep_error(host);
        Py_DECREF(entry);
        return;
    }
    PyObject *receive = entry && PyObject_HasAttrString(PyTuple_GetItem(entry, 1), "receive")
                            ? PyObject_GetAttrString(PyTuple_GetItem(entry, 1), "receive")
                            : NULL;
    Py_XDECREF(entry);
    if (!receive) return;
    ta_python_view *view = ta_python_view_new(host, host->state->send_type, index);
    PyObject *result = NULL;
    if (view) {
        view->processor = TA_NIL;
        result = PyObject_CallFunctionObjArgs(receive, (PyObject *)host, (PyObject *)view, NULL);
    }
    if (!result && view) ta_python_invocation_crashed(host, index);
    if (!result) ta_python_host_keep_error(host);
    Py_XDECREF(result);
    Py_XDECREF((PyObject *)view);
    Py_DECREF(receive);
}

/* TA_EFFECT_UNINVOKE: the invoker cancelled it; the service's stop(host),
 * then its entry goes. */
static void ta_python_dispatch_uninvoke(ta_python_host *host, uint32_t index) {
    PyObject *entry = ta_python_invocation_entry(host, index);
    if (!entry) return; /* done already, or never ran */
    ta_python_invocation *invocation = (ta_python_invocation *)PyTuple_GetItem(entry, 0);
    PyObject *service = PyTuple_GetItem(entry, 1);
    int failed = (int)invocation->failed;
    ta_python_invocation_end(invocation);
    PyObject *stop = !failed && PyObject_HasAttrString(service, "stop") ? PyObject_GetAttrString(service, "stop") : NULL;
    PyObject *result = stop ? PyObject_CallFunctionObjArgs(stop, (PyObject *)host, NULL) : Py_NewRef(Py_None);
    if (!result) ta_python_host_keep_error(host);
    Py_XDECREF(result);
    Py_XDECREF(stop);
    Py_DECREF(entry);
}

/* A migration ended: the host's migration_done handler gets {"migration",
 * "migrated", "failed", "pending"}, the sessions it left behind read now (ta_host_migration_pending:
 * valid until the next run) into a bytes the list is made from, then
 * dropped; no handler, nothing read. */
static void ta_python_dispatch_migration(ta_python_host *host, uint32_t index) {
    ta_host *core = host->host;
    uint32_t id = (uint32_t)ta_host_effect_number(core, index, TA_EFFECT_FIELD_MIGRATION);
    PyObject *handler = host->migration_done, *migration = NULL, *pending = NULL, *result = NULL, *called = NULL;
    int found = handler == Py_None ? 0 : (migration = PyLong_FromUnsignedLong(id)) ? 1 : -1;
    if (found > 0) {
        uint32_t count = (uint32_t)ta_host_effect_number(core, index, TA_EFFECT_FIELD_PENDING);
        PyObject *room = PyBytes_FromStringAndSize(NULL, (Py_ssize_t)count * 8);
        uint64_t *left = room ? (uint64_t *)PyBytes_AsString(room) : NULL;
        uint32_t got = left ? ta_host_migration_pending(core, id, (ta_uint64_array){left, count}) : 0;
        pending = left ? PyList_New(got < count ? got : count) : NULL;
        for (uint32_t i = 0; pending && i < got && i < count; i++) {
            PyObject *session = PyLong_FromUnsignedLongLong(left[i]);
            if (!session) Py_CLEAR(pending);
            else PyList_SetItem(pending, i, session);
        }
        Py_XDECREF(room);
        result = pending ? Py_BuildValue("{s:O,s:k,s:k,s:O}", "migration", migration, "migrated",
                                         (unsigned long)ta_host_effect_number(core, index, TA_EFFECT_FIELD_MIGRATED), "failed",
                                         (unsigned long)ta_host_effect_number(core, index, TA_EFFECT_FIELD_FAILED), "pending", pending)
                         : NULL;
        called = result ? PyObject_CallFunctionObjArgs(handler, (PyObject *)host, result, NULL) : NULL;
    }
    if (found < 0 || (found > 0 && !called)) ta_python_host_keep_error(host);
    Py_XDECREF(called);
    Py_XDECREF(result);
    Py_XDECREF(pending);
    Py_XDECREF(migration);
}

void ta_python_dispatch(ta_python_host *host, uint32_t index) {
    ta_host *core = host->host;
    switch ((uint32_t)ta_host_effect_number(core, index, TA_EFFECT_FIELD_KIND)) {
    case TA_EFFECT_SEND: ta_python_dispatch_send(host, index); break;
    case TA_EFFECT_LOG: ta_python_dispatch_view(host, (ta_python_handler){host->log, host->state->entry_type}, index); break;
    case TA_EFFECT_REQUEST: { /* a fetch at run time: refused in C until it is built (AGENTS.md) */
        ta_resume resume = {(uint32_t)ta_host_effect_number(core, index, TA_EFFECT_FIELD_SEGMENT),
                            (uint32_t)ta_host_effect_number(core, index, TA_EFFECT_FIELD_TOKEN), TA_ERROR_INVALID, 0, 0};
        ta_host_resume(core, &resume);
        ta_python_loop_schedule(host); /* an input: the turn runs it */
        break;
    }
    case TA_HOST_EFFECT_REPLY: ta_python_reply(host, index); break;
    case TA_EFFECT_WAKE: ta_python_dispatch_wake(host, index); break;
    case TA_HOST_EFFECT_MIGRATED:
        ta_python_dispatch_view(host, (ta_python_handler){host->migrated, host->state->migrated_type}, index);
        break;
    case TA_HOST_EFFECT_MIGRATION_DONE: ta_python_dispatch_migration(host, index); break;
    case TA_EFFECT_INVOKE: ta_python_dispatch_invoke(host, index); break;
    case TA_EFFECT_UNINVOKE: ta_python_dispatch_uninvoke(host, index); break;
    case TA_HOST_EFFECT_TRACES: /* a request is ready: taken (the next comes only after a take) */
    case TA_HOST_EFFECT_LOGS: {
        ta_python_take(host, ta_host_effect_number(core, index, TA_EFFECT_FIELD_KIND) == TA_HOST_EFFECT_TRACES);
        break;
    }
    default: /* DONE, ERROR: the datamodel's rows went with destroy; SEGMENT_FREED: the host keeps the
                images, we keep none; a NOTIFICATION we do not know; the rest has no API yet */
        break;
    }
}

int ta_python_effects_init_types(PyObject *module, ta_python_module_state *state) {
    state->send_type = PyType_FromModuleAndSpec(module, &ta_python_send_spec, NULL);
    state->entry_type = state->send_type ? PyType_FromModuleAndSpec(module, &ta_python_entry_spec, NULL) : NULL;
    state->migrated_type = state->entry_type ? PyType_FromModuleAndSpec(module, &ta_python_migrated_spec, NULL) : NULL;
    state->invocation_type = state->migrated_type ? PyType_FromModuleAndSpec(module, &ta_python_invocation_spec, NULL) : NULL;
    if (!state->invocation_type) return -1;
    if (PyModule_AddObjectRef(module, "Invocation", state->invocation_type) < 0) return -1;
    if (PyModule_AddObjectRef(module, "Migrated", state->migrated_type) < 0) return -1;
    if (PyModule_AddObjectRef(module, "Send", state->send_type) < 0) return -1;
    return PyModule_AddObjectRef(module, "LogEntry", state->entry_type);
}
