/* The compiler (AGENTS.md, "The API"): compile_charts is
 * ta_compile_charts, compile_system is ta_compile_system (a manifest:
 * charts, boot actors, I/O processors), Compiled holds the result of either, and a resolver, a callable
 * resolver(base, source) -> str, is its ta_compile_options.resolve, the
 * only place a resolver is given: it lives at compile time only. */
#include "extension.h"
#include "docs.h"
#include <string.h>


typedef struct ta_python_compiled {
    PyObject_HEAD
    ta_compile_result *result; /* NULL once closed */
    Py_ssize_t exports;        /* buffers of `image` held */
    PyObject *diagnostics;
} ta_python_compiled;

/* ---- diagnostics ----------------------------------------------------------------- */

static PyObject *ta_python_view_text(ta_string_view text) {
    return PyUnicode_DecodeUTF8(text.pointer ? text.pointer : "", text.length, "replace");
}

static const char *const ta_python_severities[] = {"note", "warning", "error"};

/* Every diagnostic of a result, converted once: a list of Diagnostic. */
static PyObject *ta_python_diagnostics(ta_python_module_state *state, const ta_compile_result *result, int *errors) {
    const ta_diagnostic *diagnostics = NULL;
    uint32_t count = result ? ta_compile_diagnostics(result, &diagnostics) : 0;
    PyObject *list = PyList_New(0);
    for (uint32_t i = 0; list && i < count; i++) {
        const ta_diagnostic *d = &diagnostics[i];
        if (d->severity == TA_DIAGNOSTIC_ERROR) (*errors)++;
        PyObject *file = ta_python_view_text(d->file), *message = ta_python_view_text(d->message);
        PyObject *item = file && message ? PyObject_CallFunction(state->diagnostic, "sIOIIO",
                                                                 ta_python_severities[d->severity < 3 ? d->severity : 2],
                                                                 d->code, file, d->line, d->column, message)
                                         : NULL;
        Py_XDECREF(file);
        Py_XDECREF(message);
        if (!item || PyList_Append(list, item) < 0) Py_CLEAR(list);
        Py_XDECREF(item);
    }
    return list;
}

/* ---- the resolver ----------------------------------------------------------------- */

typedef struct ta_python_resolving {
    ta_python_module_state *state;
    PyObject *resolver;
    PyObject *kept;    /* the texts handed to the compiler, alive until it returns */
    PyObject *raised;  /* an exception that is no refusal: raised after the compile */
} ta_python_resolving;

/* A refusal's message into the compilation's scratch arena. */
static ta_string_view ta_python_scratch_text(const ta_resolve_request *request, PyObject *message) {
    ta_string_view text = {"", 0};
    PyObject *string = message ? PyObject_Str(message) : NULL;
    Py_ssize_t length = 0;
    const char *utf8 = string ? PyUnicode_AsUTF8AndSize(string, &length) : NULL;
    char *copy = utf8 && length ? ta_growable_arena_alloc(request->scratch, (size_t)length, 1) : NULL;
    if (copy) {
        memcpy(copy, utf8, (size_t)length);
        text = (ta_string_view){copy, (uint32_t)length};
    }
    Py_XDECREF(string);
    PyErr_Clear();
    return text;
}

static uint32_t ta_python_attribute_number(PyObject *object, const char *name) {
    PyObject *value = PyObject_GetAttrString(object, name);
    uint32_t number = value && PyLong_Check(value) ? (uint32_t)PyLong_AsUnsignedLong(value) : 0;
    Py_XDECREF(value);
    PyErr_Clear();
    return number;
}

/* The exception the resolver raised (set now) as the compiler's error:
 * ResolveError and SyntaxError are refusals at their line; anything else
 * is kept and raised from compile_charts. */
static ta_status ta_python_refusal(ta_python_resolving *resolving, const ta_resolve_request *request, ta_resolve_result *out) {
    PyObject *raised = PyErr_GetRaisedException();
    if (PyErr_GivenExceptionMatches(raised, resolving->state->resolve_error)) {
        PyObject *message = PyObject_GetAttrString(raised, "message");
        out->error.message = ta_python_scratch_text(request, message);
        out->error.line = ta_python_attribute_number(raised, "line");
        out->error.column = ta_python_attribute_number(raised, "column");
        Py_XDECREF(message);
    } else if (PyErr_GivenExceptionMatches(raised, PyExc_SyntaxError)) {
        PyObject *message = PyObject_GetAttrString(raised, "msg");
        out->error.message = ta_python_scratch_text(request, message);
        out->error.line = ta_python_attribute_number(raised, "lineno");
        out->error.column = ta_python_attribute_number(raised, "offset");
        Py_XDECREF(message);
    } else {
        out->error.message = ta_python_scratch_text(request, raised);
        if (!resolving->raised) resolving->raised = Py_NewRef(raised);
    }
    Py_DECREF(raised);
    return TA_ERROR_INVALID;
}

static int ta_python_ends_with(ta_string_view text, const char *suffix) {
    size_t length = strlen(suffix);
    return text.length >= length && !memcmp(text.pointer + text.length - length, suffix, length);
}

static ta_status ta_python_resolve(void *context, const ta_resolve_request *request, ta_resolve_result *out) {
    ta_python_resolving *resolving = context;
    PyObject *base = ta_python_view_text(request->base), *source = ta_python_view_text(request->source);
    PyObject *text = base && source ? PyObject_CallFunctionObjArgs(resolving->resolver, base, source, NULL) : NULL;
    ta_status status = TA_OK;
    if (text && !PyUnicode_Check(text)) {
        Py_CLEAR(text);
        PyErr_SetString(PyExc_TypeError, "resolver(base, source) returns a str");
    }
    /* a .py file gets the "python" datamodel's check as it is read */
    if (text && ta_python_ends_with(request->source, ".py") && ta_python_check_file(resolving->state, source, text) < 0)
        Py_CLEAR(text);
    if (!text || PyList_Append(resolving->kept, text) < 0) status = ta_python_refusal(resolving, request, out);
    else if (ta_python_text(text, &out->source.text) < 0) status = ta_python_refusal(resolving, request, out);
    else out->source.name = request->source;
    Py_XDECREF(text);
    Py_XDECREF(base);
    Py_XDECREF(source);
    return status;
}

/* ---- compile_charts, compile_system -------------------------------------------------- */

/* The arguments of a compile call: `first` is the sources, or the manifest
 * as a list of its one pair; `name` is the call's, for its messages. */
typedef struct ta_python_compile_call {
    const char *name;
    const char *first; /* the first parameter's keyword */
    int manifest;
} ta_python_compile_call;

typedef struct ta_python_arguments {
    PyObject *const *args;
    Py_ssize_t count;
    PyObject *names;
} ta_python_arguments;

static PyObject *ta_python_compile_with(PyObject *module, const ta_python_arguments *given,
                                        const ta_python_compile_call *how) {
    ta_python_module_state *state = ta_python_codec_state(module);
    if (!state) return NULL;
    PyObject *const *args = given->args;
    Py_ssize_t count = given->count;
    PyObject *sources = NULL, *resolver = Py_None;
    Py_ssize_t keywords = given->names ? PyTuple_Size(given->names) : 0;
    if (count >= 1) sources = args[0];
    if (count >= 2) resolver = args[1];
    for (Py_ssize_t i = 0; i < keywords; i++) {
        PyObject *name = PyTuple_GetItem(given->names, i);
        if (PyUnicode_CompareWithASCIIString(name, "resolver") == 0 && count < 2) resolver = args[count + i];
        else if (PyUnicode_CompareWithASCIIString(name, how->first) == 0 && count < 1) sources = args[count + i];
        else {
            PyErr_Format(PyExc_TypeError, "%s() got an unexpected keyword argument %R", how->name, name);
            return NULL;
        }
    }
    if (!sources || count > 2) {
        PyErr_Format(PyExc_TypeError, "%s(%s, resolver=None)", how->name, how->first);
        return NULL;
    }
    PyObject *list = how->manifest ? PyList_New(1) : PySequence_List(sources);
    if (!list) return NULL;
    if (how->manifest) PyList_SetItem(list, 0, Py_NewRef(sources));
    Py_ssize_t total = PyList_Size(list);
    /* the ta_source array in a Python bytes: the call's memory, no host here */
    PyObject *room = PyBytes_FromStringAndSize(NULL, (Py_ssize_t)sizeof(ta_source) * (total ? total : 1));
    ta_python_resolving resolving = {state, resolver, PyList_New(0), NULL};
    PyObject *result = NULL;
    if (!room || !resolving.kept) goto done;
    ta_source *charts = (ta_source *)PyBytes_AsString(room);
    for (Py_ssize_t i = 0; i < total; i++) {
        PyObject *item = PyList_GetItem(list, i), *pair = PySequence_Tuple(item);
        int ok = pair && PyTuple_Size(pair) == 2 && PyUnicode_Check(PyTuple_GetItem(pair, 0)) &&
                 PyUnicode_Check(PyTuple_GetItem(pair, 1)) && PyList_Append(resolving.kept, pair) == 0 &&
                 ta_python_text(PyTuple_GetItem(pair, 0), &charts[i].name) == 0 &&
                 ta_python_text(PyTuple_GetItem(pair, 1), &charts[i].text) == 0;
        Py_XDECREF(pair);
        if (!ok) {
            if (!PyErr_Occurred())
                PyErr_SetString(PyExc_TypeError, how->manifest ? "a manifest is a (name, text) pair of str"
                                                               : "a source is a (name, text) pair of str");
            goto done;
        }
    }
    if (resolver != Py_None && !PyCallable_Check(resolver)) {
        PyErr_SetString(PyExc_TypeError, "a resolver is a callable resolver(base, source) -> str");
        goto done;
    }
    ta_compile_options options;
    memset(&options, 0, sizeof options);
    options.size = sizeof options;
    if (resolver != Py_None) {
        options.resolve = ta_python_resolve;
        options.resolve_context = &resolving;
    }
    ta_compile_result *compiled = NULL;
    ta_python_compilation compilation = {{charts, (uint32_t)total}, how->manifest};
    ta_status status = ta_python_compile(&options, &compilation, &compiled);
    int errors = 0;
    PyObject *diagnostics = ta_python_diagnostics(state, compiled, &errors);
    if (!diagnostics || resolving.raised || status != TA_OK || errors) {
        if (compiled) ta_compile_free(compiled); /* freed before it raises */
        if (!diagnostics) goto done;
        if (resolving.raised) {
            PyErr_SetObject((PyObject *)Py_TYPE(resolving.raised), resolving.raised);
        } else if (status == TA_ERROR_NO_MEMORY) {
            PyErr_NoMemory();
        } else {
            PyObject *first = NULL;
            for (Py_ssize_t i = 0; i < PyList_Size(diagnostics) && !first; i++) {
                PyObject *severity = PyObject_GetAttrString(PyList_GetItem(diagnostics, i), "severity");
                if (severity && PyUnicode_CompareWithASCIIString(severity, "error") == 0)
                    first = PyObject_Repr(PyList_GetItem(diagnostics, i));
                Py_XDECREF(severity);
            }
            PyObject *error = PyObject_CallFunction(state->compile_error, "OO", first ? first : Py_None, diagnostics);
            if (error) PyErr_SetObject(state->compile_error, error);
            Py_XDECREF(error);
            Py_XDECREF(first);
        }
        Py_DECREF(diagnostics);
        goto done;
    }
    ta_python_compiled *object = (ta_python_compiled *)PyType_GenericAlloc((PyTypeObject *)state->compiled_type, 0);
    if (!object) {
        ta_compile_free(compiled);
        Py_DECREF(diagnostics);
        goto done;
    }
    object->result = compiled;
    object->diagnostics = diagnostics;
    result = (PyObject *)object;
done:
    Py_XDECREF(resolving.raised);
    Py_XDECREF(resolving.kept); /* after the compile: the views pointed into them */
    Py_XDECREF(room);
    Py_DECREF(list);
    return result;
}

PyObject *ta_python_compile_charts(PyObject *module, PyObject *const *args, Py_ssize_t count, PyObject *names) {
    static const ta_python_compile_call how = {"compile_charts", "sources", 0};
    return ta_python_compile_with(module, &(ta_python_arguments){args, count, names}, &how);
}

PyObject *ta_python_compile_system(PyObject *module, PyObject *const *args, Py_ssize_t count, PyObject *names) {
    static const ta_python_compile_call how = {"compile_system", "manifest", 1};
    return ta_python_compile_with(module, &(ta_python_arguments){args, count, names}, &how);
}

/* ---- Compiled -------------------------------------------------------------------------- */

static int ta_python_compiled_getbuffer(PyObject *self, Py_buffer *view, int flags) {
    ta_python_compiled *compiled = (ta_python_compiled *)self;
    if (!compiled->result) {
        PyErr_SetString(PyExc_BufferError, "the compile result was closed");
        return -1;
    }
    ta_string_view image = ta_compile_image(compiled->result);
    if (PyBuffer_FillInfo(view, self, (void *)image.pointer, image.length, 1, flags) < 0) return -1;
    compiled->exports++;
    return 0;
}

static void ta_python_compiled_releasebuffer(PyObject *self, Py_buffer *view) {
    (void)view;
    ((ta_python_compiled *)self)->exports--;
}

/* A thin view of the image's bytes: a read-only memoryview over the
 * result, valid until close(). */
static PyObject *ta_python_compiled_image(PyObject *self, void *closure) {
    (void)closure;
    ta_python_compiled *compiled = (ta_python_compiled *)self;
    if (!compiled->result) {
        ta_python_module_state *state = ta_python_state_of_type(Py_TYPE(self));
        if (state) ta_python_raise_status(state, TA_ERROR_STALE);
        return NULL;
    }
    return PyMemoryView_FromObject(self);
}

static PyObject *ta_python_compiled_diagnostics(PyObject *self, void *closure) {
    (void)closure;
    return Py_NewRef(((ta_python_compiled *)self)->diagnostics);
}

static PyObject *ta_python_compiled_close(PyObject *self, PyObject *unused) {
    (void)unused;
    ta_python_compiled *compiled = (ta_python_compiled *)self;
    if (compiled->exports > 0) { /* as bytearray: a held buffer would read freed memory */
        PyErr_SetString(PyExc_BufferError, "close() while a buffer of Compiled.image is held: release it first");
        return NULL;
    }
    if (compiled->result) ta_compile_free(compiled->result);
    compiled->result = NULL;
    Py_RETURN_NONE;
}

static PyObject *ta_python_compiled_enter(PyObject *self, PyObject *unused) {
    (void)unused;
    return Py_NewRef(self);
}

static PyObject *ta_python_compiled_exit(PyObject *self, PyObject *arguments) {
    (void)arguments;
    PyObject *closed = ta_python_compiled_close(self, NULL);
    if (!closed) return NULL;
    Py_DECREF(closed);
    Py_RETURN_FALSE;
}

static void ta_python_compiled_dealloc(PyObject *self) {
    PyTypeObject *type = Py_TYPE(self);
    ta_python_compiled *compiled = (ta_python_compiled *)self;
    if (compiled->result) ta_compile_free(compiled->result); /* the collector's fallback */
    Py_XDECREF(compiled->diagnostics);
    freefunc ta_python_release = (freefunc)PyType_GetSlot(type, Py_tp_free);
    ta_python_release(self);
    Py_DECREF(type);
}

static PyGetSetDef ta_python_compiled_properties[] = {
    {"image", ta_python_compiled_image, NULL, ta_python_doc_compiled_image, NULL},
    {"diagnostics", ta_python_compiled_diagnostics, NULL, ta_python_doc_compiled_diagnostics, NULL},
    {NULL, NULL, NULL, NULL, NULL}
};

static PyMethodDef ta_python_compiled_methods[] = {
    {"close", ta_python_compiled_close, METH_NOARGS, ta_python_doc_compiled_close},
    {"__enter__", ta_python_compiled_enter, METH_NOARGS, ta_python_doc_compiled_enter},
    {"__exit__", ta_python_compiled_exit, METH_VARARGS, ta_python_doc_compiled_exit},
    {NULL, NULL, 0, NULL}
};

static PyType_Slot ta_python_compiled_slots[] = {
    {Py_tp_doc, (void *)ta_python_doc_compiled},
    {Py_tp_dealloc, ta_python_compiled_dealloc},
    {Py_tp_getset, ta_python_compiled_properties},
    {Py_tp_methods, ta_python_compiled_methods},
    {Py_bf_getbuffer, ta_python_compiled_getbuffer},
    {Py_bf_releasebuffer, ta_python_compiled_releasebuffer},
    {0, NULL}
};

static PyType_Spec ta_python_compiled_spec = {
    "tinyactors.Compiled", sizeof(ta_python_compiled), 0,
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_IMMUTABLETYPE | Py_TPFLAGS_DISALLOW_INSTANTIATION, ta_python_compiled_slots
};

int ta_python_compile_init_type(PyObject *module, ta_python_module_state *state) {
    state->compiled_type = PyType_FromModuleAndSpec(module, &ta_python_compiled_spec, NULL);
    if (!state->compiled_type) return -1;
    return PyModule_AddObjectRef(module, "Compiled", state->compiled_type);
}
