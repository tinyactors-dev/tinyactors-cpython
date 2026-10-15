/* The Host (AGENTS.md, "The API"): ta_host_create from Python options, the
 * inputs (add_image, chart, spawn, send, send_many, terminate, boot),
 * the call's arena mark, and close. The host's allocator is here, and
 * nowhere else calls an allocator (tools/lint/rules/
 * allocation-in-tables-and-host.yml). The loop is loop.c, the effects
 * effects.c, the typed methods methods.c. */
#include "extension.h"
#include "docs.h"
#include <string.h>

/* ---- the host's allocator ------------------------------------------------------ */

/* Core's alloc: a raw block or NULL to refuse. Core asks for an alignment
 * of at most 8 (allocator.h), and the raw domain gives 16 (AGENTS.md,
 * "The host's allocator"). Called without the GIL's help: the raw domain
 * needs none. */
static void *ta_python_alloc(void *context, size_t size, size_t align) {
    (void)context;
    assert(align <= 8);
    (void)align;
    return PyMem_RawMalloc(size);
}

static void ta_python_free(void *context, void *block, size_t size) {
    (void)context;
    (void)size;
    PyMem_RawFree(block);
}

static const ta_allocator ta_python_allocator = {NULL, ta_python_alloc, ta_python_free};

/* A writer in `room` growing through `allocator`, or the host's raw
 * allocator (tinyactors.encode, which has no host). */
void ta_python_cbor_init(ta_cbor_writer *writer, const ta_python_room *room) {
    ta_cbor_writer_init(writer, room->room, room->allocator ? room->allocator : &ta_python_allocator);
}

/* The compiler allocates through the host's allocator too (compile.c):
 * ta_compile_system of the one source when it is a manifest, else
 * ta_compile_charts of them all. */
ta_status ta_python_compile(ta_compile_options *options, const ta_python_compilation *compilation,
                            ta_compile_result **out) {
    options->alloc = ta_python_allocator;
    if (compilation->manifest) return ta_compile_system(options, compilation->charts.pointer, out);
    return ta_compile_charts(options, compilation->charts, out);
}

/* ---- errors -------------------------------------------------------------------- */

static PyObject *ta_python_error_new(ta_python_module_state *state, PyObject *arguments, PyObject *keywords) {
    return PyObject_Call(state->error, arguments, keywords);
}

void ta_python_raise_status(ta_python_module_state *state, ta_status status) {
    if (status == TA_ERROR_NO_MEMORY) {
        PyErr_NoMemory();
        return;
    }
    const char *name = ta_status_name(status);
    PyObject *keywords = Py_BuildValue("{s:s}", "status", name ? name : "unknown");
    PyObject *arguments = keywords ? Py_BuildValue("(s)", name ? name : "unknown status") : NULL;
    PyObject *error = arguments ? ta_python_error_new(state, arguments, keywords) : NULL;
    Py_XDECREF(arguments);
    Py_XDECREF(keywords);
    if (error) {
        PyErr_SetObject(state->error, error);
        Py_DECREF(error);
    }
}

/* {"code", "message", "data": {"reason", "path"}} as core wrote it. */
PyObject *ta_python_error_from_reply(ta_python_module_state *state, PyObject *error) {
    PyObject *message = NULL, *code = NULL, *data = NULL, *reason = NULL, *path = NULL;
    if (PyDict_Check(error)) {
        PyDict_GetItemStringRef(error, "message", &message);
        PyDict_GetItemStringRef(error, "code", &code);
        if (PyDict_GetItemStringRef(error, "data", &data) > 0 && PyDict_Check(data)) {
            PyDict_GetItemStringRef(data, "reason", &reason);
            PyDict_GetItemStringRef(data, "path", &path);
        }
    }
    PyObject *keywords = Py_BuildValue("{s:O,s:O,s:O}", "code", code ? code : Py_None, "reason",
                                       reason ? reason : Py_None, "path", path ? path : Py_None);
    PyObject *arguments = keywords ? PyTuple_Pack(1, message ? message : Py_None) : NULL;
    PyObject *result = arguments ? ta_python_error_new(state, arguments, keywords) : NULL;
    Py_XDECREF(arguments);
    Py_XDECREF(keywords);
    Py_XDECREF(message);
    Py_XDECREF(code);
    Py_XDECREF(data);
    Py_XDECREF(reason);
    Py_XDECREF(path);
    return result;
}

/* ---- a call --------------------------------------------------------------------- */

int ta_python_enter(ta_python_host *host, ta_python_call *call) {
    if (!host->host) {
        ta_python_raise_status(host->state, TA_ERROR_STATE);
        return -1;
    }
    if (PyThread_get_thread_ident() != host->thread) { /* bindings.md, section 5 */
        PyErr_SetString(host->state->error, "a Host is called only from the thread that made it");
        return -1;
    }
    call->host = host;
    call->mark = ta_growable_arena_position(&host->arena);
    host->depth++;
    return 0;
}

void ta_python_leave(ta_python_call *call) {
    ta_python_host *host = call->host;
    ta_growable_arena_release(&host->arena, call->mark);
    host->depth--;
    if (host->depth == 0 && !host->turning) {
        if (host->closing) ta_python_host_close(host, 0);
        else ta_python_datamodel_flush(&host->python);
    }
}

int ta_python_text(PyObject *text, ta_string_view *out) {
    if (text == NULL || text == Py_None) {
        *out = (ta_string_view){"", 0};
        return 0;
    }
    if (!PyUnicode_Check(text)) {
        PyErr_SetString(PyExc_TypeError, "expected a str");
        return -1;
    }
    Py_ssize_t length;
    const char *pointer = PyUnicode_AsUTF8AndSize(text, &length);
    if (!pointer) return -1;
    if ((size_t)length > UINT32_MAX) {
        PyErr_SetString(PyExc_OverflowError, "text past core's length limit");
        return -1;
    }
    *out = (ta_string_view){pointer, (uint32_t)length};
    return 0;
}

void ta_python_host_keep_error(ta_python_host *host) {
    PyObject *error = PyErr_GetRaisedException();
    if (!host->error) host->error = error;
    else Py_XDECREF(error);
}

/* ---- payloads: a value for a session's datamodel ----------------------------------- */

static uint32_t ta_python_datamodel_index(ta_string_view name) {
    if (name.length == 6 && !memcmp(name.pointer, "python", 6)) return TA_PYTHON_DATAMODEL_PYTHON;
    if (name.length == 10 && !memcmp(name.pointer, "ecmascript", 10)) return TA_PYTHON_DATAMODEL_ECMASCRIPT;
    return TA_NIL;
}

static uint32_t ta_python_chart_datamodel(ta_python_host *host, ta_host_chart_ref chart) {
    ta_host_segment_info info;
    memset(&info, 0, sizeof info);
    info.size = sizeof info;
    if (ta_host_segment_info_get(host->host, chart.segment, &info) != TA_OK || !info.image) return TA_NIL;
    return ta_python_datamodel_index(ta_image_chart_datamodel(info.image, chart.chart));
}

uint32_t ta_python_host_session_datamodel(ta_python_host *host, uint64_t session) {
    ta_host_location location;
    if (ta_host_locate(host->host, session, &location) != TA_OK) return TA_NIL;
    ta_system *system = ta_host_system(host->host, location.segment);
    ta_actor_info info;
    memset(&info, 0, sizeof info);
    info.size = sizeof info;
    if (!system || ta_actor_info_get(system, location.actor, &info) != TA_OK) return TA_NIL;
    return ta_python_chart_datamodel(host, (ta_host_chart_ref){location.segment, info.chart});
}

int ta_python_host_payload(ta_python_host *host, ta_python_payload_for *target) {
    target->payload = 0;
    target->datamodel = TA_NIL;
    if (target->value == NULL || target->value == Py_None) return 0;
    uint32_t datamodel = target->session ? ta_python_host_session_datamodel(host, target->session)
                                         : ta_python_chart_datamodel(host, target->chart);
    if (datamodel == TA_PYTHON_DATAMODEL_PYTHON) {
        if (ta_python_payload_new(&host->python, target->value, &target->payload) < 0) return -1;
    } else if (datamodel == TA_PYTHON_DATAMODEL_ECMASCRIPT) {
        /* CBOR toward quickjs, in the call's arena (AGENTS.md, "The binding's memory") */
        ta_cbor_writer writer;
        ta_python_cbor_init(&writer, &(ta_python_room){{NULL, 0, 0}, &host->arena_allocator});
        if (ta_python_value_write(host->state, &writer, target->value) < 0) return -1;
        ta_bytes cbor;
        ta_status status = ta_cbor_writer_bytes(&writer, &cbor);
        if (status == TA_OK) status = ta_datamodel_ecmascript_payload_from_cbor(host->ecmascript, cbor, &target->payload);
        if (status != TA_OK) {
            ta_python_raise_status(host->state, status);
            return -1;
        }
    } else if (target->session && !ta_host_locate(host->host, target->session, &(ta_host_location){0, 0})) {
        PyErr_SetString(PyExc_TypeError, "the session's chart has a datamodel that takes no data");
        return -1;
    } else {
        ta_python_raise_status(host->state, TA_ERROR_STALE); /* no such session */
        return -1;
    }
    target->datamodel = datamodel;
    return 0;
}

void ta_python_host_payload_release(ta_python_host *host, ta_payload payload, uint32_t datamodel) {
    if (!payload || datamodel >= TA_PYTHON_DATAMODELS) return;
    host->datamodels[datamodel].vtable->release(host->datamodels[datamodel].datamodel, payload);
}

PyObject *ta_python_host_payload_value(ta_python_host *host, const ta_python_payload_of *payload) {
    if (!payload->payload) return Py_NewRef(Py_None);
    if (payload->datamodel == TA_PYTHON_DATAMODEL_PYTHON) {
        PyObject *value = ta_python_payload_value(&host->python, payload->payload);
        return Py_NewRef(value ? value : Py_None);
    }
    if (payload->datamodel == TA_PYTHON_DATAMODEL_ECMASCRIPT) {
        ta_bytes cbor; /* quickjs's, until its next call: read at once */
        ta_status status = ta_datamodel_ecmascript_payload_to_cbor(host->ecmascript, payload->payload, &cbor);
        if (status != TA_OK) {
            ta_python_raise_status(host->state, status);
            return NULL;
        }
        return ta_python_value_read(host->state, cbor);
    }
    return Py_NewRef(Py_None);
}

/* ---- ChartRef ---------------------------------------------------------------------- */

typedef struct ta_python_chart_ref {
    PyObject_HEAD
    uint32_t segment;
    uint32_t chart;
} ta_python_chart_ref;

static PyMemberDef ta_python_chart_members[] = {
    {"segment", Py_T_UINT, offsetof(ta_python_chart_ref, segment), Py_READONLY, ta_python_doc_chart_ref_segment},
    {"chart", Py_T_UINT, offsetof(ta_python_chart_ref, chart), Py_READONLY, ta_python_doc_chart_ref_chart},
    {NULL, 0, 0, 0, NULL}
};

static PyObject *ta_python_chart_repr(PyObject *self) {
    ta_python_chart_ref *ref = (ta_python_chart_ref *)self;
    return PyUnicode_FromFormat("ChartRef(segment=%u, chart=%u)", ref->segment, ref->chart);
}

static void ta_python_simple_dealloc(PyObject *self) {
    PyTypeObject *type = Py_TYPE(self);
    freefunc ta_python_release = (freefunc)PyType_GetSlot(type, Py_tp_free);
    ta_python_release(self);
    Py_DECREF(type);
}

static PyType_Slot ta_python_chart_slots[] = {
    {Py_tp_doc, (void *)ta_python_doc_chart_ref},
    {Py_tp_members, ta_python_chart_members},
    {Py_tp_repr, ta_python_chart_repr},
    {Py_tp_dealloc, ta_python_simple_dealloc},
    {0, NULL}
};

static PyType_Spec ta_python_chart_spec = {
    "tinyactors.ChartRef", sizeof(ta_python_chart_ref), 0,
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_IMMUTABLETYPE | Py_TPFLAGS_DISALLOW_INSTANTIATION, ta_python_chart_slots
};

/* ---- creation ------------------------------------------------------------------------ */

/* The CBOR of a value, in the call's arena. */
static int ta_python_host_cbor(ta_python_host *host, PyObject *value, ta_bytes *out) {
    ta_cbor_writer writer;
    ta_python_cbor_init(&writer, &(ta_python_room){{NULL, 0, 0}, &host->arena_allocator});
    if (ta_python_value_write(host->state, &writer, value) < 0) return -1;
    ta_status status = ta_cbor_writer_bytes(&writer, out);
    if (status != TA_OK) {
        ta_python_raise_status(host->state, status);
        return -1;
    }
    return 0;
}

/* host.config, posted once before the host is handed out (no batch is
 * open: bindings.md, section 3): the effective config as Python values. */
static PyObject *ta_python_host_config(ta_python_host *host) {
    unsigned char room[64];
    ta_cbor_writer writer;
    ta_python_cbor_init(&writer, &(ta_python_room){{room, sizeof room, 0}, &host->arena_allocator});
    ta_cbor_write_map(&writer, 2);
    ta_cbor_write_text(&writer, ta_string("method"));
    ta_cbor_write_text(&writer, ta_string("host.config"));
    ta_cbor_write_text(&writer, ta_string("id"));
    ta_cbor_write_uint(&writer, 0);
    ta_bytes message;
    ta_status status = ta_cbor_writer_bytes(&writer, &message);
    if (status == TA_OK) status = ta_host_post(host->host, message);
    if (status != TA_OK) {
        ta_python_raise_status(host->state, status);
        return NULL;
    }
    const ta_host_effect *effects;
    uint32_t count = ta_host_effects(host->host, &effects);
    PyObject *reply = count ? ta_python_value_read(host->state, ta_host_effect_bytes(host->host, count - 1, TA_EFFECT_FIELD_WIRE))
                            : NULL;
    ta_host_effects_ack(host->host);
    if (!reply) {
        if (!PyErr_Occurred()) ta_python_raise_status(host->state, TA_ERROR_STATE);
        return NULL;
    }
    PyObject *result = NULL;
    if (PyDict_Check(reply)) PyDict_GetItemStringRef(reply, "result", &result);
    Py_DECREF(reply);
    if (!result || !PyDict_Check(result)) {
        Py_XDECREF(result);
        ta_python_raise_status(host->state, TA_ERROR_STATE);
        return NULL;
    }
    return result;
}

/* The I/O processors' handlers by their index in the effective config's
 * ioProcessors: (type, handler or None, is async). */
static int ta_python_host_processors(ta_python_host *host, PyObject *config, PyObject *handlers) {
    PyObject *entries = NULL;
    if (PyDict_GetItemStringRef(config, "ioProcessors", &entries) < 0) return -1;
    host->processors = PyList_New(0);
    if (!host->processors) {
        Py_XDECREF(entries);
        return -1;
    }
    Py_ssize_t found = 0, count = entries && PyList_Check(entries) ? PyList_Size(entries) : 0;
    int status = 0;
    for (Py_ssize_t i = 0; i < count && status == 0; i++) {
        PyObject *entry = PyList_GetItem(entries, i), *type = NULL, *handler = NULL;
        if (PyDict_Check(entry)) PyDict_GetItemStringRef(entry, "type", &type);
        if (type && handlers) status = PyDict_GetItemRef(handlers, type, &handler) < 0 ? -1 : 0;
        if (handler) found++;
        int is_async = 0;
        if (status == 0 && handler) {
            PyObject *answer = PyObject_CallFunctionObjArgs(host->state->coroutine_function, handler, NULL);
            is_async = answer ? PyObject_IsTrue(answer) : -1;
            Py_XDECREF(answer);
            if (is_async < 0) status = -1;
            if (is_async) host->has_async = 1;
        }
        PyObject *row = status == 0 ? Py_BuildValue("(OOO)", type ? type : Py_None, handler ? handler : Py_None,
                                                     is_async ? Py_True : Py_False)
                                    : NULL;
        if (!row || PyList_Append(host->processors, row) < 0) status = -1;
        Py_XDECREF(row);
        Py_XDECREF(type);
        Py_XDECREF(handler);
    }
    Py_XDECREF(entries);
    if (status == 0 && handlers && found != PyDict_Size(handlers)) {
        PyErr_SetString(PyExc_ValueError, "io_processors names a type the config's ioProcessors does not have");
        status = -1;
    }
    return status;
}

/* How a Host is made: Host(...)'s arguments, and for Host.restore the blob
 * of a snapshot (a new host running it instead of an empty one). */
typedef struct ta_python_build {
    PyObject *arguments, *keywords;
    const ta_bytes *pack; /* NULL: Host(...) */
} ta_python_build;

/* A host from a snapshot's pack (ta_host_snapshot_unpack): its images and
 * host blob to ta_host_restore, then the datamodels' state back before any
 * run; *time is the host's time when it was packed. */
typedef struct ta_python_restoring {
    ta_bytes pack;
    const ta_host_environment *environment;
    ta_bytes config;
    ta_time time;
    ta_string_view system_id; /* the snapshot's, in the pack */
} ta_python_restoring;

static ta_status ta_python_host_restore_core(ta_python_host *host, ta_python_restoring *restoring) {
    ta_host_snapshot_contents contents;
    memset(&contents, 0, sizeof contents);
    contents.size = sizeof contents;
    ta_status status = ta_host_snapshot_unpack(restoring->pack, &contents);
    static const char label[] = "tinyactors-cpython";
    if (status == TA_OK && (contents.datamodel.length != sizeof label - 1 ||
                            memcmp(contents.datamodel.pointer, label, sizeof label - 1) != 0))
        status = TA_ERROR_INVALID; /* a pack another program wrote: its datamodels' state is not ours */
    ta_bytes *images = status == TA_OK && contents.image_count
                           ? ta_growable_arena_alloc(&host->arena, sizeof(ta_bytes) * contents.image_count, 8)
                           : NULL;
    if (status == TA_OK && contents.image_count && !images) status = TA_ERROR_NO_MEMORY;
    for (uint32_t i = 0; status == TA_OK && i < contents.image_count; i++)
        images[i] = ta_host_snapshot_image(restoring->pack, i);
    if (status == TA_OK) {
        ta_host_snapshot_input input = {restoring->config, {contents.host, contents.host_length}, images,
                                        contents.image_count, 0};
        status = ta_host_restore(restoring->environment, &input, &host->host);
    }
    if (status != TA_OK) {
        host->host = NULL;
        return status;
    }
    restoring->time = contents.time;
    restoring->system_id = contents.system_id;
    /* the datamodels' handles valid again before the first run */
    PyObject *state = ta_python_value_read(host->state, (ta_bytes){contents.state, contents.state_length});
    PyObject *python = NULL, *ecmascript = NULL;
    int ok = state && PyDict_Check(state) && PyDict_GetItemStringRef(state, "python", &python) > 0 &&
             PyDict_GetItemStringRef(state, "ecmascript", &ecmascript) > 0 && PyBytes_Check(python) &&
             PyBytes_Check(ecmascript);
    if (!ok && !PyErr_Occurred()) PyErr_SetString(PyExc_ValueError, "not a snapshot of a tinyactors.Host");
    if (ok) ok = ta_python_datamodel_restore(&host->python, (ta_bytes){PyBytes_AsString(python),
                                                                         (size_t)PyBytes_Size(python)}) == 0;
    if (ok) {
        ta_status restored = ta_datamodel_ecmascript_restore(host->ecmascript,
                                                             (ta_bytes){PyBytes_AsString(ecmascript),
                                                                        (size_t)PyBytes_Size(ecmascript)});
        if (restored != TA_OK) {
            ta_python_raise_status(host->state, restored);
            ok = 0;
        }
    }
    Py_XDECREF(python);
    Py_XDECREF(ecmascript);
    Py_XDECREF(state);
    return ok ? TA_OK : TA_ERROR_INVALID;
}

static PyObject *ta_python_host_build(PyTypeObject *type, const ta_python_build *build) {
    static char *const names[] = {"config", "io_processors", "log", "clock", "telemetry", "wake", "migrated",
                                  "services", "migration_done", NULL};
    PyObject *config, *handlers = Py_None, *log = Py_None, *clock = Py_None, *telemetry = Py_None;
    PyObject *wake = Py_None, *migrated = Py_None, *services = Py_None, *migration_done = Py_None;
    if (!PyArg_ParseTupleAndKeywords(build->arguments, build->keywords, build->pack ? "O|O$OOOOOOO:restore" : "O|O$OOOOOOO:Host",
                                     names, &config, &handlers, &log, &clock, &telemetry, &wake, &migrated, &services,
                                     &migration_done))
        return NULL;
    ta_time restored_time = 0;
    ta_string_view restored_id = {NULL, 0};
    ta_python_module_state *state = ta_python_state_of_type(type);
    if (!state) return NULL;
    if (services != Py_None && !PyDict_Check(services)) {
        PyErr_SetString(PyExc_TypeError, "services is a dict of an invoke's type -> handler");
        return NULL;
    }
    if (handlers != Py_None && !PyDict_Check(handlers)) {
        PyErr_SetString(PyExc_TypeError, "io_processors is a dict of type -> handler");
        return NULL;
    }
    if (clock != Py_None && !Py_IS_TYPE(clock, (PyTypeObject *)state->clock_type)) {
        PyErr_SetString(PyExc_TypeError, "clock is a tinyactors.SimulatedClock or None");
        return NULL;
    }
    ta_python_host *host = (ta_python_host *)PyType_GenericAlloc(type, 0);
    if (!host) return NULL;
    host->state = state;
    host->thread = PyThread_get_thread_ident();
    host->allocator = ta_python_allocator;
    host->deadline = UINT64_MAX;
    host->log = Py_NewRef(log);
    host->telemetry = Py_NewRef(telemetry);
    host->wake = Py_NewRef(wake);
    host->migrated = Py_NewRef(migrated);
    host->migration_done = Py_NewRef(migration_done);
    host->services = services == Py_None ? PyDict_New() : PyDict_Copy(services);
    host->invocations = PyDict_New();
    if (wake != Py_None) {
        PyObject *answer = PyObject_CallFunctionObjArgs(state->coroutine_function, wake, NULL);
        host->wake_async = answer ? PyObject_IsTrue(answer) : -1;
        Py_XDECREF(answer);
        if (host->wake_async < 0) goto fail;
        if (host->wake_async) host->has_async = 1;
    }
    if (telemetry != Py_None) {
        PyObject *answer = PyObject_CallFunctionObjArgs(state->coroutine_function, telemetry, NULL);
        host->telemetry_async = answer ? PyObject_IsTrue(answer) : -1;
        Py_XDECREF(answer);
        if (host->telemetry_async < 0) goto fail;
        if (host->telemetry_async) host->has_async = 1;
    }
    host->clock = Py_NewRef(clock);
    host->loop = Py_NewRef(Py_None);
    host->timer = Py_NewRef(Py_None);
    host->scheduled = Py_NewRef(Py_None);
    host->waiters = PyList_New(0);
    host->ending_tasks = PyList_New(0);
    ta_growable_arena_config arena = {sizeof arena, 0, 16384, &host->allocator};
    ta_growable_arena_init(&host->arena, &arena);
    host->arena_allocator = ta_growable_arena_allocator(&host->arena);
    ta_table_config pending = {sizeof pending, sizeof(ta_python_pending_row), &host->allocator};
    ta_table_init(&host->pending, &pending);
    if (!host->waiters || !host->ending_tasks || !host->services || !host->invocations ||
        ta_python_datamodel_init(&host->python, host) < 0) goto fail;
    ta_datamodel_ecmascript_config ecmascript;
    memset(&ecmascript, 0, sizeof ecmascript);
    ecmascript.size = sizeof ecmascript;
    host->ecmascript = ta_datamodel_ecmascript_new(&ecmascript);
    if (!host->ecmascript) {
        PyErr_NoMemory();
        goto fail;
    }
    host->datamodels[TA_PYTHON_DATAMODEL_PYTHON] = (ta_datamodel_binding){ta_python_datamodel_vtable(), &host->python};
    host->datamodels[TA_PYTHON_DATAMODEL_ECMASCRIPT] = ta_datamodel_ecmascript_binding(host->ecmascript);

    ta_bytes cbor;
    ta_growable_arena_mark mark = ta_growable_arena_position(&host->arena);
    if (ta_python_host_cbor(host, config, &cbor) < 0) goto fail;
    ta_buffer refusal = {ta_growable_arena_alloc(&host->arena, 8192, 8), 8192, 0};
    ta_host_environment environment = {sizeof environment, TA_PYTHON_DATAMODELS, host->datamodels,
                                       host->allocator, NULL, refusal.pointer ? &refusal : NULL};
    ta_status status;
    if (build->pack) {
        ta_python_restoring restoring = {*build->pack, &environment, cbor, 0, {NULL, 0}};
        status = ta_python_host_restore_core(host, &restoring);
        restored_time = restoring.time;
        restored_id = restoring.system_id;
        if (status != TA_OK && PyErr_Occurred()) { /* the datamodels refused: the host is freed with ours */
            ta_growable_arena_release(&host->arena, mark);
            goto fail;
        }
    } else status = ta_host_create(cbor, &environment, &host->host);
    if (status != TA_OK) {
        host->host = NULL;
        PyObject *reply = status == TA_ERROR_INVALID && refusal.length && refusal.length <= refusal.capacity
                              ? ta_python_value_read(state, (ta_bytes){refusal.pointer, refusal.length})
                              : NULL;
        PyObject *error = NULL;
        if (reply && PyDict_Check(reply)) PyDict_GetItemStringRef(reply, "error", &error);
        PyObject *exception = error ? ta_python_error_from_reply(state, error) : NULL;
        if (exception) PyErr_SetObject(state->error, exception);
        else if (!PyErr_Occurred()) ta_python_raise_status(state, status);
        Py_XDECREF(exception);
        Py_XDECREF(error);
        Py_XDECREF(reply);
        ta_growable_arena_release(&host->arena, mark);
        goto fail;
    }
    PyObject *effective = ta_python_host_config(host);
    ta_growable_arena_release(&host->arena, mark);
    if (!effective) goto fail;
    int same_system = 1; /* a restore: the effective config's system id is the snapshot's */
    if (build->pack) {
        PyObject *id = NULL;
        Py_ssize_t length = 0;
        const char *text = PyDict_GetItemStringRef(effective, "systemID", &id) > 0 && PyUnicode_Check(id)
                               ? PyUnicode_AsUTF8AndSize(id, &length)
                               : NULL;
        same_system = text && (size_t)length == restored_id.length && memcmp(text, restored_id.pointer, restored_id.length) == 0;
        Py_XDECREF(id);
        PyErr_Clear();
    }
    PyObject *system = NULL, *rate = NULL;
    if (PyDict_GetItemStringRef(effective, "system", &system) > 0 && PyDict_Check(system))
        PyDict_GetItemStringRef(system, "ticksPerSecond", &rate);
    host->ticks_per_second = rate ? PyLong_AsUnsignedLongLong(rate) : 0;
    PyObject *telemetry_key = PyUnicode_FromString("telemetry");
    host->encoder = telemetry_key && PyDict_Contains(effective, telemetry_key) == 1;
    Py_XDECREF(telemetry_key);
    Py_XDECREF(system);
    Py_XDECREF(rate);
    int processors = PyErr_Occurred() || !host->ticks_per_second ? -1
                                                                  : ta_python_host_processors(host, effective,
                                                                                              handlers == Py_None ? NULL : handlers);
    Py_DECREF(effective);
    if (processors < 0) {
        if (!PyErr_Occurred()) ta_python_raise_status(state, TA_ERROR_STATE);
        goto fail;
    }
    if (clock != Py_None && ta_python_clock_register(clock, (PyObject *)host) < 0) goto fail;
    if (build->pack) {
        /* the config the snapshot was taken with: its system id at least (its I/O processors and
           tick rate come from this config too, and are not in the pack) */
        if (!same_system) {
            PyErr_Format(PyExc_ValueError, "the snapshot is of system %.*s: restore it with the config it was taken with",
                         (int)restored_id.length, restored_id.pointer ? restored_id.pointer : "");
            goto fail;
        }
        ta_python_clock_resume(host, restored_time); /* on from the snapshot's time */
    }
    ta_python_advance(host); /* the host's clock starts now, not at 0 */
    return (PyObject *)host;
fail:
    Py_DECREF(host);
    return NULL;
}

/* ---- close ----------------------------------------------------------------------- */

/* Takes, and batches, the last take makes at most. */
#define TA_PYTHON_END_TAKES 64u

/* trace.end, then the last take of traces and logs, before ta_host_free
 * (bindings.md, section 4): the spans still open are ended and go to the
 * telemetry handler with the rest (a handler's exception is kept). */
static void ta_python_host_end_traces(ta_python_host *host) {
    unsigned char room[64];
    ta_cbor_writer writer;
    ta_python_cbor_init(&writer, &(ta_python_room){{room, sizeof room, 0}, NULL});
    ta_cbor_write_map(&writer, 1);
    ta_cbor_write_text(&writer, ta_string("method"));
    ta_cbor_write_text(&writer, ta_string("trace.end"));
    ta_bytes message;
    /* a call on the stack, as a drain is: a handler's close is deferred,
     * its metrics() joins the open batch */
    host->depth++;
    host->ending = 1;
    if (ta_cbor_writer_bytes(&writer, &message) == TA_OK && ta_host_post(host->host, message) == TA_OK) {
        const ta_host_effect *effects;
        /* until a take writes nothing (bindings.md section 4), bounded: a
         * handler that makes inputs here (a spawn) makes more records */
        for (uint32_t batches = 0; batches < TA_PYTHON_END_TAKES && ta_host_effects(host->host, &effects) > 0; batches++) {
            host->batch = 1;
            ta_bytes taken;
            for (uint32_t takes = 0; takes < TA_PYTHON_END_TAKES && ta_host_traces(host->host, &taken) == TA_OK && taken.length; takes++)
                ta_python_telemetry(host, &(ta_python_export){"traces", taken});
            for (uint32_t takes = 0; takes < TA_PYTHON_END_TAKES && ta_host_logs(host->host, &taken) == TA_OK && taken.length; takes++)
                ta_python_telemetry(host, &(ta_python_export){"logs", taken});
            ta_host_effects_ack(host->host);
            host->batch = 0;
            host->acks++;
        }
    }
    host->ending = 0;
    host->depth--;
    host->closing = 0; /* a close asked by a handler here: this one */
    ta_cbor_writer_free(&writer);
}

static void ta_python_host_free(ta_python_host *host) {
    if (host->host) ta_host_free(host->host);
    host->host = NULL;
    ta_python_pending_drop(host);
    ta_table_free(&host->pending);
    if (host->invocations) PyDict_Clear(host->invocations); /* the services are dropped, not stopped */
    ta_python_datamodel_free(&host->python);
    if (host->ecmascript) ta_datamodel_ecmascript_free(host->ecmascript); /* after the host that used it */
    host->ecmascript = NULL;
    ta_growable_arena_free(&host->arena);
}

void ta_python_host_close(ta_python_host *host, int quiet) {
    if (!host->host) return;
    if (host->depth > 0 || host->turning) { /* from a handler or chart code: after the loop returns */
        host->closing = 1;
        return;
    }
    host->closing = 0;
    ta_python_loop_stop(host);
    if (!quiet && host->telemetry != Py_None) ta_python_host_end_traces(host);
    ta_python_host_free(host);
    if (host->clock && host->clock != Py_None) ta_python_clock_forget(host->clock, (PyObject *)host);
}

static int ta_python_host_closed_error(ta_python_host *host);

static PyObject *ta_python_host_close_method(PyObject *self, PyObject *unused) {
    (void)unused;
    ta_python_host *host = (ta_python_host *)self;
    if (host->host && PyThread_get_thread_ident() != host->thread) {
        PyErr_SetString(host->state->error, "a Host is called only from the thread that made it");
        return NULL;
    }
    ta_python_host_close(host, 0);
    if (ta_python_host_closed_error(host) < 0) return NULL; /* a telemetry handler raised in the last take */
    Py_RETURN_NONE;
}

static int ta_python_host_traverse(PyObject *self, visitproc visit, void *arg) {
    ta_python_host *host = (ta_python_host *)self;
    Py_VISIT(Py_TYPE(self));
    Py_VISIT(host->processors);
    Py_VISIT(host->log);
    Py_VISIT(host->telemetry);
    Py_VISIT(host->wake);
    Py_VISIT(host->migrated);
    Py_VISIT(host->migration_done);
    Py_VISIT(host->services);
    Py_VISIT(host->invocations);
    Py_VISIT(host->clock);
    Py_VISIT(host->loop);
    Py_VISIT(host->timer);
    Py_VISIT(host->scheduled);
    Py_VISIT(host->waiters);
    Py_VISIT(host->ending_tasks);
    Py_VISIT(host->error);
    for (uint32_t slot = 0; slot < host->pending.capacity; slot++) {
        ta_python_pending_row *row = ta_table_at(&host->pending, slot);
        if (!row) continue;
        Py_VISIT(row->handler);
        Py_VISIT(row->on_error);
    }
    return ta_python_datamodel_traverse(&host->python, visit, arg);
}

/* The collector's path: free the host without calling handlers, then drop
 * every reference (handlers close over the host: AGENTS.md, "Rules the
 * suite cannot see"). */
static int ta_python_host_clear(PyObject *self) {
    ta_python_host *host = (ta_python_host *)self;
    if (host->host && host->depth == 0 && !host->turning) ta_python_host_close(host, 1);
    Py_CLEAR(host->processors);
    Py_CLEAR(host->log);
    Py_CLEAR(host->telemetry);
    Py_CLEAR(host->wake);
    Py_CLEAR(host->migrated);
    Py_CLEAR(host->migration_done);
    Py_CLEAR(host->services);
    Py_CLEAR(host->invocations);
    Py_CLEAR(host->clock);
    Py_CLEAR(host->loop);
    Py_CLEAR(host->timer);
    Py_CLEAR(host->scheduled);
    Py_CLEAR(host->waiters);
    Py_CLEAR(host->ending_tasks);
    Py_CLEAR(host->error);
    return 0;
}

static void ta_python_host_dealloc(PyObject *self) {
    PyTypeObject *type = Py_TYPE(self);
    ta_python_host *host = (ta_python_host *)self;
    PyObject_GC_UnTrack(self);
    PyObject_ClearWeakRefs(self);
    host->depth = 0;
    host->turning = 0;
    if (host->host) ta_python_host_close(host, 1);
    else if (host->python.state) ta_python_host_free(host); /* a creation that failed half way */
    ta_python_host_clear(self);
    freefunc ta_python_release = (freefunc)PyType_GetSlot(type, Py_tp_free);
    ta_python_release(self);
    Py_DECREF(type);
}

/* ---- inputs ----------------------------------------------------------------------- */

static PyObject *ta_python_host_status(ta_python_host *host, ta_status status) {
    if (status == TA_OK) Py_RETURN_NONE;
    ta_python_raise_status(host->state, status);
    return NULL;
}

static PyObject *ta_python_host_add_image(PyObject *self, PyObject *image) {
    ta_python_host *host = (ta_python_host *)self;
    ta_python_call call;
    if (ta_python_enter(host, &call) < 0) return NULL;
    Py_buffer view;
    PyObject *result = NULL;
    if (PyObject_GetBuffer(image, &view, PyBUF_SIMPLE) == 0) {
        uint32_t segment = 0; /* the host copies the bytes during the call */
        ta_status status = ta_host_add_image(host->host, (ta_bytes){view.buf, (size_t)view.len}, &segment);
        PyBuffer_Release(&view);
        result = status == TA_OK ? PyLong_FromUnsignedLong(segment) : ta_python_host_status(host, status);
    }
    ta_python_leave(&call);
    return result;
}

static PyObject *ta_python_host_chart(PyObject *self, PyObject *name) {
    ta_python_host *host = (ta_python_host *)self;
    ta_python_call call;
    if (ta_python_enter(host, &call) < 0) return NULL;
    PyObject *result = NULL;
    ta_string_view text;
    ta_host_chart_ref chart;
    if (PyUnicode_Check(name) && ta_python_text(name, &text) == 0) {
        ta_status status = ta_host_chart(host->host, text, &chart);
        if (status != TA_OK) ta_python_raise_status(host->state, status);
        else {
            ta_python_chart_ref *ref = (ta_python_chart_ref *)PyType_GenericAlloc((PyTypeObject *)host->state->chart_type, 0);
            if (ref) {
                ref->segment = chart.segment;
                ref->chart = chart.chart;
            }
            result = (PyObject *)ref;
        }
    } else if (!PyErr_Occurred()) PyErr_SetString(PyExc_TypeError, "chart takes the chart's name, a str");
    ta_python_leave(&call);
    return result;
}

/* actor_by_name(name) -> session or None: ta_host_actor_by_name, the live
 * actor spawned with `name` (boot actors carry their manifest name), newest
 * segment first. */
static PyObject *ta_python_host_actor_by_name(PyObject *self, PyObject *name) {
    ta_python_host *host = (ta_python_host *)self;
    ta_python_call call;
    if (ta_python_enter(host, &call) < 0) return NULL;
    PyObject *result = NULL;
    ta_string_view text;
    if (PyUnicode_Check(name) && ta_python_text(name, &text) == 0) {
        uint64_t session = ta_host_actor_by_name(host->host, text);
        result = session ? PyLong_FromUnsignedLongLong(session) : Py_NewRef(Py_None);
    } else if (!PyErr_Occurred()) PyErr_SetString(PyExc_TypeError, "actor_by_name takes the actor's name, a str");
    ta_python_leave(&call);
    return result;
}

static int ta_python_session(PyObject *value, uint64_t *out) {
    *out = PyLong_Check(value) ? PyLong_AsUnsignedLongLong(value) : 0;
    if (PyErr_Occurred()) return -1;
    if (!PyLong_Check(value)) {
        PyErr_SetString(PyExc_TypeError, "a session is an int");
        return -1;
    }
    return 0;
}

static PyObject *ta_python_host_spawn(PyObject *self, PyObject *arguments, PyObject *keywords) {
    static char *const names[] = {"ref", "name", "params", "traceparent", NULL};
    ta_python_host *host = (ta_python_host *)self;
    PyObject *ref, *name = Py_None, *params = Py_None, *traceparent = Py_None;
    if (!PyArg_ParseTupleAndKeywords(arguments, keywords, "O|OOO:spawn", names, &ref, &name, &params, &traceparent))
        return NULL;
    if (!Py_IS_TYPE(ref, (PyTypeObject *)host->state->chart_type)) {
        PyErr_SetString(PyExc_TypeError, "spawn takes a ChartRef from Host.chart");
        return NULL;
    }
    ta_python_call call;
    if (ta_python_enter(host, &call) < 0) return NULL;
    PyObject *result = NULL;
    ta_spawn spawn;
    memset(&spawn, 0, sizeof spawn);
    spawn.segment = ((ta_python_chart_ref *)ref)->segment;
    spawn.chart = ((ta_python_chart_ref *)ref)->chart;
    ta_python_payload_for payload = {0, {spawn.segment, spawn.chart}, params, 0, TA_NIL};
    if (ta_python_text(name, &spawn.name) == 0 && ta_python_text(traceparent, &spawn.traceparent) == 0 &&
        ta_python_host_payload(host, &payload) == 0) {
        uint64_t session = 0;
        spawn.params = payload.payload;
        ta_python_advance(host);
        ta_status status = ta_host_spawn(host->host, &spawn, &session);
        if (status != TA_OK) ta_python_host_payload_release(host, payload.payload, payload.datamodel);
        result = status == TA_OK ? PyLong_FromUnsignedLongLong(session) : ta_python_host_status(host, status);
        if (status == TA_OK) ta_python_loop_schedule(host);
    }
    ta_python_leave(&call);
    return result;
}

/* An event of send or send_many: fills `event` and its payload. */
typedef struct ta_python_send_item {
    PyObject *target, *name, *data, *traceparent, *origin;
    uint32_t datamodel; /* out: the payload's */
} ta_python_send_item;

/* A send's target: a session, or a local address core resolves to one
 * (ta_host_resolve), before the payload is made in that session's
 * datamodel. */
static int ta_python_target(ta_python_host *host, PyObject *target, uint64_t *session) {
    if (!PyUnicode_Check(target)) return ta_python_session(target, session);
    ta_string_view address;
    if (ta_python_text(target, &address) < 0) return -1;
    ta_status status = ta_host_resolve(host->host, address, session);
    if (status != TA_OK) {
        ta_python_raise_status(host->state, status);
        return -1;
    }
    return 0;
}

static int ta_python_event(ta_python_host *host, ta_python_send_item *item, ta_event_in *event) {
    memset(event, 0, sizeof *event);
    event->size = sizeof *event;
    item->datamodel = TA_NIL;
    if (ta_python_target(host, item->target, &event->session) < 0 ||
        ta_python_text(item->traceparent, &event->traceparent) < 0 || ta_python_text(item->origin, &event->origin) < 0)
        return -1;
    if (!PyUnicode_Check(item->name)) {
        PyErr_SetString(PyExc_TypeError, "an event's name is a str");
        return -1;
    }
    if (ta_python_text(item->name, &event->name) < 0) return -1;
    ta_python_payload_for payload = {event->session, {0, 0}, item->data, 0, TA_NIL};
    if (ta_python_host_payload(host, &payload) < 0) return -1;
    event->data = payload.payload;
    item->datamodel = payload.datamodel;
    return 0;
}

static PyObject *ta_python_host_send(PyObject *self, PyObject *arguments, PyObject *keywords) {
    static char *const names[] = {"target", "event", "data", "traceparent", "origin", NULL};
    ta_python_host *host = (ta_python_host *)self;
    ta_python_send_item item = {NULL, NULL, Py_None, Py_None, Py_None, TA_NIL};
    if (!PyArg_ParseTupleAndKeywords(arguments, keywords, "OO|O$OO:send", names, &item.target, &item.name, &item.data,
                                     &item.traceparent, &item.origin))
        return NULL;
    ta_python_call call;
    if (ta_python_enter(host, &call) < 0) return NULL;
    PyObject *result = NULL;
    ta_event_in event;
    if (ta_python_event(host, &item, &event) == 0) {
        ta_status status = ta_host_send(host->host, &event);
        if (status != TA_OK) ta_python_host_payload_release(host, event.data, item.datamodel);
        else ta_python_loop_schedule(host);
        result = ta_python_host_status(host, status);
    }
    ta_python_leave(&call);
    return result;
}

/* Events a send_many converts and hands core at once: the arena holds a
 * slice, not the whole list, so one large call leaves no large chunk
 * kept for the host's life. */
#define TA_PYTHON_SLICE 256

/* One slice of send_many, events [first, first + count) of `list`: the
 * accepted prefix of the slice, or -1 with an exception (nothing of the
 * slice taken). */
typedef struct ta_python_slice {
    PyObject *list;
    Py_ssize_t first, count;
} ta_python_slice;

static Py_ssize_t ta_python_send_slice(ta_python_host *host, const ta_python_slice *slice) {
    ta_growable_arena_mark mark = ta_growable_arena_position(&host->arena);
    ta_event_in *events = ta_growable_arena_alloc(&host->arena, sizeof(ta_event_in) * TA_PYTHON_SLICE, 8);
    uint32_t *datamodels = ta_growable_arena_alloc(&host->arena, sizeof(uint32_t) * TA_PYTHON_SLICE, 4);
    Py_ssize_t made = 0, taken = -1;
    if (!events || !datamodels) PyErr_NoMemory();
    else {
        for (; made < slice->count; made++) {
            PyObject *entry = PyList_GetItem(slice->list, slice->first + made);
            Py_ssize_t size = PyTuple_Check(entry) ? PyTuple_Size(entry) : 0;
            if (size < 2 || size > 4) {
                PyErr_SetString(PyExc_TypeError, "a send_many item is (target, event[, data[, traceparent]])");
                break;
            }
            ta_python_send_item item = {PyTuple_GetItem(entry, 0), PyTuple_GetItem(entry, 1),
                                        size > 2 ? PyTuple_GetItem(entry, 2) : Py_None,
                                        size > 3 ? PyTuple_GetItem(entry, 3) : Py_None, Py_None, TA_NIL};
            if (ta_python_event(host, &item, &events[made]) < 0) break;
            datamodels[made] = item.datamodel;
        }
        if (made == slice->count) taken = ta_host_send_many(host->host, events, (uint32_t)made);
        for (Py_ssize_t i = taken < 0 ? 0 : taken; i < made; i++)
            ta_python_host_payload_release(host, events[i].data, datamodels[i]); /* still ours */
    }
    ta_growable_arena_release(&host->arena, mark);
    return taken;
}

/* send_many(items): each item (session, event[, data[, traceparent]]),
 * handed to core in slices (ta_host_send_many). Returns the length of the
 * accepted prefix; the payloads after it are released (ours). An item
 * that cannot be converted raises, and the slices before its own were
 * sent. */
static PyObject *ta_python_host_send_many(PyObject *self, PyObject *items) {
    ta_python_host *host = (ta_python_host *)self;
    ta_python_call call;
    if (ta_python_enter(host, &call) < 0) return NULL;
    PyObject *list = PySequence_List(items), *result = NULL;
    Py_ssize_t count = list ? PyList_Size(list) : 0, accepted = 0;
    int failed = list ? 0 : 1;
    for (Py_ssize_t first = 0; !failed && first < count; first += TA_PYTHON_SLICE) {
        ta_python_slice slice = {list, first, count - first < TA_PYTHON_SLICE ? count - first : TA_PYTHON_SLICE};
        Py_ssize_t taken = ta_python_send_slice(host, &slice);
        if (taken < 0) failed = 1;
        else accepted += taken;
        if (taken != slice.count) break; /* refused at an event: the rest is not sent */
    }
    if (accepted) ta_python_loop_schedule(host);
    if (!failed) result = PyLong_FromSsize_t(accepted);
    Py_XDECREF(list); /* after the calls: the views pointed into its items */
    ta_python_leave(&call);
    return result;
}

static PyObject *ta_python_host_terminate(PyObject *self, PyObject *value) {
    ta_python_host *host = (ta_python_host *)self;
    uint64_t session;
    if (ta_python_session(value, &session) < 0) return NULL;
    ta_python_call call;
    if (ta_python_enter(host, &call) < 0) return NULL;
    ta_status status = ta_host_terminate(host->host, session);
    if (status == TA_OK) ta_python_loop_schedule(host);
    PyObject *result = ta_python_host_status(host, status);
    ta_python_leave(&call);
    return result;
}

/* The segment's boot actors (ta_host_boot): the image's <system><actor>s,
 * or its default boot chart, each spawned under its name. */
static PyObject *ta_python_host_boot(PyObject *self, PyObject *value) {
    ta_python_host *host = (ta_python_host *)self;
    unsigned long segment = PyLong_Check(value) ? PyLong_AsUnsignedLong(value) : (unsigned long)-1;
    if (segment > UINT32_MAX) {
        if (!PyErr_Occurred()) PyErr_SetString(PyExc_TypeError, "boot requires a segment (an int add_image returned)");
        else if (PyErr_ExceptionMatches(PyExc_OverflowError)) {
            PyErr_Clear();
            PyErr_SetString(PyExc_ValueError, "boot requires a segment (an int add_image returned)");
        }
        return NULL;
    }
    ta_python_call call;
    if (ta_python_enter(host, &call) < 0) return NULL;
    ta_python_advance(host);
    ta_status status = ta_host_boot(host->host, (uint32_t)segment);
    if (status == TA_OK) ta_python_loop_schedule(host);
    PyObject *result = ta_python_host_status(host, status);
    ta_python_leave(&call);
    return result;
}

/* metrics() -> bytes: the host's counters as an OTLP metrics request
 * (ta_host_metrics), pulled when the application wants them: the host's
 * view copied once. Taken outside a drain it opened a batch of its own,
 * acknowledged here; from a handler it joins the open batch. */
static PyObject *ta_python_host_output(ta_python_host *host, ta_bytes taken);

static PyObject *ta_python_host_metrics(PyObject *self, PyObject *unused) {
    (void)unused;
    ta_python_host *host = (ta_python_host *)self;
    ta_python_call call;
    if (ta_python_enter(host, &call) < 0) return NULL;
    ta_bytes taken;
    ta_status status = ta_host_metrics(host->host, &taken);
    PyObject *result = status == TA_OK ? ta_python_host_output(host, taken) : NULL;
    if (status != TA_OK) ta_python_raise_status(host->state, status); /* TA_ERROR_STATE: no telemetry in the config */
    ta_python_leave(&call);
    return result;
}

/* ---- hibernation: the application keeps the blobs ---------------------------------- */

/* sweep(max_scan, limit) -> [session, ...]: the CLOCK sweep (ta_host_sweep),
 * at most `limit` sessions that did not run since the previous sweep and
 * can be hibernated now, scanning at most `max_scan` slots. */
static PyObject *ta_python_host_sweep(PyObject *self, PyObject *arguments) {
    ta_python_host *host = (ta_python_host *)self;
    PyObject *scan_object, *limit_object;
    if (!PyArg_ParseTuple(arguments, "O!O!:sweep", &PyLong_Type, &scan_object, &PyLong_Type, &limit_object)) return NULL;
    unsigned long scan = PyLong_AsUnsignedLong(scan_object), limit = scan == (unsigned long)-1 && PyErr_Occurred()
                                                                         ? 0 : PyLong_AsUnsignedLong(limit_object);
    if (PyErr_Occurred()) return NULL; /* negative: OverflowError */
    if (scan > UINT32_MAX || limit > UINT32_MAX) {
        PyErr_SetString(PyExc_OverflowError, "sweep's max_scan and limit are 32-bit counts");
        return NULL;
    }
    ta_python_call call;
    if (ta_python_enter(host, &call) < 0) return NULL;
    uint64_t *sessions = limit ? ta_growable_arena_alloc(&host->arena, sizeof(uint64_t) * limit, 8) : NULL;
    PyObject *result = NULL;
    if (limit && !sessions) PyErr_NoMemory();
    else {
        uint32_t found = ta_host_sweep(host->host, (uint32_t)scan, (ta_uint64_array){sessions, (uint32_t)limit});
        result = PyList_New(found);
        for (uint32_t i = 0; result && i < found; i++) {
            PyObject *session = PyLong_FromUnsignedLongLong(sessions[i]);
            if (!session) Py_CLEAR(result);
            else PyList_SetItem(result, i, session);
        }
    }
    ta_python_leave(&call);
    return result;
}

/* An output was taken (a hibernation blob, metrics): with no batch open it
 * opened a batch of this output alone, acknowledged here; with one open
 * (a handler's call) it joined that batch, which the drain acks. */
static void ta_python_host_output_done(ta_python_host *host) {
    if (host->batch) return;
    ta_host_effects_ack(host->host);
    host->acks++;
}

/* The output copied once into bytes, then done. */
static PyObject *ta_python_host_output(ta_python_host *host, ta_bytes taken) {
    PyObject *result = PyBytes_FromStringAndSize(taken.pointer, (Py_ssize_t)taken.length);
    ta_python_host_output_done(host);
    return result;
}

/* hibernate(session) -> bytes: ta_host_hibernate; the blob is the
 * application's to store until it gives it to wake. */
static PyObject *ta_python_host_hibernate(PyObject *self, PyObject *value) {
    ta_python_host *host = (ta_python_host *)self;
    uint64_t session = PyLong_Check(value) ? PyLong_AsUnsignedLongLong(value) : 0;
    if (PyErr_Occurred() || !PyLong_Check(value)) {
        if (!PyErr_Occurred()) PyErr_SetString(PyExc_TypeError, "a session is an int");
        return NULL;
    }
    ta_python_call call;
    if (ta_python_enter(host, &call) < 0) return NULL;
    ta_bytes taken;
    ta_status status = ta_host_hibernate(host->host, session, &taken);
    PyObject *result = NULL;
    if (status == TA_OK) {
        result = PyBytes_FromStringAndSize(taken.pointer, (Py_ssize_t)taken.length);
        if (!result) ta_host_wake(host->host, session, taken); /* the copy failed: back from the view, still
                                                                  valid, or the actor would have no blob */
        ta_python_host_output_done(host);
    } else ta_python_raise_status(host->state, status); /* TA_ERROR_STATE: not at a stable point */
    ta_python_leave(&call);
    return result;
}

/* wake(session, blob): ta_host_wake, with the blob hibernate gave. */
static PyObject *ta_python_host_wake(PyObject *self, PyObject *arguments) {
    ta_python_host *host = (ta_python_host *)self;
    PyObject *session_object;
    Py_buffer view;
    if (!PyArg_ParseTuple(arguments, "O!y*:wake", &PyLong_Type, &session_object, &view)) return NULL;
    uint64_t session = PyLong_AsUnsignedLongLong(session_object);
    if (PyErr_Occurred()) { /* negative or wider than 64 bits: OverflowError */
        PyBuffer_Release(&view);
        return NULL;
    }
    ta_python_call call;
    PyObject *result = NULL;
    if (ta_python_enter(host, &call) == 0) {
        ta_status status = ta_host_wake(host->host, session, (ta_bytes){view.buf, (size_t)view.len});
        if (status == TA_OK) {
            ta_python_loop_schedule(host); /* its mailbox runs at the next turn */
            result = Py_NewRef(Py_None);
        } else ta_python_raise_status(host->state, status);
        ta_python_leave(&call);
    }
    PyBuffer_Release(&view);
    return result;
}

/* ---- migration: actors move to the current version of their chart ----------------- */

/* migrate(*, chart=None, session=None, wake=False, timeout=0) -> migration:
 * ta_host_migrate of every chart (or `chart`, or one `session`), waking
 * hibernated actors for it with `wake` (TA_HOST_MIGRATE_WAKE: a WAKE each),
 * ending after `timeout` seconds of the host's clock, as ticks rounded up (0: none). The migration moves
 * actors in later runs; the host's migration_done handler gets its MIGRATION_DONE. */
static PyObject *ta_python_host_migrate(PyObject *self, PyObject *arguments, PyObject *keywords) {
    static char *const names[] = {"chart", "session", "wake", "timeout", NULL};
    ta_python_host *host = (ta_python_host *)self;
    PyObject *chart = Py_None, *session = Py_None, *timeout = NULL;
    int wake = 0;
    if (!PyArg_ParseTupleAndKeywords(arguments, keywords, "|$OOpO:migrate", names, &chart, &session, &wake,
                                     &timeout))
        return NULL;
    ta_host_migration request;
    memset(&request, 0, sizeof request);
    request.flags = wake ? TA_HOST_MIGRATE_WAKE : 0;
    if (chart != Py_None) {
        Py_ssize_t length = 0;
        const char *name = PyUnicode_Check(chart) ? PyUnicode_AsUTF8AndSize(chart, &length) : NULL;
        if (!name) {
            if (!PyErr_Occurred()) PyErr_SetString(PyExc_TypeError, "chart is a chart's name (str) or None");
            return NULL;
        }
        if (!length) { /* core reads an empty name as every chart */
            PyErr_SetString(PyExc_ValueError, "chart is a chart's name; None for every chart");
            return NULL;
        }
        request.name = (ta_string_view){name, (size_t)length};
    }
    if (session != Py_None) {
        request.session = PyLong_Check(session) ? PyLong_AsUnsignedLongLong(session) : 0;
        if (!PyErr_Occurred() && !request.session) PyErr_SetString(PyExc_ValueError, "session is a session id (int) or None");
        if (PyErr_Occurred()) return NULL;
    }
    if (timeout) {
        uint64_t nanoseconds = 0;
        if (ta_python_nanoseconds(timeout, &nanoseconds) < 0) return NULL;
        request.timeout = ta_python_ticks_after(host, nanoseconds);
    }
    ta_python_call call;
    if (ta_python_enter(host, &call) < 0) return NULL;
    ta_python_advance(host); /* the timeout counts from the host's clock now */
    uint32_t migration = 0;
    ta_status status = ta_host_migrate(host->host, &request, &migration);
    PyObject *result = NULL;
    if (status != TA_OK) ta_python_raise_status(host->state, status);
    else {
        /* the migration runs from here: should Python refuse memory now, the caller gets the exception
           and no id (the migration still moves actors, and migration_done still gets its end) */
        result = PyLong_FromUnsignedLong(migration);
        ta_python_loop_schedule(host); /* the migration moves actors in the next runs */
    }
    ta_python_leave(&call);
    return result;
}

static PyObject *ta_python_host_create(PyTypeObject *type, PyObject *arguments, PyObject *keywords) {
    return ta_python_host_build(type, &(ta_python_build){arguments, keywords, NULL});
}

/* ---- snapshots: the whole host to bytes and back ------------------------------------ */

/* restore(snapshot, config, io_processors=None, *, log=None, ...) -> Host:
 * a new host running a snapshot's pack (ta_host_restore, then both
 * datamodels' state), with the handlers Host(...) takes; the config is
 * given again, as for ta_host_create. */
static PyObject *ta_python_host_restore(PyObject *type, PyObject *arguments, PyObject *keywords) {
    if (PyTuple_Size(arguments) < 1) {
        PyErr_SetString(PyExc_TypeError, "restore() takes the snapshot's bytes first");
        return NULL;
    }
    PyObject *blob = PyTuple_GetItem(arguments, 0), *aligned = NULL, *rest = PyTuple_GetSlice(arguments, 1, PyTuple_Size(arguments));
    Py_buffer view;
    if (!rest || PyObject_GetBuffer(blob, &view, PyBUF_SIMPLE) < 0) {
        Py_XDECREF(rest);
        return NULL;
    }
    ta_bytes pack = {view.buf, (size_t)view.len};
    if ((uintptr_t)view.buf & 7u) { /* the pack's parts are 8-aligned when the pack is */
        aligned = PyBytes_FromStringAndSize(view.buf, view.len);
        if (aligned) pack = (ta_bytes){PyBytes_AsString(aligned), (size_t)view.len};
    }
    PyObject *host = !((uintptr_t)view.buf & 7u) || aligned
                         ? ta_python_host_build((PyTypeObject *)type, &(ta_python_build){rest, keywords, &pack})
                         : NULL;
    PyBuffer_Release(&view);
    Py_XDECREF(aligned);
    Py_DECREF(rest);
    return host;
}

/* The ECMAScript datamodel's state (ta_datamodel_ecmascript_snapshot) as
 * bytes: written into room for the last state's length and a quarter more,
 * once more only when it outgrew that (each call writes the whole state). */
static PyObject *ta_python_ecmascript_state(ta_python_host *host) {
    size_t room = host->ecmascript_room + host->ecmascript_room / 4 + 4096;
    PyObject *bytes = PyBytes_FromStringAndSize(NULL, (Py_ssize_t)room);
    ta_buffer out = {bytes ? PyBytes_AsString(bytes) : NULL, room, 0};
    ta_status status = bytes ? ta_datamodel_ecmascript_snapshot(host->ecmascript, &out) : TA_OK;
    if (bytes && status == TA_ERROR_NO_MEMORY && out.length > room) { /* it outgrew the guess */
        Py_DECREF(bytes);
        room = out.length;
        bytes = PyBytes_FromStringAndSize(NULL, (Py_ssize_t)room);
        out = (ta_buffer){bytes ? PyBytes_AsString(bytes) : NULL, room, 0};
        status = bytes ? ta_datamodel_ecmascript_snapshot(host->ecmascript, &out) : TA_OK;
    }
    if (bytes && status != TA_OK) {
        Py_CLEAR(bytes);
        ta_python_raise_status(host->state, status); /* TA_ERROR_UNSUPPORTED: a value it cannot store */
    }
    if (!bytes) return NULL;
    host->ecmascript_room = out.length;
    PyObject *exact = PyBytes_FromStringAndSize(PyBytes_AsString(bytes), (Py_ssize_t)out.length);
    Py_DECREF(bytes);
    return exact;
}

/* One value as CBOR bytes, through the host's allocator and freed here. */
static PyObject *ta_python_host_encode(ta_python_host *host, PyObject *value) {
    ta_cbor_writer writer;
    ta_python_cbor_init(&writer, &(ta_python_room){{NULL, 0, 0}, NULL});
    PyObject *result = NULL;
    if (ta_python_value_write(host->state, &writer, value) == 0) {
        ta_bytes bytes;
        if (ta_cbor_writer_bytes(&writer, &bytes) == TA_OK)
            result = PyBytes_FromStringAndSize(bytes.pointer, (Py_ssize_t)bytes.length);
        else PyErr_NoMemory();
    }
    ta_cbor_writer_free(&writer);
    return result;
}

/* snapshot() -> bytes: ta_host_snapshot_pack of the whole host, with both
 * datamodels' state, the images, the system id and the host's time. A
 * quiescent host only: not from a handler (a batch is open) nor while a
 * migration runs (TA_ERROR_STATE). The services running are not in
 * it: the restored host's first run announces each invocation again. */
static PyObject *ta_python_host_snapshot(PyObject *self, PyObject *unused) {
    (void)unused;
    ta_python_host *host = (ta_python_host *)self;
    ta_python_call call;
    if (ta_python_enter(host, &call) < 0) return NULL;
    /* quiescent first (effects or replies pending, a migration): core's own check, before the
       datamodels' state is built for nothing */
    ta_buffer quiet = {NULL, 0, 0};
    ta_status ready = ta_host_snapshot(host->host, &quiet);
    if (ready != TA_OK && ready != TA_ERROR_NO_MEMORY) ta_python_raise_status(host->state, ready);
    PyObject *python = ready == TA_OK || ready == TA_ERROR_NO_MEMORY ? ta_python_datamodel_snapshot(&host->python) : NULL;
    PyObject *ecmascript = python ? ta_python_ecmascript_state(host) : NULL;
    PyObject *both = ecmascript ? Py_BuildValue("{s:O,s:O}", "python", python, "ecmascript", ecmascript) : NULL;
    Py_XDECREF(python);
    Py_XDECREF(ecmascript);
    PyObject *encoded = both ? ta_python_host_encode(host, both) : NULL;
    Py_XDECREF(both);
    PyObject *result = NULL;
    if (encoded) {
        static const char label[] = "tinyactors-cpython";
        ta_datamodel_state state = {{label, sizeof label - 1}, {PyBytes_AsString(encoded), (size_t)PyBytes_Size(encoded)}};
        ta_buffer probe = {NULL, 0, 0};
        ta_status status = ta_host_snapshot_pack(host->host, &state, &probe);
        if (status == TA_ERROR_NO_MEMORY) {
            result = PyBytes_FromStringAndSize(NULL, (Py_ssize_t)probe.length);
            ta_buffer out = {result ? PyBytes_AsString(result) : NULL, probe.length, 0};
            status = result ? ta_host_snapshot_pack(host->host, &state, &out) : TA_OK;
            if (status != TA_OK) Py_CLEAR(result);
        }
        if (status != TA_OK) ta_python_raise_status(host->state, status); /* TA_ERROR_STATE: not quiescent */
        Py_DECREF(encoded);
    }
    ta_python_leave(&call);
    return result;
}

/* ---- context managers ---------------------------------------------------------------- */

static PyObject *ta_python_host_enter_with(PyObject *self, PyObject *unused) {
    (void)unused;
    ta_python_host *host = (ta_python_host *)self;
    if (host->has_async) {
        for (Py_ssize_t i = 0; i < PyList_Size(host->processors); i++) {
            PyObject *row = PyList_GetItem(host->processors, i);
            if (PyTuple_GetItem(row, 2) == Py_True) {
                PyErr_Format(PyExc_TypeError,
                             "the I/O processor %R has an async def handler: use async with, the synchronous driver "
                             "runs no event loop",
                             PyTuple_GetItem(row, 0));
                return NULL;
            }
        }
        if (host->wake_async || host->telemetry_async) {
            PyErr_Format(PyExc_TypeError, "the %s handler is an async def: use async with, the synchronous driver runs no "
                         "event loop", host->wake_async ? "wake" : "telemetry");
            return NULL;
        }
    }
    return Py_NewRef(self);
}

/* A telemetry handler's exception in the last take, once the host is freed. */
static int ta_python_host_closed_error(ta_python_host *host) {
    if (!host->error || host->host) return 0;
    PyErr_SetRaisedException(host->error);
    host->error = NULL;
    return -1;
}

static PyObject *ta_python_host_exit_with(PyObject *self, PyObject *arguments) {
    (void)arguments;
    ta_python_host_close((ta_python_host *)self, 0);
    if (ta_python_host_closed_error((ta_python_host *)self) < 0) return NULL;
    Py_RETURN_FALSE;
}

/* A future of the running loop holding `value` already. */
static PyObject *ta_python_done_future(PyObject *loop, PyObject *value) {
    PyObject *future = PyObject_CallMethod(loop, "create_future", NULL);
    if (!future) return NULL;
    PyObject *set = PyObject_CallMethod(future, "set_result", "O", value);
    if (!set) {
        Py_DECREF(future);
        return NULL;
    }
    Py_DECREF(set);
    return future;
}

static PyObject *ta_python_running_loop(void) {
    PyObject *asyncio = PyImport_ImportModule("asyncio");
    if (!asyncio) return NULL;
    PyObject *loop = PyObject_CallMethod(asyncio, "get_running_loop", NULL);
    Py_DECREF(asyncio);
    return loop;
}

static PyObject *ta_python_host_enter_async(PyObject *self, PyObject *unused) {
    (void)unused;
    ta_python_host *host = (ta_python_host *)self;
    PyObject *loop = ta_python_running_loop();
    if (!loop) return NULL;
    ta_python_replace(&host->loop, loop);
    return ta_python_done_future(loop, self);
}

static PyObject *ta_python_host_exit_async(PyObject *self, PyObject *arguments) {
    (void)arguments;
    ta_python_host *host = (ta_python_host *)self;
    PyObject *loop = Py_NewRef(host->loop);
    ta_python_host_close(host, 0);
    PyObject *result = NULL;
    if (ta_python_host_closed_error(host) == 0 && loop != Py_None) {
        if (host->ending_tasks && PyList_Size(host->ending_tasks) > 0) { /* the async last take: awaited here */
            PyObject *asyncio = PyImport_ImportModule("asyncio");
            PyObject *tasks = asyncio ? PyList_AsTuple(host->ending_tasks) : NULL;
            PyObject *gather = tasks ? PyObject_GetAttrString(asyncio, "gather") : NULL;
            result = gather ? PyObject_Call(gather, tasks, NULL) : NULL; /* raises the first exception */
            Py_XDECREF(gather);
            Py_XDECREF(tasks);
            Py_XDECREF(asyncio);
            PyList_SetSlice(host->ending_tasks, 0, PyList_Size(host->ending_tasks), NULL);
        } else result = ta_python_done_future(loop, Py_False);
    }
    Py_DECREF(loop);
    if (!result && !PyErr_Occurred()) PyErr_SetString(PyExc_RuntimeError, "async with outside an event loop");
    return result;
}

static PyObject *ta_python_host_closed(PyObject *self, void *closure) {
    (void)closure;
    return PyBool_FromLong(((ta_python_host *)self)->host == NULL);
}

static PyObject *ta_python_host_ticks(PyObject *self, void *closure) {
    (void)closure;
    return PyLong_FromUnsignedLongLong(((ta_python_host *)self)->ticks_per_second);
}

/* The last idle run's next_deadline (ta_run), as seconds from now. */
static PyObject *ta_python_host_next_deadline(PyObject *self, void *closure) {
    (void)closure;
    ta_python_host *host = (ta_python_host *)self;
    if (!host->host || host->deadline == UINT64_MAX) Py_RETURN_NONE;
    return PyFloat_FromDouble(ta_python_delay(host));
}

static PyGetSetDef ta_python_host_properties[] = {
    {"closed", ta_python_host_closed, NULL, ta_python_doc_host_closed, NULL},
    {"ticks_per_second", ta_python_host_ticks, NULL, ta_python_doc_host_ticks, NULL},
    {"next_deadline", ta_python_host_next_deadline, NULL, ta_python_doc_host_next_deadline, NULL},
    {NULL, NULL, NULL, NULL, NULL}
};

PyObject *ta_python_method_actor_state(PyObject *self, PyObject *arguments, PyObject *keywords);
PyObject *ta_python_method_actor_context(PyObject *self, PyObject *arguments, PyObject *keywords);
PyObject *ta_python_method_actor_list(PyObject *self, PyObject *arguments, PyObject *keywords);
PyObject *ta_python_method_actor_get(PyObject *self, PyObject *arguments, PyObject *keywords);
PyObject *ta_python_method_host_stats(PyObject *self, PyObject *arguments, PyObject *keywords);
PyObject *ta_python_method_host_config(PyObject *self, PyObject *arguments, PyObject *keywords);
PyObject *ta_python_method_trace_configure(PyObject *self, PyObject *arguments, PyObject *keywords);
PyObject *ta_python_method_trace_watch(PyObject *self, PyObject *arguments, PyObject *keywords);
PyObject *ta_python_method_trace_unwatch(PyObject *self, PyObject *arguments, PyObject *keywords);
PyObject *ta_python_method_trace_end(PyObject *self, PyObject *arguments, PyObject *keywords);
PyObject *ta_python_method_trace_spans_decode(PyObject *self, PyObject *arguments, PyObject *keywords);

#define TA_PYTHON_TYPED(name, method) \
    {#name, (PyCFunction)(void (*)(void))method, METH_VARARGS | METH_KEYWORDS, ta_python_doc_typed_##name}

static PyMethodDef ta_python_host_methods[] = {
    {"add_image", ta_python_host_add_image, METH_O,
     ta_python_doc_host_add_image},
    {"chart", ta_python_host_chart, METH_O, ta_python_doc_host_chart},
    {"actor_by_name", ta_python_host_actor_by_name, METH_O, ta_python_doc_host_actor_by_name},
    {"spawn", (PyCFunction)(void (*)(void))ta_python_host_spawn, METH_VARARGS | METH_KEYWORDS,
     ta_python_doc_host_spawn},
    {"send", (PyCFunction)(void (*)(void))ta_python_host_send, METH_VARARGS | METH_KEYWORDS,
     ta_python_doc_host_send},
    {"send_many", ta_python_host_send_many, METH_O,
     ta_python_doc_host_send_many},
    {"terminate", ta_python_host_terminate, METH_O, ta_python_doc_host_terminate},
    {"boot", ta_python_host_boot, METH_O, ta_python_doc_host_boot},
    {"metrics", ta_python_host_metrics, METH_NOARGS,
     ta_python_doc_host_metrics},
    {"sweep", ta_python_host_sweep, METH_VARARGS,
     ta_python_doc_host_sweep},
    {"hibernate", ta_python_host_hibernate, METH_O,
     ta_python_doc_host_hibernate},
    {"wake", ta_python_host_wake, METH_VARARGS,
     ta_python_doc_host_wake},
    {"snapshot", ta_python_host_snapshot, METH_NOARGS,
     ta_python_doc_host_snapshot},
    {"restore", (PyCFunction)(void (*)(void))ta_python_host_restore, METH_CLASS | METH_VARARGS | METH_KEYWORDS,
     ta_python_doc_host_restore},
    {"migrate", (PyCFunction)(void (*)(void))ta_python_host_migrate, METH_VARARGS | METH_KEYWORDS,
     ta_python_doc_host_migrate},
    {"run_until_idle", ta_python_host_run_until_idle, METH_NOARGS,
     ta_python_doc_host_run_until_idle},
    {"run", ta_python_host_run_forever, METH_NOARGS,
     ta_python_doc_host_run},
    {"idle", ta_python_host_idle, METH_NOARGS, ta_python_doc_host_idle},
    {"close", ta_python_host_close_method, METH_NOARGS,
     ta_python_doc_host_close},
    {"__enter__", ta_python_host_enter_with, METH_NOARGS, ta_python_doc_host_enter},
    {"__exit__", ta_python_host_exit_with, METH_VARARGS, ta_python_doc_host_exit},
    {"__aenter__", ta_python_host_enter_async, METH_NOARGS, ta_python_doc_host_aenter},
    {"__aexit__", ta_python_host_exit_async, METH_VARARGS, ta_python_doc_host_aexit},
    TA_PYTHON_TYPED(actor_state, ta_python_method_actor_state),
    TA_PYTHON_TYPED(actor_context, ta_python_method_actor_context),
    TA_PYTHON_TYPED(actor_list, ta_python_method_actor_list),
    TA_PYTHON_TYPED(actor_get, ta_python_method_actor_get),
    TA_PYTHON_TYPED(host_stats, ta_python_method_host_stats),
    TA_PYTHON_TYPED(host_config, ta_python_method_host_config),
    TA_PYTHON_TYPED(trace_configure, ta_python_method_trace_configure),
    TA_PYTHON_TYPED(trace_watch, ta_python_method_trace_watch),
    TA_PYTHON_TYPED(trace_unwatch, ta_python_method_trace_unwatch),
    TA_PYTHON_TYPED(trace_end, ta_python_method_trace_end),
    TA_PYTHON_TYPED(trace_spans_decode, ta_python_method_trace_spans_decode),
    {NULL, NULL, 0, NULL}
};

/* Weak references to a Host (the limited API's way: a member naming the
 * slot): a SimulatedClock holds its hosts weakly. */
static PyMemberDef ta_python_host_members[] = {
    {"__weaklistoffset__", Py_T_PYSSIZET, offsetof(ta_python_host, weakreferences), Py_READONLY, NULL},
    {NULL, 0, 0, 0, NULL}
};

static PyType_Slot ta_python_host_slots[] = {
    {Py_tp_members, ta_python_host_members},
    {Py_tp_doc, (void *)ta_python_doc_host},
    {Py_tp_new, ta_python_host_create},
    {Py_tp_dealloc, ta_python_host_dealloc},
    {Py_tp_traverse, ta_python_host_traverse},
    {Py_tp_clear, ta_python_host_clear},
    {Py_tp_methods, ta_python_host_methods},
    {Py_tp_getset, ta_python_host_properties},
    {0, NULL}
};

static PyType_Spec ta_python_host_spec = {
    "tinyactors.Host", sizeof(ta_python_host), 0,
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC | Py_TPFLAGS_IMMUTABLETYPE, ta_python_host_slots
};

int ta_python_host_init_type(PyObject *module, ta_python_module_state *state) {
    state->host_type = PyType_FromModuleAndSpec(module, &ta_python_host_spec, NULL);
    if (!state->host_type || PyModule_AddObjectRef(module, "Host", state->host_type) < 0) return -1;
    state->chart_type = PyType_FromModuleAndSpec(module, &ta_python_chart_spec, NULL);
    if (!state->chart_type) return -1;
    return PyModule_AddObjectRef(module, "ChartRef", state->chart_type);
}
