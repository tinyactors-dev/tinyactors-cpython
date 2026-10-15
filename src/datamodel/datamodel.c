/* The "python" datamodel's vtable (tinyactors/datamodel.h; AGENTS.md,
 * "The "python" datamodel"): a class per chart at chart_attach, an
 * instance per session, `_event` an immutable Event kept from bind_event
 * to the next one or destroy, payloads as rows of Python values, and
 * hibernation by the session row's handle. An expression that raises is
 * TA_ERROR_EXECUTION; a BaseException that is not an Exception is kept
 * and raised again after the run. */
#include "datamodel.h"
#include "../docs.h"
#include <string.h>

/* ---- the Event of _event ------------------------------------------------------------ */

typedef struct ta_python_event {
    PyObject_HEAD
    PyObject *name, *type, *sendid, *origin, *origintype, *invokeid, *data, *raw;
    /* core's ids of its strings (ta_event_view): written into a blob with them, so a woken or
       restored session's Event shares the chart's strings again */
    uint32_t name_id, invoke_slot, origintype_id, pad;
} ta_python_event;

static PyMemberDef ta_python_event_members[] = {
    {"name", Py_T_OBJECT_EX, offsetof(ta_python_event, name), Py_READONLY, ta_python_doc_event_name},
    {"type", Py_T_OBJECT_EX, offsetof(ta_python_event, type), Py_READONLY, ta_python_doc_event_type},
    {"sendid", Py_T_OBJECT_EX, offsetof(ta_python_event, sendid), Py_READONLY, ta_python_doc_event_sendid},
    {"origin", Py_T_OBJECT_EX, offsetof(ta_python_event, origin), Py_READONLY, ta_python_doc_event_origin},
    {"origintype", Py_T_OBJECT_EX, offsetof(ta_python_event, origintype), Py_READONLY, ta_python_doc_event_origintype},
    {"invokeid", Py_T_OBJECT_EX, offsetof(ta_python_event, invokeid), Py_READONLY, ta_python_doc_event_invokeid},
    {"data", Py_T_OBJECT_EX, offsetof(ta_python_event, data), Py_READONLY, ta_python_doc_event_data},
    {"raw", Py_T_OBJECT_EX, offsetof(ta_python_event, raw), Py_READONLY, ta_python_doc_event_raw},
    {NULL, 0, 0, 0, NULL}
};

static int ta_python_event_traverse(PyObject *self, visitproc visit, void *arg) {
    ta_python_event *event = (ta_python_event *)self;
    Py_VISIT(Py_TYPE(self));
    Py_VISIT(event->data);
    return 0;
}

static int ta_python_event_clear(PyObject *self) {
    ta_python_event *event = (ta_python_event *)self;
    Py_CLEAR(event->name);
    Py_CLEAR(event->type);
    Py_CLEAR(event->sendid);
    Py_CLEAR(event->origin);
    Py_CLEAR(event->origintype);
    Py_CLEAR(event->invokeid);
    Py_CLEAR(event->data);
    Py_CLEAR(event->raw);
    return 0;
}

static void ta_python_event_dealloc(PyObject *self) {
    PyTypeObject *type = Py_TYPE(self);
    PyObject_GC_UnTrack(self);
    ta_python_event_clear(self);
    freefunc ta_python_release = (freefunc)PyType_GetSlot(type, Py_tp_free);
    ta_python_release(self);
    Py_DECREF(type);
}

static PyType_Slot ta_python_event_slots[] = {
    {Py_tp_doc, (void *)ta_python_doc_event},
    {Py_tp_members, ta_python_event_members},
    {Py_tp_dealloc, ta_python_event_dealloc},
    {Py_tp_traverse, ta_python_event_traverse},
    {Py_tp_clear, ta_python_event_clear},
    {0, NULL}
};

static PyType_Spec ta_python_event_spec = {
    "tinyactors.Event", sizeof(ta_python_event), 0,
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC | Py_TPFLAGS_IMMUTABLETYPE | Py_TPFLAGS_DISALLOW_INSTANTIATION,
    ta_python_event_slots
};

int ta_python_event_init_type(PyObject *module, ta_python_module_state *state) {
    state->event_type = PyType_FromModuleAndSpec(module, &ta_python_event_spec, NULL);
    return state->event_type ? PyModule_AddObjectRef(module, "Event", state->event_type) : -1;
}

/* ---- the datamodel of a host ------------------------------------------------------------ */

int ta_python_datamodel_init(ta_python_datamodel *datamodel, ta_python_host *host) {
    datamodel->host = host;
    datamodel->state = host->state;
    if (ta_python_rows_init(datamodel) < 0) return -1;
    return ta_python_namespace_init(datamodel);
}

void ta_python_datamodel_free(ta_python_datamodel *datamodel) {
    if (!datamodel->state) return;
    ta_python_rows_free(datamodel);
    if (datamodel->capsule) PyCapsule_SetContext(datamodel->capsule, NULL); /* In() of a freed host refuses */
    Py_CLEAR(datamodel->capsule);
    Py_CLEAR(datamodel->namespace);
    Py_CLEAR(datamodel->raised);
    Py_CLEAR(datamodel->output);
    Py_CLEAR(datamodel->origintypes);
    if (datamodel->wire.pointer) {
        const ta_allocator *allocator = &datamodel->host->allocator;
        allocator->free(allocator->context, datamodel->wire.pointer, datamodel->wire.capacity);
    }
    datamodel->wire = (ta_buffer){NULL, 0, 0};
    datamodel->state = NULL;
}

int ta_python_datamodel_traverse(ta_python_datamodel *datamodel, visitproc visit, void *arg) {
    Py_VISIT(datamodel->namespace);
    Py_VISIT(datamodel->raised);
    Py_VISIT(datamodel->output);
    Py_VISIT(datamodel->origintypes);
    return datamodel->state ? ta_python_rows_traverse(datamodel, visit, arg) : 0;
}

/* ---- an evaluation ------------------------------------------------------------------------ */

/* What an entry evaluates with: the session's instance and its chart's
 * functions, held (a row pointer would not survive an add that chart
 * code causes), and the environment In() reads. */
typedef struct ta_python_scope {
    ta_python_datamodel *datamodel;
    PyObject *instance;
    PyObject *functions;
    ta_python_session_row row; /* a copy, for its chart and flags */
    uint64_t handle;
    ta_datamodel_environment *saved;
} ta_python_scope;

static int ta_python_scope_enter(const ta_datamodel_call *call, ta_python_scope *scope) {
    ta_python_datamodel *datamodel = call->datamodel;
    ta_python_session_row *row = ta_table_get(&datamodel->sessions, call->instance);
    ta_python_chart_row *chart = row && row->instance ? ta_table_get(&datamodel->charts, row->chart) : NULL;
    if (!chart) return -1;
    scope->datamodel = datamodel;
    scope->row = *row;
    scope->handle = call->instance;
    scope->instance = Py_NewRef(row->instance);
    scope->functions = Py_NewRef(chart->functions);
    scope->saved = datamodel->environment;
    datamodel->environment = call->environment;
    if (call->environment) ta_datamodel_environment_charge(call->environment, 1); /* fuel once per evaluation */
    return 0;
}

static void ta_python_scope_leave(ta_python_scope *scope) {
    scope->datamodel->environment = scope->saved;
    Py_DECREF(scope->instance);
    Py_DECREF(scope->functions);
}

/* The exception set now as the entry's failure: error.execution for the
 * chart; a BaseException that is not an Exception (KeyboardInterrupt,
 * SystemExit) is kept for the driver, which raises the same object. */
static ta_status ta_python_failed(ta_python_datamodel *datamodel) {
    PyObject *raised = PyErr_GetRaisedException();
    if (raised && !PyErr_GivenExceptionMatches(raised, PyExc_Exception) && !datamodel->raised)
        datamodel->raised = Py_NewRef(raised);
    Py_XDECREF(raised);
    return TA_ERROR_EXECUTION;
}

/* The compiled form of expression `e` (borrowed), or NULL with the
 * exception that refused it set. */
static PyObject *ta_python_compiled(ta_python_scope *scope, ta_expression e) {
    if (e >= (ta_expression)PyTuple_Size(scope->functions)) {
        PyErr_SetString(PyExc_IndexError, "no such expression");
        return NULL;
    }
    PyObject *compiled = PyTuple_GetItem(scope->functions, e);
    if (PyExceptionInstance_Check(compiled)) { /* refused at chart_attach: error.execution now (W3C 312) */
        PyErr_SetObject((PyObject *)Py_TYPE(compiled), compiled);
        return NULL;
    }
    return compiled;
}

/* The value of expression `e`: a new reference, or NULL. */
static PyObject *ta_python_value(ta_python_scope *scope, ta_expression e) {
    PyObject *compiled = ta_python_compiled(scope, e);
    if (!compiled) return NULL;
    if (PyTuple_Check(compiled)) compiled = PyTuple_GetItem(compiled, 0); /* a location's getter */
    return PyObject_CallFunctionObjArgs(compiled, scope->instance, NULL);
}

/* Assign `value` (borrowed) to location `e`. */
static int ta_python_assign_value(ta_python_scope *scope, ta_expression e, PyObject *value) {
    PyObject *compiled = ta_python_compiled(scope, e);
    if (!compiled) return -1;
    if (!PyTuple_Check(compiled)) {
        PyErr_SetString(PyExc_TypeError, "not a location");
        return -1;
    }
    PyObject *done = PyObject_CallFunctionObjArgs(PyTuple_GetItem(compiled, 1), scope->instance, value, NULL);
    if (!done) return -1;
    Py_DECREF(done);
    return 0;
}

/* A string the datamodel answers (eval_string, log): the str's UTF-8,
 * held until the datamodel's next call. */
static ta_status ta_python_string_out(ta_python_datamodel *datamodel, PyObject *value, ta_string_view *out) {
    PyObject *text = value && PyUnicode_Check(value) ? Py_NewRef(value) : value ? PyObject_Str(value) : NULL;
    if (!text || ta_python_text(text, out) < 0) {
        Py_XDECREF(text);
        return ta_python_failed(datamodel);
    }
    ta_python_replace(&datamodel->output, text);
    return TA_OK;
}

/* ---- the vtable's entries ---------------------------------------------------------------- */

static ta_status ta_python_chart_attach(void *self, const ta_datamodel_chart *chart, void **context) {
    ta_python_datamodel *datamodel = self;
    ta_python_chart_row made = {NULL, NULL, NULL, NULL, 0, 0, 0, 0};
    if (ta_python_chart_compile(datamodel, &(ta_python_attach){chart->image, chart->chart}, &made) < 0 || !made.names) {
        ta_python_drop(datamodel, made.chart);
        ta_python_drop(datamodel, made.functions);
        ta_python_drop(datamodel, made.data);
        PyErr_Clear(); /* a chart this datamodel cannot run: the image is refused */
        return TA_ERROR_INVALID;
    }
    uint64_t handle = 0;
    ta_python_chart_row *row = ta_table_add(&datamodel->charts, &handle);
    if (!row) {
        ta_python_drop(datamodel, made.chart);
        ta_python_drop(datamodel, made.functions);
        ta_python_drop(datamodel, made.data);
        ta_python_drop(datamodel, made.names);
        return TA_ERROR_NO_MEMORY;
    }
    *row = made;
    row->segment = chart->segment;
    row->index = chart->chart;
    *context = (void *)(uintptr_t)handle; /* the row's handle, never a pointer */
    return TA_OK;
}

static void ta_python_chart_detach(void *self, void *context) {
    ta_python_datamodel *datamodel = self;
    uint64_t handle = (uint64_t)(uintptr_t)context;
    ta_python_chart_row *row = ta_table_get(&datamodel->charts, handle);
    if (!row) return;
    ta_python_chart_row kept = *row;
    ta_table_remove(&datamodel->charts, handle);
    ta_python_drop(datamodel, kept.chart);
    ta_python_drop(datamodel, kept.functions);
    ta_python_drop(datamodel, kept.data);
    ta_python_drop(datamodel, kept.names);
}

static PyObject *ta_python_view_str(ta_string_view text) {
    return PyUnicode_DecodeUTF8(text.pointer ? text.pointer : "", text.length, "strict");
}

static int ta_python_ends_with(ta_string_view text, const char *suffix) {
    size_t length = strlen(suffix);
    return text.length >= length && !memcmp(text.pointer + text.length - length, suffix, length);
}

/* _ioprocessors: {type: {"location": ...}}, with the short names the W3C
 * tests use, as the ECMAScript datamodel has them. */
static PyObject *ta_python_io_processors(const ta_datamodel_system_variables *variables) {
    PyObject *processors = PyDict_New();
    for (uint32_t i = 0; processors && i < variables->io_processor_count; i++) {
        const ta_io_processor_entry *entry = &variables->io_processors[i];
        PyObject *location = ta_python_view_str(entry->location), *type = ta_python_view_str(entry->type);
        PyObject *value = location ? Py_BuildValue("{s:O}", "location", location) : NULL;
        int status = value && type && PyDict_SetItem(processors, type, value) == 0 ? 0 : -1;
        if (status == 0 && ta_python_ends_with(entry->type, "#SCXMLEventProcessor"))
            status = PyDict_SetItemString(processors, "scxml", value);
        if (status == 0 && ta_python_ends_with(entry->type, "#BasicHTTPEventProcessor"))
            status = PyDict_SetItemString(processors, "basichttp", value);
        Py_XDECREF(location);
        Py_XDECREF(type);
        Py_XDECREF(value);
        if (status < 0) Py_CLEAR(processors);
    }
    return processors;
}

/* The system variables a chart reads, made once at create (core's views
 * end with the call): (_sessionid, _name, _ioprocessors), None for the
 * ones it does not read; None when it reads none. */
static PyObject *ta_python_system(const ta_python_chart_row *chart, const ta_datamodel_system_variables *variables) {
    if (!chart->reads) return Py_NewRef(Py_None);
    PyObject *sessionid = chart->reads & TA_PYTHON_READS_SESSIONID ? ta_python_view_str(variables->sessionid) : Py_NewRef(Py_None);
    PyObject *name = chart->reads & TA_PYTHON_READS_NAME ? ta_python_view_str(variables->name) : Py_NewRef(Py_None);
    PyObject *processors = chart->reads & TA_PYTHON_READS_IOPROCESSORS ? ta_python_io_processors(variables) : Py_NewRef(Py_None);
    PyObject *system = sessionid && name && processors ? PyTuple_Pack(3, sessionid, name, processors) : NULL;
    Py_XDECREF(sessionid);
    Py_XDECREF(name);
    Py_XDECREF(processors);
    return system;
}

/* An instance of the chart's class: every <data> id None (unbound until
 * init_data), no _event yet, and the system variables. */
static PyObject *ta_python_instance(ta_python_datamodel *datamodel, const ta_python_chart_row *chart,
                                    const ta_datamodel_system_variables *variables) {
    (void)datamodel;
    PyObject *instance = PyObject_CallNoArgs(chart->chart);
    PyObject *system = instance ? ta_python_system(chart, variables) : NULL;
    int status = system && PyObject_SetAttrString(instance, "_ta_sys", system) == 0 &&
                         PyObject_SetAttrString(instance, "_event", Py_None) == 0
                     ? 0
                     : -1;
    for (Py_ssize_t i = 0; status == 0 && i < PyTuple_Size(chart->data); i++)
        status = PyObject_SetAttr(instance, PyTuple_GetItem(chart->data, i), Py_None);
    Py_XDECREF(system);
    if (status < 0) Py_CLEAR(instance);
    return instance;
}

/* The session's I/O processors' types, once per host (every session has the same ones): the
 * origintypes an event names by index. */
static void ta_python_keep_origintypes(ta_python_datamodel *datamodel, const ta_datamodel_system_variables *variables) {
    if (datamodel->origintypes || !variables) return;
    PyObject *types = PyTuple_New(variables->io_processor_count);
    for (uint32_t i = 0; types && i < variables->io_processor_count; i++) {
        PyObject *type = ta_python_view_str(variables->io_processors[i].type);
        if (!type) Py_CLEAR(types);
        else PyTuple_SetItem(types, i, type);
    }
    if (!types) PyErr_Clear(); /* the events make their own strs */
    datamodel->origintypes = types;
}

static ta_status ta_python_create(void *self, const ta_datamodel_start *start, uint64_t *out) {
    ta_python_datamodel *datamodel = self;
    ta_python_keep_origintypes(datamodel, start->system_variables);
    PyObject *init = start->init ? ta_python_payload_value(datamodel, start->init) : NULL;
    Py_XINCREF(init);
    ta_python_payload_release(datamodel, start->init); /* the init payload's reference is ours */
    ta_python_chart_row *chart = ta_table_get(&datamodel->charts, (uint64_t)(uintptr_t)start->chart_context);
    PyObject *instance = chart ? ta_python_instance(datamodel, chart, start->system_variables) : NULL;
    uint64_t chart_handle = (uint64_t)(uintptr_t)start->chart_context;
    if (!instance) {
        ta_python_drop(datamodel, init);
        return ta_python_failed(datamodel);
    }
    uint64_t handle = 0;
    ta_python_session_row *row = ta_table_add(&datamodel->sessions, &handle);
    if (!row) {
        ta_python_drop(datamodel, instance);
        ta_python_drop(datamodel, init);
        return TA_ERROR_NO_MEMORY;
    }
    row->instance = instance;
    row->chart = chart_handle;
    row->extra = init;
    *out = handle;
    return TA_OK;
}

static void ta_python_destroy(const ta_datamodel_call *call) {
    ta_python_datamodel *datamodel = call->datamodel;
    ta_python_session_row *row = ta_table_get(&datamodel->sessions, call->instance);
    if (!row) return;
    ta_python_session_row kept = *row;
    ta_table_remove(&datamodel->sessions, call->instance);
    ta_python_drop(datamodel, kept.instance);
    ta_python_drop(datamodel, kept.extra);
}

static ta_status ta_python_init_data(const ta_datamodel_call *call, uint32_t declaration) {
    ta_python_scope scope;
    if (ta_python_scope_enter(call, &scope) < 0) return TA_ERROR_EXECUTION;
    ta_python_datamodel *datamodel = scope.datamodel;
    ta_python_chart_row *chart = ta_table_get(&datamodel->charts, scope.row.chart);
    PyObject *id = chart && declaration < (uint32_t)PyTuple_Size(chart->data) ? Py_NewRef(PyTuple_GetItem(chart->data, declaration)) : NULL;
    ta_data_info info;
    ta_status status = TA_OK;
    PyObject *value = NULL;
    if (!id || !call->environment ||
        ta_image_data(ta_datamodel_environment_image(call->environment),
                      (ta_chart_item){ta_datamodel_environment_chart(call->environment), declaration}, &info) != TA_OK) {
        status = TA_ERROR_EXECUTION;
    } else {
        PyObject *given = NULL; /* the spawn's or invoke's params override <data> of their names */
        if (scope.row.extra && PyDict_Check(scope.row.extra) && PyDict_GetItemRef(scope.row.extra, id, &given) < 0) PyErr_Clear();
        if (given) value = given;
        else if (info.expression != TA_NIL) value = ta_python_value(&scope, info.expression);
        else value = Py_NewRef(Py_None);
        if (!value) status = ta_python_failed(datamodel);
    }
    if (id && PyObject_SetAttr(scope.instance, id, value ? value : Py_None) < 0 && status == TA_OK)
        status = ta_python_failed(datamodel);
    Py_XDECREF(value);
    Py_XDECREF(id);
    ta_python_scope_leave(&scope);
    return status;
}

/* An event's name: the chart's shared str for a name its image has (one
 * per name, not one per event and session), else a new one. */
static PyObject *ta_python_event_name(ta_python_datamodel *datamodel, uint64_t chart, const ta_event_view *view) {
    ta_python_chart_row *row = ta_table_get(&datamodel->charts, chart);
    PyObject *names = row ? row->names : NULL;
    if (!names || view->name_id >= (uint32_t)PyList_Size(names)) return ta_python_view_str(view->name);
    PyObject *name = PyList_GetItem(names, view->name_id);
    Py_ssize_t length = 0;
    const char *text = name != Py_None ? PyUnicode_AsUTF8AndSize(name, &length) : NULL;
    if (text && (size_t)length == view->name.length && !memcmp(text, view->name.pointer, view->name.length))
        return Py_NewRef(name);
    PyErr_Clear();
    PyObject *made = ta_python_view_str(view->name);
    if (made && name == Py_None) PyList_SetItem(names, view->name_id, Py_NewRef(made)); /* the first: kept */
    return made;
}

static PyObject *ta_python_optional_str(ta_string_view text) {
    return text.length ? ta_python_view_str(text) : Py_NewRef(Py_None);
}

/* The str of `text` kept at `index` of a chart's list (its names, invoke ids
 * or origins): the one there when its text is the same, else `made` (or a
 * str of the text), kept there when the place was empty. The list belongs
 * to the chart's row, so what it keeps lives as long as the chart. A text
 * that differs (an id generated per execution) gets a str of its own. */
typedef struct ta_python_kept_at {
    PyObject *list;
    uint32_t index;
    int grow; /* the list grows to the index (invoke slots: bounded by the chart's invokes) */
} ta_python_kept_at;

static PyObject *ta_python_kept(const ta_python_kept_at *at, ta_string_view text, PyObject *made) {
    PyObject *list = at->list;
    if (list && at->index != TA_NIL && at->grow)
        while (PyList_Size(list) <= (Py_ssize_t)at->index && PyList_Append(list, Py_None) == 0) {}
    PyErr_Clear();
    PyObject *kept = list && at->index != TA_NIL && (Py_ssize_t)at->index < PyList_Size(list)
                         ? PyList_GetItem(list, at->index)
                         : NULL;
    if (kept && kept != Py_None) {
        Py_ssize_t length = 0;
        const char *chars = PyUnicode_AsUTF8AndSize(kept, &length);
        if (chars && (size_t)length == text.length && !memcmp(chars, text.pointer, text.length)) {
            Py_XDECREF(made);
            return Py_NewRef(kept);
        }
        PyErr_Clear();
    }
    if (!made) made = ta_python_view_str(text);
    if (made && kept == Py_None) PyList_SetItem(list, at->index, Py_NewRef(made)); /* the first: kept */
    return made;
}

/* The str of an origintype: the session's I/O processor's type the id names,
 * kept by the datamodel; else `made` (or a str of the text). */
static PyObject *ta_python_origintype(ta_python_datamodel *datamodel, uint32_t id, ta_string_view text) {
    PyObject *types = datamodel->origintypes;
    if (!text.length) return Py_NewRef(Py_None);
    if (types && id != TA_NIL && (Py_ssize_t)id < PyTuple_Size(types)) {
        PyObject *type = PyTuple_GetItem(types, id);
        Py_ssize_t length = 0;
        const char *chars = PyUnicode_AsUTF8AndSize(type, &length);
        if (chars && (size_t)length == text.length && !memcmp(chars, text.pointer, text.length)) return Py_NewRef(type);
        PyErr_Clear();
    }
    return ta_python_view_str(text);
}

/* A str's own UTF-8, as a view for the call. */
static ta_string_view ta_python_text_of(PyObject *text) {
    Py_ssize_t length = 0;
    const char *chars = text && PyUnicode_Check(text) ? PyUnicode_AsUTF8AndSize(text, &length) : NULL;
    if (!chars) PyErr_Clear();
    return (ta_string_view){chars, chars ? (uint32_t)length : 0};
}

static ta_status ta_python_bind_event(const ta_datamodel_call *call, const ta_event_view *view) {
    ta_python_datamodel *datamodel = call->datamodel;
    ta_python_session_row *row = ta_table_get(&datamodel->sessions, call->instance);
    if (!row || !row->instance) return TA_ERROR_EXECUTION;
    PyObject *instance = Py_NewRef(row->instance);
    uint64_t chart = row->chart;
    if (row->extra) { /* the params were read by init_data: an event has come */
        ta_python_drop(datamodel, row->extra);
        row->extra = NULL;
    }
    ta_python_event *event = (ta_python_event *)PyType_GenericAlloc((PyTypeObject *)datamodel->state->event_type, 0);
    ta_status status = TA_OK;
    if (event) {
        PyObject *data = view->data ? ta_python_payload_value(datamodel, view->data) : NULL;
        event->name = ta_python_event_name(datamodel, chart, view);
        event->type = Py_NewRef(PyTuple_GetItem(datamodel->state->event_kinds, view->type % 3));
        event->sendid = ta_python_optional_str(view->sendid);
        ta_python_chart_row *row_of_chart = ta_table_get(&datamodel->charts, chart);
        ta_python_kept_at invokeid_at = {row_of_chart ? row_of_chart->invokeids : NULL, view->invoke_slot, 1};
        ta_python_kept_at origin_at = {row_of_chart ? row_of_chart->origins : NULL, view->invoke_slot, 1};
        event->origin = view->origin.length ? ta_python_kept(&origin_at, view->origin, NULL) : Py_NewRef(Py_None);
        event->origintype = ta_python_origintype(datamodel, view->origintype_id, view->origintype);
        event->invokeid = view->invokeid.length ? ta_python_kept(&invokeid_at, view->invokeid, NULL) : Py_NewRef(Py_None);
        event->name_id = view->name_id;
        event->invoke_slot = view->invoke_slot;
        event->origintype_id = view->origintype_id;
        event->data = Py_NewRef(data ? data : Py_None); /* the payload's value itself (held by the Event) */
        event->raw = view->size >= sizeof *view ? ta_python_optional_str(view->raw) : Py_NewRef(Py_None);
        if (!event->name || !event->sendid || !event->origin || !event->origintype || !event->invokeid || !event->raw ||
            PyObject_SetAttr(instance, datamodel->state->event_key, (PyObject *)event) < 0)
            status = ta_python_failed(datamodel);
        Py_DECREF(event); /* the instance holds it now */
    } else status = ta_python_failed(datamodel);
    Py_DECREF(instance);
    return status;
}

/* SCXML keeps _event bound to the last event (5.10): nothing to drop. */
static void ta_python_detach_event(const ta_datamodel_call *call) {
    (void)call;
}

static ta_status ta_python_eval_condition(const ta_datamodel_call *call, ta_expression e, int *out) {
    ta_python_scope scope;
    if (ta_python_scope_enter(call, &scope) < 0) return TA_ERROR_EXECUTION;
    PyObject *value = ta_python_value(&scope, e);
    int truth = value ? PyObject_IsTrue(value) : -1;
    Py_XDECREF(value);
    ta_status status = truth < 0 ? ta_python_failed(scope.datamodel) : TA_OK;
    if (truth >= 0) *out = truth;
    ta_python_scope_leave(&scope);
    return status;
}

static ta_status ta_python_eval_string(const ta_datamodel_call *call, ta_expression e, ta_string_view *out) {
    ta_python_scope scope;
    if (ta_python_scope_enter(call, &scope) < 0) return TA_ERROR_EXECUTION;
    PyObject *value = ta_python_value(&scope, e);
    ta_status status = value ? ta_python_string_out(scope.datamodel, value, out) : ta_python_failed(scope.datamodel);
    Py_XDECREF(value);
    ta_python_scope_leave(&scope);
    return status;
}

static ta_status ta_python_assign(const ta_datamodel_call *call, ta_expression location, ta_expression e) {
    ta_python_scope scope;
    if (ta_python_scope_enter(call, &scope) < 0) return TA_ERROR_EXECUTION;
    PyObject *value = ta_python_value(&scope, e);
    ta_status status = value && ta_python_assign_value(&scope, location, value) == 0 ? TA_OK : ta_python_failed(scope.datamodel);
    Py_XDECREF(value);
    ta_python_scope_leave(&scope);
    return status;
}

static ta_status ta_python_assign_string(const ta_datamodel_call *call, ta_expression location, ta_string_view text) {
    ta_python_scope scope;
    if (ta_python_scope_enter(call, &scope) < 0) return TA_ERROR_EXECUTION;
    PyObject *value = ta_python_view_str(text);
    ta_status status = value && ta_python_assign_value(&scope, location, value) == 0 ? TA_OK : ta_python_failed(scope.datamodel);
    Py_XDECREF(value);
    ta_python_scope_leave(&scope);
    return status;
}

static ta_status ta_python_script(const ta_datamodel_call *call, ta_expression e) {
    ta_python_scope scope;
    if (ta_python_scope_enter(call, &scope) < 0) return TA_ERROR_EXECUTION;
    PyObject *value = ta_python_value(&scope, e);
    ta_status status = value ? TA_OK : ta_python_failed(scope.datamodel);
    Py_XDECREF(value);
    ta_python_scope_leave(&scope);
    return status;
}

static ta_status ta_python_log(const ta_datamodel_call *call, ta_expression e, ta_string_view *out) {
    return ta_python_eval_string(call, e, out);
}

static ta_status ta_python_foreach_begin(const ta_datamodel_call *call, ta_expression array, ta_datamodel_loop *loop) {
    ta_python_scope scope;
    if (ta_python_scope_enter(call, &scope) < 0) return TA_ERROR_EXECUTION;
    ta_python_datamodel *datamodel = scope.datamodel;
    PyObject *value = ta_python_value(&scope, array);
    PyObject *items = value && (PyList_Check(value) || PyTuple_Check(value)) ? PySequence_Tuple(value) : NULL;
    if (value && !items && !PyErr_Occurred()) PyErr_SetString(PyExc_TypeError, "<foreach> takes a list or a tuple");
    Py_XDECREF(value);
    ta_status status = TA_OK;
    if (!items || PyTuple_Size(items) > (Py_ssize_t)INT32_MAX) {
        Py_XDECREF(items);
        status = ta_python_failed(datamodel);
    } else {
        uint64_t handle = 0;
        ta_python_loop_row *row = ta_table_add(&datamodel->loops, &handle);
        if (!row) {
            ta_python_drop(datamodel, items);
            status = TA_ERROR_NO_MEMORY;
        } else {
            row->items = items; /* a shallow copy, as the ECMAScript datamodel takes */
            loop->iter = handle;
            loop->n = (uint32_t)PyTuple_Size(items);
        }
    }
    ta_python_scope_leave(&scope);
    return status;
}

static ta_status ta_python_foreach_item(const ta_datamodel_call *call, const ta_datamodel_loop *loop) {
    ta_python_scope scope;
    if (ta_python_scope_enter(call, &scope) < 0) return TA_ERROR_EXECUTION;
    ta_python_loop_row *row = ta_table_get(&scope.datamodel->loops, loop->iter);
    PyObject *items = row ? Py_NewRef(row->items) : NULL;
    int status = items && loop->i < (uint32_t)PyTuple_Size(items) ? 0 : -1;
    if (status == 0) status = ta_python_assign_value(&scope, loop->item, PyTuple_GetItem(items, loop->i));
    if (status == 0 && loop->index != TA_NIL) {
        PyObject *index = PyLong_FromUnsignedLong(loop->i);
        status = index ? ta_python_assign_value(&scope, loop->index, index) : -1;
        Py_XDECREF(index);
    }
    Py_XDECREF(items);
    ta_status result = status == 0 ? TA_OK : ta_python_failed(scope.datamodel);
    ta_python_scope_leave(&scope);
    return result;
}

static void ta_python_foreach_end(const ta_datamodel_call *call, const ta_datamodel_loop *loop) {
    ta_python_datamodel *datamodel = call->datamodel;
    ta_python_loop_row *row = ta_table_get(&datamodel->loops, loop->iter);
    if (!row) return;
    PyObject *items = row->items;
    ta_table_remove(&datamodel->loops, loop->iter);
    ta_python_drop(datamodel, items);
}

/* A namelist entry's key: its location's text. */
static PyObject *ta_python_location_name(const ta_datamodel_call *call, ta_expression location) {
    ta_expression_info info;
    if (!call->environment ||
        ta_image_expression(ta_datamodel_environment_image(call->environment),
                            (ta_chart_item){ta_datamodel_environment_chart(call->environment), location}, &info) != TA_OK) {
        PyErr_SetString(PyExc_RuntimeError, "no such location");
        return NULL;
    }
    PyObject *source = ta_python_view_str(info.source);
    PyObject *name = source ? PyObject_CallMethod(source, "strip", NULL) : NULL;
    Py_XDECREF(source);
    return name;
}

/* What a <send>, <invoke> or <donedata> captures, copied: actors share no
 * mutable value (bindings.md, section 8). */
static ta_status ta_python_capture(const ta_datamodel_call *call, const ta_datamodel_capture *capture, ta_payload *out) {
    ta_python_scope scope;
    if (ta_python_scope_enter(call, &scope) < 0) return TA_ERROR_EXECUTION;
    ta_python_datamodel *datamodel = scope.datamodel;
    PyObject *captured = NULL;
    if (capture->content != TA_NIL) {
        PyObject *value = ta_python_value(&scope, capture->content);
        captured = value ? ta_python_value_copy(datamodel->state, value) : NULL;
        Py_XDECREF(value);
    } else {
        captured = PyDict_New();
        const ta_image *image = call->environment ? ta_datamodel_environment_image(call->environment) : NULL;
        for (uint32_t i = 0; captured && i < capture->namelist_count; i++) {
            PyObject *name = ta_python_location_name(call, capture->namelist[i]);
            PyObject *value = name ? ta_python_value(&scope, capture->namelist[i]) : NULL;
            PyObject *copy = value ? ta_python_value_copy(datamodel->state, value) : NULL;
            if (!copy || PyDict_SetItem(captured, name, copy) < 0) Py_CLEAR(captured);
            Py_XDECREF(name);
            Py_XDECREF(value);
            Py_XDECREF(copy);
        }
        for (uint32_t i = 0; captured && i < capture->param_count; i++) {
            PyObject *name = image ? ta_python_view_str(ta_image_string(image, capture->params[2 * i])) : NULL;
            PyObject *value = name ? ta_python_value(&scope, capture->params[2 * i + 1]) : NULL;
            PyObject *copy = value ? ta_python_value_copy(datamodel->state, value) : NULL;
            if (!copy || PyDict_SetItem(captured, name, copy) < 0) Py_CLEAR(captured);
            Py_XDECREF(name);
            Py_XDECREF(value);
            Py_XDECREF(copy);
        }
    }
    ta_status status = TA_OK;
    if (!captured) status = ta_python_failed(datamodel);
    else if (ta_python_payload_add(datamodel, captured, out) < 0) {
        PyErr_Clear();
        status = TA_ERROR_NO_MEMORY;
    }
    ta_python_scope_leave(&scope);
    return status;
}

static void ta_python_retain(void *self, ta_payload payload) {
    ta_python_payload_retain(self, payload);
}

static void ta_python_release(void *self, ta_payload payload) {
    ta_python_payload_release(self, payload);
}

static ta_status ta_python_custom(const ta_datamodel_call *call, uint32_t definition) {
    (void)call;
    (void)definition;
    return TA_ERROR_UNSUPPORTED;
}

/* A value as CBOR, in a block of the host's allocator kept between calls:
 * a view until the datamodel's next call (to_wire, serialize,
 * payload_serialize). A value outside the table: TA_ERROR_UNSUPPORTED. */
static ta_status ta_python_wire(ta_python_datamodel *datamodel, PyObject *value, ta_bytes *out) {
    ta_cbor_writer writer;
    ta_python_cbor_init(&writer, &(ta_python_room){{datamodel->wire.pointer, datamodel->wire.capacity, 0}, &datamodel->host->allocator});
    int written = ta_python_value_write(datamodel->state, &writer, value);
    int memory = written < 0 && PyErr_ExceptionMatches(PyExc_MemoryError);
    ta_status status = written < 0 ? (memory ? TA_ERROR_NO_MEMORY : TA_ERROR_UNSUPPORTED) : ta_cbor_writer_bytes(&writer, out);
    if (written < 0) (void)ta_python_failed(datamodel); /* kept if it is a KeyboardInterrupt */
    if (status != TA_OK) {
        ta_cbor_writer_free(&writer); /* a block it grew, never our room */
        return status == TA_ERROR_NO_MEMORY ? status : TA_ERROR_UNSUPPORTED;
    }
    if (writer.bytes != (unsigned char *)datamodel->wire.pointer) { /* it grew: the block is the room now */
        const ta_allocator *allocator = &datamodel->host->allocator;
        if (datamodel->wire.pointer) allocator->free(allocator->context, datamodel->wire.pointer, datamodel->wire.capacity);
        datamodel->wire = (ta_buffer){writer.bytes, writer.capacity, 0};
    }
    return TA_OK;
}

static const char *const ta_python_event_fields[] = {"name", "type", "sendid", "origin", "origintype", "invokeid", "data", "raw"};

/* An Event as the list a blob or a snapshot holds: its eight fields, then core's ids of its
 * name, its invoke and its origintype. */
static PyObject *ta_python_event_list(PyObject *event) {
    PyObject *fields = PyList_New(11);
    for (int i = 0; fields && i < 8; i++) {
        PyObject *field = PyObject_GetAttrString(event, ta_python_event_fields[i]);
        if (!field) Py_CLEAR(fields);
        else PyList_SetItem(fields, i, field);
    }
    ta_python_event *record = (ta_python_event *)event;
    uint32_t ids[3] = {record->name_id, record->invoke_slot, record->origintype_id};
    for (int i = 0; fields && i < 3; i++) {
        PyObject *id = PyLong_FromUnsignedLong(ids[i]);
        if (!id) Py_CLEAR(fields);
        else PyList_SetItem(fields, 8 + i, id);
    }
    return fields;
}

/* Hibernation and export write the session's state into the blob: its
 * <data> values and its _event, as one CBOR map {"vars": {id: value},
 * "event": [name, type, sendid, origin, origintype, invokeid, data, raw,
 * name_id, invoke_slot, origintype_id] or null}, through the value table (no format that runs code when read:
 * bindings.md section 8). Then destroy frees the instance: a hibernated
 * session costs nothing on the Python heap. A value outside the table
 * keeps the session resident (TA_ERROR_UNSUPPORTED). */
static ta_status ta_python_serialize(const ta_datamodel_call *call, ta_bytes *out) {
    ta_python_datamodel *datamodel = call->datamodel;
    ta_python_session_row *row = ta_table_get(&datamodel->sessions, call->instance);
    ta_python_chart_row *chart = row && row->instance ? ta_table_get(&datamodel->charts, row->chart) : NULL;
    if (!chart) return TA_ERROR_INVALID;
    PyObject *instance = Py_NewRef(row->instance), *data = Py_NewRef(chart->data);
    PyObject *vars = PyDict_New(), *state = NULL, *event = NULL, *fields = NULL;
    int status = vars ? 0 : -1;
    for (Py_ssize_t i = 0; status == 0 && i < PyTuple_Size(data); i++) {
        PyObject *id = PyTuple_GetItem(data, i), *value = PyObject_GetAttr(instance, id);
        if (!value || PyDict_SetItem(vars, id, value) < 0) status = -1;
        Py_XDECREF(value);
    }
    if (status == 0) event = PyObject_GetAttr(instance, datamodel->state->event_key);
    if (event && event != Py_None) {
        fields = ta_python_event_list(event);
    } else if (event) fields = Py_NewRef(Py_None);
    if (fields) state = Py_BuildValue("{s:O,s:O}", "vars", vars, "event", fields);
    ta_status result = state ? ta_python_wire(datamodel, state, out) : TA_ERROR_UNSUPPORTED; /* stays resident */
    if (!state) (void)ta_python_failed(datamodel); /* kept if it is a KeyboardInterrupt */
    Py_XDECREF(state);
    Py_XDECREF(fields);
    Py_XDECREF(event);
    Py_XDECREF(vars);
    Py_DECREF(data);
    Py_DECREF(instance);
    return result;
}

/* An Event again, from the list serialize or a snapshot wrote, for the chart its session runs
 * now: its strings shared with the chart's (the name, the invoke's id and origin by core's ids,
 * the type and origintype with the datamodel's) when their text is the same. */
static PyObject *ta_python_event_from(ta_python_datamodel *datamodel, const ta_python_chart_row *chart, PyObject *fields) {
    if (fields == Py_None) return Py_NewRef(Py_None);
    if (!PyList_Check(fields) || PyList_Size(fields) != 11) {
        PyErr_SetString(PyExc_ValueError, "not an event of a hibernation blob");
        return NULL;
    }
    ta_python_event *event = (ta_python_event *)PyType_GenericAlloc((PyTypeObject *)datamodel->state->event_type, 0);
    if (!event) return NULL;
    PyObject **slots[8] = {&event->name, &event->type, &event->sendid, &event->origin,
                           &event->origintype, &event->invokeid, &event->data, &event->raw};
    for (int i = 0; i < 8; i++) *slots[i] = Py_NewRef(PyList_GetItem(fields, i));
    uint32_t *ids[3] = {&event->name_id, &event->invoke_slot, &event->origintype_id};
    for (int i = 0; i < 3; i++) {
        unsigned long id = PyLong_AsUnsignedLong(PyList_GetItem(fields, 8 + i));
        *ids[i] = PyErr_Occurred() ? TA_NIL : (uint32_t)id;
    }
    PyErr_Clear();
    for (Py_ssize_t i = 0; i < PyTuple_Size(datamodel->state->event_kinds); i++) { /* one of three */
        PyObject *kind = PyTuple_GetItem(datamodel->state->event_kinds, i);
        if (PyObject_RichCompareBool(kind, event->type, Py_EQ) == 1) {
            Py_DECREF(event->type);
            event->type = Py_NewRef(kind);
        }
    }
    PyErr_Clear();
    ta_python_kept_at name_at = {chart->names, event->name_id, 0};
    ta_python_kept_at invokeid_at = {chart->invokeids, event->invoke_slot, 1};
    ta_python_kept_at origin_at = {chart->origins, event->invoke_slot, 1};
    if (event->name != Py_None) event->name = ta_python_kept(&name_at, ta_python_text_of(event->name), event->name);
    if (event->invokeid != Py_None)
        event->invokeid = ta_python_kept(&invokeid_at, ta_python_text_of(event->invokeid), event->invokeid);
    if (event->origin != Py_None) event->origin = ta_python_kept(&origin_at, ta_python_text_of(event->origin), event->origin);
    if (event->origintype != Py_None) {
        PyObject *shared = ta_python_origintype(datamodel, event->origintype_id, ta_python_text_of(event->origintype));
        if (shared) {
            Py_DECREF(event->origintype);
            event->origintype = shared;
        }
        else PyErr_Clear();
    }
    if (!event->name || !event->invokeid || !event->origin) {
        Py_DECREF(event);
        return NULL;
    }
    return (PyObject *)event;
}

/* A session from a blob serialize wrote, for its chart now: an instance
 * of the chart's class with the values the chart declares. Every value of
 * the blob is kept until the first event (or migrate takes it): when the
 * blob is of an older version of the chart, migrate answers them as the
 * old bindings. */
static ta_status ta_python_deserialize(void *self, const ta_datamodel_start *start, uint64_t *out) {
    ta_python_datamodel *datamodel = self;
    ta_python_keep_origintypes(datamodel, start->system_variables);
    ta_python_chart_row *chart = ta_table_get(&datamodel->charts, (uint64_t)(uintptr_t)start->chart_context);
    PyObject *state = chart ? ta_python_value_read(datamodel->state, start->state) : NULL;
    PyObject *vars = NULL, *fields = NULL, *event = NULL, *instance = NULL;
    if (state && PyDict_Check(state)) {
        PyDict_GetItemStringRef(state, "vars", &vars);
        PyDict_GetItemStringRef(state, "event", &fields);
    }
    int status = vars && PyDict_Check(vars) && fields ? 0 : -1;
    if (status == 0) instance = ta_python_instance(datamodel, chart, start->system_variables);
    if (status == 0 && instance) event = ta_python_event_from(datamodel, chart, fields);
    if (!instance || !event || PyObject_SetAttr(instance, datamodel->state->event_key, event) < 0) status = -1;
    PyObject *data = chart ? Py_NewRef(chart->data) : NULL;
    for (Py_ssize_t i = 0; status == 0 && i < PyTuple_Size(data); i++) {
        PyObject *id = PyTuple_GetItem(data, i), *value = NULL;
        if (PyDict_GetItemRef(vars, id, &value) < 0) status = -1;
        else if (value) status = PyObject_SetAttr(instance, id, value);
        Py_XDECREF(value);
    }
    Py_XDECREF(data);
    Py_XDECREF(event);
    Py_XDECREF(fields);
    Py_XDECREF(state);
    ta_python_session_row *row = NULL;
    uint64_t handle = 0;
    if (status == 0 && !(row = ta_table_add(&datamodel->sessions, &handle))) status = -2;
    if (status < 0) {
        if (status == -1) ta_python_failed(datamodel);
        ta_python_drop(datamodel, instance);
        ta_python_drop(datamodel, vars);
        return status == -2 ? TA_ERROR_NO_MEMORY : TA_ERROR_INVALID;
    }
    row->instance = instance;
    row->chart = (uint64_t)(uintptr_t)start->chart_context;
    row->extra = vars; /* the old bindings, for migrate, until the first event */
    *out = handle;
    return TA_OK;
}

static ta_status ta_python_migrate(const ta_datamodel_call *call, ta_payload *bindings) {
    ta_python_datamodel *datamodel = call->datamodel;
    ta_python_session_row *row = ta_table_get(&datamodel->sessions, call->instance);
    *bindings = 0;
    if (!row || !row->extra) return TA_OK;
    PyObject *taken = row->extra;
    row->extra = NULL;
    return ta_python_payload_add(datamodel, taken, bindings) < 0 ? (PyErr_Clear(), TA_ERROR_NO_MEMORY) : TA_OK;
}

/* A payload's value as CBOR (payload_serialize): TA_ERROR_UNSUPPORTED when
 * it is outside the table. */
static ta_status ta_python_payload_serialize(void *self, ta_payload payload, ta_bytes *out) {
    ta_python_datamodel *datamodel = self;
    PyObject *value = ta_python_payload_value(datamodel, payload);
    if (!value) return TA_ERROR_INVALID;
    Py_INCREF(value);
    ta_status status = ta_python_wire(datamodel, value, out);
    Py_DECREF(value);
    return status;
}

/* The same as a neutral value (to_wire): TA_ERROR_INVALID when it cannot be one. */
static ta_status ta_python_to_wire(void *self, ta_payload payload, ta_bytes *out) {
    ta_status status = ta_python_payload_serialize(self, payload, out);
    return status == TA_ERROR_UNSUPPORTED ? TA_ERROR_INVALID : status;
}

static ta_status ta_python_from_wire(void *self, ta_bytes cbor, ta_payload *out) {
    ta_python_datamodel *datamodel = self;
    PyObject *value = ta_python_value_read(datamodel->state, cbor);
    if (!value) {
        PyErr_Clear();
        return TA_ERROR_INVALID;
    }
    if (ta_python_payload_add(datamodel, value, out) < 0) {
        PyErr_Clear();
        return TA_ERROR_NO_MEMORY;
    }
    return TA_OK;
}

static ta_status ta_python_variable(const ta_datamodel_call *call, ta_string_view name, ta_payload *out) {
    ta_python_datamodel *datamodel = call->datamodel;
    ta_python_session_row *row = ta_table_get(&datamodel->sessions, call->instance);
    ta_python_chart_row *chart = row && row->instance ? ta_table_get(&datamodel->charts, row->chart) : NULL;
    PyObject *id = chart ? ta_python_view_str(name) : NULL;
    int declared = id ? PySequence_Contains(chart->data, id) : 0;
    PyObject *value = declared == 1 ? PyObject_GetAttr(row->instance, id) : NULL;
    PyObject *copy = value ? ta_python_value_copy(datamodel->state, value) : NULL;
    Py_XDECREF(id);
    Py_XDECREF(value);
    if (!copy) {
        PyErr_Clear();
        return TA_ERROR_INVALID;
    }
    if (ta_python_payload_add(datamodel, copy, out) < 0) {
        PyErr_Clear();
        return TA_ERROR_NO_MEMORY;
    }
    return TA_OK;
}

/* ---- snapshots: the tables' namespaces and every row's values ------------------------ */

/* A table's handle namespace as bytes (ta_table_snapshot). */
static PyObject *ta_python_table_layout(const ta_table *table) {
    ta_buffer probe = {NULL, 0, 0};
    ta_table_snapshot(table, &probe); /* TA_ERROR_NO_MEMORY: probe.length is the room */
    PyObject *bytes = PyBytes_FromStringAndSize(NULL, (Py_ssize_t)probe.length);
    ta_buffer out = {bytes ? PyBytes_AsString(bytes) : NULL, probe.length, 0};
    if (bytes && ta_table_snapshot(table, &out) != TA_OK) Py_CLEAR(bytes);
    return bytes;
}

/* One value as CBOR bytes of its own, so that a value nests as deep in a
 * snapshot as anywhere else: TinyactorsError TA_ERROR_UNSUPPORTED for one
 * outside the value table, MemoryError when memory is refused. */
static PyObject *ta_python_value_bytes(ta_python_datamodel *datamodel, PyObject *value) {
    ta_bytes bytes;
    ta_status status = ta_python_wire(datamodel, value, &bytes);
    if (status == TA_OK) return PyBytes_FromStringAndSize(bytes.pointer, (Py_ssize_t)bytes.length);
    if (status == TA_ERROR_NO_MEMORY) PyErr_NoMemory();
    else ta_python_raise_status(datamodel->state, status);
    return NULL;
}

/* The value of bytes ta_python_value_bytes wrote. */
static PyObject *ta_python_bytes_value(ta_python_datamodel *datamodel, PyObject *bytes) {
    if (!PyBytes_Check(bytes)) {
        PyErr_SetString(PyExc_ValueError, "not a snapshot of the python datamodel");
        return NULL;
    }
    return ta_python_value_read(datamodel->state, (ta_bytes){PyBytes_AsString(bytes), (size_t)PyBytes_Size(bytes)});
}

/* One session row: [handle, segment, chart index, vars, _event's fields or
 * None, extra or None (each as bytes of its own), the system variables or
 * None]. The segment and index name the chart's attachment, which a
 * restore makes again under another handle. */
static PyObject *ta_python_session_entry(ta_python_datamodel *datamodel, uint32_t slot) {
    ta_python_session_row *row = ta_table_at(&datamodel->sessions, slot);
    ta_python_chart_row *chart = row ? ta_table_get(&datamodel->charts, row->chart) : NULL;
    if (!chart) {
        PyErr_SetString(PyExc_RuntimeError, "a session without its chart");
        return NULL;
    }
    PyObject *instance = row->instance, *vars = PyDict_New(), *event = NULL, *fields = NULL, *system = NULL;
    int status = vars ? 0 : -1;
    for (Py_ssize_t i = 0; status == 0 && i < PyTuple_Size(chart->data); i++) {
        PyObject *id = PyTuple_GetItem(chart->data, i), *value = PyObject_GetAttr(instance, id);
        if (!value || PyDict_SetItem(vars, id, value) < 0) status = -1;
        Py_XDECREF(value);
    }
    if (status == 0) event = PyObject_GetAttr(instance, datamodel->state->event_key);
    if (event && event != Py_None) {
        fields = ta_python_event_list(event);
    } else if (event) fields = Py_NewRef(Py_None);
    PyObject *sys = fields ? PyObject_GetAttrString(instance, "_ta_sys") : NULL;
    if (sys) system = sys == Py_None ? Py_NewRef(Py_None) : PySequence_List(sys);
    PyObject *vars_bytes = system ? ta_python_value_bytes(datamodel, vars) : NULL;
    PyObject *event_bytes = !vars_bytes ? NULL : fields == Py_None ? Py_NewRef(Py_None) : ta_python_value_bytes(datamodel, fields);
    PyObject *extra_bytes = !event_bytes ? NULL : !row->extra ? Py_NewRef(Py_None) : ta_python_value_bytes(datamodel, row->extra);
    PyObject *entry = extra_bytes ? Py_BuildValue("[KIIOOOO]", (unsigned long long)ta_table_handle(&datamodel->sessions, slot),
                                                  chart->segment, chart->index, vars_bytes, event_bytes, extra_bytes, system)
                                  : NULL;
    Py_XDECREF(vars_bytes);
    Py_XDECREF(event_bytes);
    Py_XDECREF(extra_bytes);
    Py_XDECREF(sys);
    Py_XDECREF(system);
    Py_XDECREF(fields);
    Py_XDECREF(event);
    Py_XDECREF(vars);
    return entry;
}

PyObject *ta_python_datamodel_snapshot(ta_python_datamodel *datamodel) {
    PyObject *sessions = PyList_New(0), *values = PyList_New(0), *items = PyList_New(0), *state = NULL;
    int status = sessions && values && items ? 0 : -1;
    for (uint32_t slot = 0; status == 0 && slot < datamodel->sessions.capacity; slot++) {
        if (!ta_table_at(&datamodel->sessions, slot)) continue;
        PyObject *entry = ta_python_session_entry(datamodel, slot);
        if (!entry || PyList_Append(sessions, entry) < 0) status = -1;
        Py_XDECREF(entry);
    }
    for (uint32_t slot = 0; status == 0 && slot < datamodel->payloads.capacity; slot++) {
        ta_python_payload_row *row = ta_table_at(&datamodel->payloads, slot);
        if (!row) continue;
        PyObject *value = ta_python_value_bytes(datamodel, row->value);
        PyObject *entry = value ? Py_BuildValue("[KIO]", (unsigned long long)ta_table_handle(&datamodel->payloads, slot),
                                                row->references, value)
                                : NULL;
        if (!entry || PyList_Append(values, entry) < 0) status = -1;
        Py_XDECREF(entry);
        Py_XDECREF(value);
    }
    for (uint32_t slot = 0; status == 0 && slot < datamodel->loops.capacity; slot++) {
        ta_python_loop_row *row = ta_table_at(&datamodel->loops, slot);
        if (!row) continue;
        PyObject *list = PySequence_List(row->items);
        PyObject *bytes = list ? ta_python_value_bytes(datamodel, list) : NULL;
        PyObject *entry = bytes ? Py_BuildValue("[KO]", (unsigned long long)ta_table_handle(&datamodel->loops, slot), bytes) : NULL;
        if (!entry || PyList_Append(items, entry) < 0) status = -1;
        Py_XDECREF(entry);
        Py_XDECREF(bytes);
        Py_XDECREF(list);
    }
    PyObject *layouts[3] = {NULL, NULL, NULL};
    if (status == 0) {
        layouts[0] = ta_python_table_layout(&datamodel->sessions);
        layouts[1] = layouts[0] ? ta_python_table_layout(&datamodel->payloads) : NULL;
        layouts[2] = layouts[1] ? ta_python_table_layout(&datamodel->loops) : NULL;
    }
    if (layouts[2])
        state = Py_BuildValue("{s:O,s:O,s:O,s:O,s:O,s:O}", "sessions", layouts[0], "payloads", layouts[1], "loops", layouts[2],
                              "rows", sessions, "values", values, "items", items);
    for (int i = 0; i < 3; i++) Py_XDECREF(layouts[i]);
    Py_XDECREF(sessions);
    Py_XDECREF(values);
    Py_XDECREF(items);
    if (!state) return NULL;
    ta_cbor_writer writer; /* through the host's allocator, freed here: a snapshot's size is no datamodel's room */
    ta_python_cbor_init(&writer, &(ta_python_room){{NULL, 0, 0}, NULL});
    PyObject *result = NULL;
    if (ta_python_value_write(datamodel->state, &writer, state) == 0) { /* bytes and handles: only memory can fail */
        ta_bytes bytes;
        if (ta_cbor_writer_bytes(&writer, &bytes) == TA_OK)
            result = PyBytes_FromStringAndSize(bytes.pointer, (Py_ssize_t)bytes.length);
        else PyErr_NoMemory();
    }
    ta_cbor_writer_free(&writer);
    Py_DECREF(state);
    return result;
}

/* A table's namespace back from the snapshot's bytes. */
static int ta_python_table_restore(ta_table *table, PyObject *layout) {
    char *pointer;
    Py_ssize_t length;
    if (!layout || !PyBytes_Check(layout) || PyBytes_AsStringAndSize(layout, &pointer, &length) < 0) {
        if (!PyErr_Occurred()) PyErr_SetString(PyExc_ValueError, "not a snapshot of the python datamodel");
        return -1;
    }
    ta_status status = ta_table_restore(table, (ta_bytes){pointer, (size_t)length});
    if (status == TA_ERROR_NO_MEMORY) PyErr_NoMemory();
    else if (status != TA_OK) PyErr_SetString(PyExc_ValueError, "a table of the snapshot does not restore");
    return status == TA_OK ? 0 : -1;
}

/* The restored host's charts by attachment, (segment, chart index) ->
 * the chart row's handle: built once for every session to look up. */
static PyObject *ta_python_chart_index(ta_python_datamodel *datamodel) {
    PyObject *index = PyDict_New();
    for (uint32_t slot = 0; index && slot < datamodel->charts.capacity; slot++) {
        ta_python_chart_row *row = ta_table_at(&datamodel->charts, slot);
        if (!row) continue;
        PyObject *key = Py_BuildValue("(II)", row->segment, row->index);
        PyObject *handle = key ? PyLong_FromUnsignedLongLong(ta_table_handle(&datamodel->charts, slot)) : NULL;
        if (!handle || PyDict_SetItem(index, key, handle) < 0) Py_CLEAR(index);
        Py_XDECREF(key);
        Py_XDECREF(handle);
    }
    return index;
}

/* What a session's restore needs besides its entry: the charts by attachment. */
typedef struct ta_python_session_source {
    PyObject *entry;
    PyObject *charts;
} ta_python_session_source;

/* One session row back: an instance of its chart's class with its values. */
static int ta_python_session_restore(ta_python_datamodel *datamodel, const ta_python_session_source *source) {
    unsigned long long handle;
    unsigned int segment, index;
    PyObject *vars_bytes, *event_bytes, *extra_bytes, *system;
    if (!PyList_Check(source->entry) || PyList_Size(source->entry) != 7) {
        PyErr_SetString(PyExc_ValueError, "not a session of a snapshot");
        return -1;
    }
    PyObject *tuple = PyList_AsTuple(source->entry); /* borrowed values live while the entry does */
    int parsed = tuple && PyArg_ParseTuple(tuple, "KIIOOOO", &handle, &segment, &index, &vars_bytes, &event_bytes,
                                           &extra_bytes, &system);
    Py_XDECREF(tuple);
    if (!parsed) return -1;
    PyObject *key = Py_BuildValue("(II)", segment, index), *found = NULL;
    if (!key || PyDict_GetItemRef(source->charts, key, &found) < 0) {
        Py_XDECREF(key);
        return -1;
    }
    Py_DECREF(key);
    uint64_t chart_handle = found ? PyLong_AsUnsignedLongLong(found) : 0;
    Py_XDECREF(found);
    ta_python_chart_row *chart = ta_table_get(&datamodel->charts, chart_handle);
    ta_python_session_row *row = ta_table_get(&datamodel->sessions, handle);
    if (!chart || !row || row->instance) {
        PyErr_SetString(PyExc_ValueError, !chart ? "a session whose chart the restored host does not have"
                                                 : "a session the snapshot's tables do not hold, or twice");
        return -1;
    }
    PyObject *vars = ta_python_bytes_value(datamodel, vars_bytes);
    PyObject *fields = !vars ? NULL : event_bytes == Py_None ? Py_NewRef(Py_None) : ta_python_bytes_value(datamodel, event_bytes);
    PyObject *extra = !fields ? NULL : extra_bytes == Py_None ? Py_NewRef(Py_None) : ta_python_bytes_value(datamodel, extra_bytes);
    PyObject *instance = extra && PyDict_Check(vars) ? PyObject_CallNoArgs(chart->chart) : NULL;
    PyObject *sys = !instance ? NULL : system == Py_None ? Py_NewRef(Py_None) : PySequence_Tuple(system);
    PyObject *event = sys ? ta_python_event_from(datamodel, chart, fields) : NULL;
    int status = event && PyObject_SetAttrString(instance, "_ta_sys", sys) == 0 &&
                         PyObject_SetAttr(instance, datamodel->state->event_key, event) == 0
                     ? 0
                     : -1;
    for (Py_ssize_t i = 0; status == 0 && i < PyTuple_Size(chart->data); i++) {
        PyObject *id = PyTuple_GetItem(chart->data, i), *value = NULL;
        if (PyDict_GetItemRef(vars, id, &value) < 0) status = -1;
        else status = PyObject_SetAttr(instance, id, value ? value : Py_None);
        Py_XDECREF(value);
    }
    if (status == 0) {
        row->instance = Py_NewRef(instance);
        row->chart = chart_handle;
        row->extra = extra == Py_None ? NULL : Py_NewRef(extra);
    } else if (!PyErr_Occurred()) {
        PyErr_SetString(PyExc_ValueError, "not a session of a snapshot");
    }
    ta_python_drop(datamodel, instance);
    Py_XDECREF(sys);
    Py_XDECREF(event);
    Py_XDECREF(vars);
    Py_XDECREF(fields);
    Py_XDECREF(extra);
    return status;
}

int ta_python_datamodel_restore(ta_python_datamodel *datamodel, ta_bytes bytes) {
    if (datamodel->sessions.count || datamodel->payloads.count || datamodel->loops.count) {
        ta_python_raise_status(datamodel->state, TA_ERROR_STATE); /* not a fresh datamodel */
        return -1;
    }
    PyObject *state = ta_python_value_read(datamodel->state, bytes);
    PyObject *parts[6] = {NULL, NULL, NULL, NULL, NULL, NULL}, *charts = NULL;
    static const char *const names[] = {"sessions", "payloads", "loops", "rows", "values", "items"};
    int status = state && PyDict_Check(state) ? 0 : -1;
    for (int i = 0; status == 0 && i < 6; i++)
        if (PyDict_GetItemStringRef(state, names[i], &parts[i]) <= 0 || (i >= 3 && !PyList_Check(parts[i]))) status = -1;
    if (status < 0 && !PyErr_Occurred()) PyErr_SetString(PyExc_ValueError, "not a snapshot of the python datamodel");
    if (status == 0 && (ta_python_table_restore(&datamodel->sessions, parts[0]) < 0 ||
                        ta_python_table_restore(&datamodel->payloads, parts[1]) < 0 ||
                        ta_python_table_restore(&datamodel->loops, parts[2]) < 0))
        status = -1;
    /* every live handle has its row, once */
    if (status == 0 && ((size_t)PyList_Size(parts[3]) != datamodel->sessions.count ||
                        (size_t)PyList_Size(parts[4]) != datamodel->payloads.count ||
                        (size_t)PyList_Size(parts[5]) != datamodel->loops.count)) {
        PyErr_SetString(PyExc_ValueError, "the snapshot's rows do not match its tables");
        status = -1;
    }
    if (status == 0 && !(charts = ta_python_chart_index(datamodel))) status = -1;
    for (Py_ssize_t i = 0; status == 0 && i < PyList_Size(parts[3]); i++)
        status = ta_python_session_restore(datamodel, &(ta_python_session_source){PyList_GetItem(parts[3], i), charts});
    for (Py_ssize_t i = 0; status == 0 && i < PyList_Size(parts[4]); i++) {
        unsigned long long handle;
        unsigned int references;
        PyObject *value_bytes, *entry = PyList_GetItem(parts[4], i);
        PyObject *tuple = PyList_Check(entry) ? PyList_AsTuple(entry) : NULL;
        ta_python_payload_row *row = NULL;
        if (!tuple || !PyArg_ParseTuple(tuple, "KIO", &handle, &references, &value_bytes) ||
            !(row = ta_table_get(&datamodel->payloads, handle)) || row->value) {
            if (!PyErr_Occurred()) PyErr_SetString(PyExc_ValueError, "a payload the snapshot's tables do not hold, or twice");
            status = -1;
        } else if (!(row->value = ta_python_bytes_value(datamodel, value_bytes))) status = -1;
        else row->references = references;
        Py_XDECREF(tuple);
    }
    for (Py_ssize_t i = 0; status == 0 && i < PyList_Size(parts[5]); i++) {
        unsigned long long handle;
        PyObject *items_bytes, *entry = PyList_GetItem(parts[5], i);
        PyObject *tuple = PyList_Check(entry) ? PyList_AsTuple(entry) : NULL;
        ta_python_loop_row *row = NULL;
        if (!tuple || !PyArg_ParseTuple(tuple, "KO", &handle, &items_bytes) ||
            !(row = ta_table_get(&datamodel->loops, handle)) || row->items) {
            if (!PyErr_Occurred()) PyErr_SetString(PyExc_ValueError, "a <foreach> the snapshot's tables do not hold, or twice");
            status = -1;
        } else {
            PyObject *list = ta_python_bytes_value(datamodel, items_bytes);
            row->items = list && PyList_Check(list) ? PyList_AsTuple(list) : NULL;
            if (!row->items) {
                if (!PyErr_Occurred()) PyErr_SetString(PyExc_ValueError, "a <foreach>'s items are not a list");
                status = -1;
            }
            Py_XDECREF(list);
        }
        Py_XDECREF(tuple);
    }
    Py_XDECREF(charts);
    for (int i = 0; i < 6; i++) Py_XDECREF(parts[i]);
    Py_XDECREF(state);
    return status; /* on failure the host is freed: the rows filled so far go with it */
}

static const ta_datamodel_vtable ta_python_vtable = {
    .size = sizeof(ta_datamodel_vtable),
    .flags = TA_DATAMODEL_EVENT_DETACHABLE,
    .name = "python",
    .chart_attach = ta_python_chart_attach,
    .chart_detach = ta_python_chart_detach,
    .create = ta_python_create,
    .destroy = ta_python_destroy,
    .init_data = ta_python_init_data,
    .bind_event = ta_python_bind_event,
    .detach_event = ta_python_detach_event,
    .eval_condition = ta_python_eval_condition,
    .eval_string = ta_python_eval_string,
    .assign = ta_python_assign,
    .assign_string = ta_python_assign_string,
    .script = ta_python_script,
    .log = ta_python_log,
    .foreach_begin = ta_python_foreach_begin,
    .foreach_item = ta_python_foreach_item,
    .foreach_end = ta_python_foreach_end,
    .capture = ta_python_capture,
    .retain = ta_python_retain,
    .release = ta_python_release,
    .custom = ta_python_custom,
    .serialize = ta_python_serialize,
    .deserialize = ta_python_deserialize,
    .migrate = ta_python_migrate,
    .to_wire = ta_python_to_wire,
    .from_wire = ta_python_from_wire,
    .payload_serialize = ta_python_payload_serialize, /* a delayed send's payload in an export blob: its CBOR */
    .payload_deserialize = ta_python_from_wire,
    .variable = ta_python_variable,
};

const ta_datamodel_vtable *ta_python_datamodel_vtable(void) {
    return &ta_python_vtable;
}
