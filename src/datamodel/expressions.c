/* The "python" datamodel's expressions (AGENTS.md, "The "python"
 * datamodel"): the restrictive rewrite and the chart classes, in Python,
 * in support.py beside this file (the build writes it into support.inc as
 * a C string). The AST is not in the limited C API, so the rewrite walks
 * it with Python's own `ast` module, once per chart at chart_attach and
 * once per `.py` file a resolver reads; an evaluation is one call of a
 * compiled function of the session. In() is C, over the evaluation's
 * environment. */
#include "datamodel.h"
#include "../docs.h"

static const char ta_python_support_source[] =
#include "support.inc"
    ;

static PyObject *ta_python_support_get(PyObject *namespace, const char *name) {
    PyObject *value = NULL;
    if (PyDict_GetItemStringRef(namespace, name, &value) == 0)
        PyErr_Format(PyExc_RuntimeError, "support.py defines no %s", name);
    return value;
}

static PyObject *ta_python_attribute_of(const char *module, const char *name) {
    PyObject *imported = PyImport_ImportModule(module);
    if (!imported) return NULL;
    PyObject *value = PyObject_GetAttrString(imported, name);
    Py_DECREF(imported);
    return value;
}

/* Run support.py once for this module instance, and keep what C uses. */
int ta_python_support_init(PyObject *module, ta_python_module_state *state) {
    PyObject *namespace = PyDict_New(), *builtins = PyImport_ImportModule("builtins");
    PyObject *name = PyUnicode_FromString("tinyactors._support");
    int status = namespace && builtins && name && PyDict_SetItemString(namespace, "__builtins__", builtins) == 0 &&
                         PyDict_SetItemString(namespace, "__name__", name) == 0
                     ? 0
                     : -1;
    Py_XDECREF(name);
    PyObject *code = status == 0 ? Py_CompileString(ta_python_support_source, "tinyactors/support.py", Py_file_input) : NULL;
    PyObject *done = code ? PyEval_EvalCode(code, namespace, namespace) : NULL;
    Py_XDECREF(code);
    if (!done) status = -1;
    Py_XDECREF(done);
    if (status == 0) {
        state->support = Py_NewRef(namespace);
        state->error = ta_python_support_get(namespace, "TinyactorsError");
        state->compile_error = ta_python_support_get(namespace, "CompileError");
        state->resolve_error = ta_python_support_get(namespace, "ResolveError");
        state->diagnostic = ta_python_support_get(namespace, "Diagnostic");
        state->attach = ta_python_support_get(namespace, "attach");
        state->check_file = ta_python_support_get(namespace, "check_file");
        PyObject *names = ta_python_support_get(namespace, "BUILTINS");
        state->builtins = PyDict_New();
        for (Py_ssize_t i = 0; names && state->builtins && i < PyTuple_Size(names); i++) {
            PyObject *name = PyTuple_GetItem(names, i), *value = PyObject_GetAttr(builtins, name);
            if (!value || PyDict_SetItem(state->builtins, name, value) < 0) status = -1;
            Py_XDECREF(value);
        }
        Py_XDECREF(names);
        state->time_ns = ta_python_attribute_of("time", "time_ns");
        state->coroutine_function = ta_python_attribute_of("inspect", "iscoroutinefunction");
        state->event_kinds = Py_BuildValue("(sss)", "platform", "internal", "external");
        state->event_key = PyUnicode_InternFromString("_event");
        if (!state->error || !state->compile_error || !state->resolve_error || !state->diagnostic || !state->attach ||
            !state->check_file || !names || !state->builtins || !state->time_ns || !state->coroutine_function ||
            !state->event_kinds || !state->event_key)
            status = -1;
        static const char *const exported[] = {"TinyactorsError", "CompileError", "ResolveError", "Diagnostic"};
        for (size_t i = 0; status == 0 && i < sizeof exported / sizeof exported[0]; i++) {
            PyObject *value = ta_python_support_get(namespace, exported[i]);
            if (!value || PyModule_AddObjectRef(module, exported[i], value) < 0) status = -1;
            Py_XDECREF(value);
        }
    }
    Py_XDECREF(namespace);
    Py_XDECREF(builtins);
    return status;
}

int ta_python_check_file(ta_python_module_state *state, PyObject *file, PyObject *source) {
    PyObject *done = PyObject_CallFunctionObjArgs(state->check_file, source, file, Py_None, NULL);
    if (!done) return -1;
    Py_DECREF(done);
    return 0;
}

/* ---- In() ----------------------------------------------------------------------- */

/* In(state): the active state configuration of the evaluating session
 * (SCXML's In()), through the evaluation's environment. */
static PyObject *ta_python_in(PyObject *self, PyObject *name) {
    ta_python_datamodel *datamodel = PyCapsule_GetContext(self); /* NULL once the host is freed */
    if (!datamodel) {
        if (!PyErr_Occurred()) PyErr_SetString(PyExc_RuntimeError, "In() of a freed host");
        return NULL;
    }
    ta_datamodel_environment *environment = datamodel->environment;
    ta_string_view text;
    if (!PyUnicode_Check(name) || ta_python_text(name, &text) < 0) {
        if (!PyErr_Occurred()) PyErr_SetString(PyExc_TypeError, "In() takes a state's id, a str");
        return NULL;
    }
    if (!environment) {
        PyErr_SetString(PyExc_RuntimeError, "In() outside an evaluation");
        return NULL;
    }
    const ta_image *image = ta_datamodel_environment_image(environment);
    uint32_t state = ta_image_state_by_name(image, ta_datamodel_environment_chart(environment), text);
    return PyBool_FromLong(state != TA_NIL && ta_datamodel_environment_in(environment, state));
}

static PyMethodDef ta_python_in_def = {"In", ta_python_in, METH_O, ta_python_doc_in};

/* The globals of a host's chart code: the allowed builtins and In().
 * In() reaches the datamodel through a capsule that holds no reference to
 * the Host, so a Host the application drops is freed at once, not at the
 * collector's next pass; ta_python_datamodel_free clears the capsule. */
int ta_python_namespace_init(ta_python_datamodel *datamodel) {
    datamodel->capsule = PyCapsule_New(datamodel, "tinyactors.datamodel", NULL);
    if (!datamodel->capsule || PyCapsule_SetContext(datamodel->capsule, datamodel) < 0) return -1;
    PyObject *in = PyCFunction_New(&ta_python_in_def, datamodel->capsule);
    datamodel->namespace = in ? PyDict_New() : NULL;
    int status = datamodel->namespace && PyDict_SetItemString(datamodel->namespace, "__builtins__", datamodel->state->builtins) == 0 &&
                         PyDict_SetItemString(datamodel->namespace, "In", in) == 0
                     ? 0
                     : -1;
    Py_XDECREF(in);
    return status;
}

/* ---- a chart ---------------------------------------------------------------------- */

static PyObject *ta_python_view_str(ta_string_view text) {
    return PyUnicode_DecodeUTF8(text.pointer ? text.pointer : "", text.length, "replace");
}

int ta_python_chart_compile(ta_python_datamodel *datamodel, const ta_python_attach *attach, ta_python_chart_row *row) {
    const ta_image *image = attach->image;
    uint32_t chart = attach->chart;
    uint32_t data_count = ta_image_data_count(image, chart), expression_count = ta_image_expression_count(image, chart);
    PyObject *data = PyTuple_New(data_count), *expressions = data ? PyList_New(0) : NULL;
    int status = expressions ? 0 : -1;
    for (uint32_t i = 0; status == 0 && i < data_count; i++) {
        ta_data_info info;
        PyObject *id = ta_image_data(image, (ta_chart_item){chart, i}, &info) == TA_OK ? ta_python_view_str(info.id) : NULL;
        if (!id) status = -1;
        else PyTuple_SetItem(data, i, id);
    }
    for (uint32_t i = 0; status == 0 && i < expression_count; i++) {
        ta_expression_info info;
        if (ta_image_expression(image, (ta_chart_item){chart, i}, &info) != TA_OK) {
            status = -1;
            break;
        }
        PyObject *source = ta_python_view_str(info.source);
        PyObject *entry = source ? Py_BuildValue("(IOI)", info.kind, source, info.line ? info.line : 1) : NULL;
        if (!entry || PyList_Append(expressions, entry) < 0) status = -1;
        Py_XDECREF(source);
        Py_XDECREF(entry);
    }
    PyObject *name = status == 0 ? ta_python_view_str(ta_image_chart_name(image, chart)) : NULL;
    PyObject *file = name ? ta_python_view_str(ta_image_chart_source(image, chart)) : NULL;
    PyObject *made = file ? PyObject_CallFunctionObjArgs(datamodel->state->attach, name, data, expressions,
                                                         datamodel->namespace, file, NULL)
                          : NULL;
    Py_XDECREF(name);
    Py_XDECREF(file);
    Py_XDECREF(expressions);
    if (!made || !PyTuple_Check(made) || PyTuple_Size(made) != 4) {
        Py_XDECREF(made);
        Py_XDECREF(data);
        return -1;
    }
    row->chart = Py_NewRef(PyTuple_GetItem(made, 0));
    row->functions = Py_NewRef(PyTuple_GetItem(made, 1));
    row->data = Py_NewRef(PyTuple_GetItem(made, 3)); /* the <data> ids first, then what the chart declares */
    Py_DECREF(data);
    ta_image_info info;
    info.size = sizeof info;
    ta_image_get_info(image, &info);
    row->names = PyList_New(info.event_name_count);
    for (uint32_t i = 0; row->names && i < info.event_name_count; i++) PyList_SetItem(row->names, i, Py_NewRef(Py_None));
    row->invokeids = PyList_New(0); /* grown to a slot at its first event */
    row->origins = PyList_New(0);
    PyObject *reads = PyTuple_GetItem(made, 2);
    static const char *const names[] = {"_sessionid", "_name", "_ioprocessors"};
    for (uint32_t i = 0; i < 3; i++) {
        PyObject *key = PyUnicode_FromString(names[i]);
        if (key && PySet_Contains(reads, key) == 1) row->reads |= 1u << i;
        Py_XDECREF(key);
    }
    PyErr_Clear();
    Py_DECREF(made);
    return 0;
}
