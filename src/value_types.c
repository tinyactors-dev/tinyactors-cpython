/* Python-owned representations of the two neutral values that built-in
 * Python types cannot preserve: tag 32 (Address) and undefined (UNDEFINED).
 * Both belong to the codec, not to an actor or Host. They contain no core
 * handles and remain usable after the message that produced them is gone.
 *
 * The public contract is docs/values.md. Address is immutable and holds an
 * exact str; the sentinel has no fields and cannot be constructed publicly.
 * Heap types and the sentinel are created separately for each module instance
 * and retained in module state. CPython's registration tables are read-only
 * from the extension's perspective; they contain no runtime identities. */
#include "extension.h"
#include "docs.h"

/* Visit both edges of an Address: its immutable text and its heap type.
 * CPython calls this while finding cycles, including type -> module -> type.
 * The type must be visited even though the text itself cannot form a cycle. */
static int ta_python_address_traverse(ta_python_address *self, visitproc visit, void *arg) {
    Py_VISIT(self->text);
    Py_VISIT((PyObject *)Py_TYPE((PyObject *)self));
    return 0;
}

/* Collector-only cleanup. Python exposes no operation that clears .text. */
static int ta_python_address_clear(ta_python_address *self) {
    Py_CLEAR(self->text);
    return 0;
}

/* Release the owned text, then the GC allocation and the instance's heap
 * type reference. Save the type until after tp_free has consumed self. */
static void ta_python_address_dealloc(ta_python_address *self) {
    PyTypeObject *type = Py_TYPE((PyObject *)self);
    PyObject_GC_UnTrack(self);
    ta_python_address_clear(self);
    freefunc ta_python_release = (freefunc)PyType_GetSlot(type, Py_tp_free);
    ta_python_release(self);
    Py_DECREF(type);
}

/* CPython tp_new for Address(text), one positional exact str. Accept text
 * according to the wire's UTF-8 contract; address routing/validation belongs
 * to core when a reference is used, not to this value wrapper. */
static PyObject *ta_python_address_new(PyTypeObject *type, PyObject *args, PyObject *kwargs) {
    PyObject *text;
    if (kwargs != NULL && PyDict_Size(kwargs) != 0) {
        PyErr_SetString(PyExc_TypeError, "Address requires one positional str");
        return NULL;
    }
    if (!PyArg_ParseTuple(args, "U:Address", &text)) return NULL;
    if (!PyUnicode_CheckExact(text)) {
        PyErr_SetString(PyExc_TypeError, "Address text must be an exact str");
        return NULL;
    }
    Py_ssize_t size;
    if (PyUnicode_AsUTF8AndSize(text, &size) == NULL) return NULL;
    if ((size_t)size > UINT32_MAX) {
        PyErr_SetString(PyExc_OverflowError, "Address text exceeds the core string length limit");
        return NULL;
    }
    ta_python_module_state *state = ta_python_codec_state(PyType_GetModule(type));
    if (state == NULL) return NULL;
    return ta_python_address_from_text(state, text);
}

/* Internal constructor: public construction and decoding share ownership.
 * PyType_GenericAlloc zeroes fields and tracks the GC-enabled instance. */
PyObject *ta_python_address_from_text(ta_python_module_state *state, PyObject *text) {
    ta_python_address *self = (ta_python_address *)PyType_GenericAlloc((PyTypeObject *)state->address_type, 0);
    if (self == NULL) return NULL;
    self->text = Py_NewRef(text);
    return (PyObject *)self;
}

/* Read-only .text getter: return an owned reference to the immutable text. */
static PyObject *ta_python_address_text(ta_python_address *self, void *closure) {
    (void)closure;
    return Py_NewRef(self->text);
}

/* The address's parts, read by core's parser (ta_address_parse): 1 parsed,
 * 0 not a tinyactors address, -1 with an exception. */
static int ta_python_address_parts(ta_python_address *self, ta_address *out) {
    Py_ssize_t size = 0;
    const char *text = PyUnicode_AsUTF8AndSize(self->text, &size);
    if (text == NULL) return -1;
    return ta_address_parse((ta_string_view){text, (uint32_t)size}, out) == TA_OK;
}

/* .system: the system id; None when the text is no address. */
static PyObject *ta_python_address_system(ta_python_address *self, void *closure) {
    (void)closure;
    ta_address parts;
    int parsed = ta_python_address_parts(self, &parts);
    if (parsed < 0) return NULL;
    return parsed ? PyUnicode_FromStringAndSize(parts.system.pointer, (Py_ssize_t)parts.system.length)
                  : Py_NewRef(Py_None);
}

/* .session: the session id of a #_scxml_<id> address; None otherwise. */
static PyObject *ta_python_address_session(ta_python_address *self, void *closure) {
    (void)closure;
    ta_address parts;
    int parsed = ta_python_address_parts(self, &parts);
    if (parsed < 0) return NULL;
    return parsed && parts.kind == TA_ADDRESS_SESSION ? PyLong_FromUnsignedLongLong(parts.session)
                                                      : Py_NewRef(Py_None);
}

/* .name: the decoded name of an @<name> address; None otherwise. The name
 * decodes in place of a bytes object of its encoded length (a decoded
 * name is never longer), then as UTF-8. */
static PyObject *ta_python_address_name(ta_python_address *self, void *closure) {
    (void)closure;
    ta_address parts;
    int parsed = ta_python_address_parts(self, &parts);
    if (parsed < 0) return NULL;
    if (!parsed || parts.kind != TA_ADDRESS_NAME) return Py_NewRef(Py_None);
    PyObject *bytes = PyBytes_FromStringAndSize(NULL, (Py_ssize_t)parts.name.length);
    char *room = bytes ? PyBytes_AsString(bytes) : NULL;
    PyObject *name = NULL;
    if (room) {
        ta_buffer out = {room, parts.name.length, 0};
        if (ta_address_name_decode(parts.name, &out) == TA_OK)
            name = PyUnicode_DecodeUTF8(room, (Py_ssize_t)out.length, "strict");
        else
            PyErr_SetString(PyExc_ValueError, "the address's name does not decode");
    }
    Py_XDECREF(bytes);
    return name;
}

/* str(Address) is for displaying or explicitly extracting its address.
 * It does not change the value's tagged encoding. */
static PyObject *ta_python_address_str(ta_python_address *self) {
    return Py_NewRef(self->text);
}

/* repr distinguishes a reference from an ordinary string during debugging. */
static PyObject *ta_python_address_repr(ta_python_address *self) {
    return PyUnicode_FromFormat("Address(%R)", self->text);
}

/* Immutable references can be dictionary/set keys in Python. The wire's
 * maps still require ordinary str keys; the codec rejects Address keys. */
static Py_hash_t ta_python_address_hash(ta_python_address *self) {
    return PyObject_Hash(self->text);
}

/* Compare text only between Address values of this module's exact type.
 * Returning NotImplemented for strings preserves the tagged distinction;
 * ordering is deliberately left to explicit comparisons of .text. */
static PyObject *ta_python_address_compare(PyObject *left, PyObject *right, int op) {
    if (Py_TYPE(left) != Py_TYPE(right) || (op != Py_EQ && op != Py_NE))
        return Py_NewRef(Py_NotImplemented);
    return PyObject_RichCompare(((ta_python_address *)left)->text,
                               ((ta_python_address *)right)->text, op);
}

/* The address core's formatter writes for `system` and `local` (a session
 * when `session` is not 0, else the name): written into a bytes object of
 * the longest it can be, then made a str and an Address. */
typedef struct ta_python_address_parts_in {
    ta_string_view system;
    ta_string_view name;
    uint64_t session;
} ta_python_address_parts_in;

static PyObject *ta_python_address_formatted(PyTypeObject *type, const ta_python_address_parts_in *in) {
    size_t room = in->session ? TA_ADDRESS_SESSION_MAX : 13u + TA_ADDRESS_SYSTEM_MAX + 2u + 3u * (size_t)in->name.length;
    PyObject *bytes = PyBytes_FromStringAndSize(NULL, (Py_ssize_t)room);
    if (!bytes) return NULL;
    ta_buffer out = {PyBytes_AsString(bytes), room, 0};
    ta_status status = in->session ? ta_address_format(in->system, in->session, &out)
                                   : ta_address_format_name(in->system, in->name, &out);
    PyObject *address = NULL;
    if (status != TA_OK) {
        PyErr_Format(PyExc_ValueError, "%s: %s", in->session ? "Address.from_session(system, session)"
                                                             : "Address.from_name(system, name)",
                     ta_address_system_valid(in->system) ? "an empty name" : "an invalid system id");
    } else {
        PyObject *text = PyUnicode_DecodeUTF8(out.pointer, (Py_ssize_t)out.length, "strict");
        ta_python_module_state *state = text ? ta_python_codec_state(PyType_GetModule(type)) : NULL;
        address = state ? ta_python_address_from_text(state, text) : NULL;
        Py_XDECREF(text);
    }
    Py_DECREF(bytes);
    return address;
}

/* A str's UTF-8, for the call. */
static int ta_python_address_view(PyObject *text, ta_string_view *out) {
    Py_ssize_t size = 0;
    const char *pointer = PyUnicode_AsUTF8AndSize(text, &size);
    if (!pointer) return -1;
    if ((size_t)size > UINT32_MAX) {
        PyErr_SetString(PyExc_OverflowError, "the text exceeds the core string length limit");
        return -1;
    }
    *out = (ta_string_view){pointer, (uint32_t)size};
    return 0;
}

/* Address.from_name(system, name): "tinyactors://<system>/@<name>", the name
 * percent-encoded by core (ta_address_format_name). */
static PyObject *ta_python_address_from_name(PyObject *type, PyObject *args) {
    PyObject *system, *name;
    if (!PyArg_ParseTuple(args, "UU:Address.from_name", &system, &name)) return NULL;
    ta_python_address_parts_in in = {0};
    if (ta_python_address_view(system, &in.system) < 0 || ta_python_address_view(name, &in.name) < 0) return NULL;
    return ta_python_address_formatted((PyTypeObject *)type, &in);
}

/* Address.from_session(system, session): "tinyactors://<system>/#_scxml_<session>"
 * (ta_address_format), the session an int from 1 to 2**64-1. */
static PyObject *ta_python_address_from_session(PyObject *type, PyObject *args) {
    PyObject *system, *session;
    if (!PyArg_ParseTuple(args, "UO:Address.from_session", &system, &session)) return NULL;
    if (!PyLong_Check(session) || PyBool_Check(session)) {
        PyErr_SetString(PyExc_TypeError, "Address.from_session(system, session): session is an int");
        return NULL;
    }
    ta_python_address_parts_in in = {0};
    in.session = PyLong_AsUnsignedLongLong(session);
    if (in.session == (uint64_t)-1 && PyErr_Occurred()) return NULL;
    if (in.session == 0) {
        PyErr_SetString(PyExc_ValueError, "Address.from_session(system, session): session is from 1");
        return NULL;
    }
    if (ta_python_address_view(system, &in.system) < 0) return NULL;
    return ta_python_address_formatted((PyTypeObject *)type, &in);
}

static PyMethodDef ta_python_address_methods[] = {
    {"from_name", ta_python_address_from_name, METH_VARARGS | METH_CLASS, ta_python_doc_address_from_name},
    {"from_session", ta_python_address_from_session, METH_VARARGS | METH_CLASS, ta_python_doc_address_from_session},
    {NULL, NULL, 0, NULL}
};

static PyGetSetDef ta_python_address_properties[] = {
    {"text", (getter)ta_python_address_text, NULL, ta_python_doc_address_text, NULL},
    {"system", (getter)ta_python_address_system, NULL, ta_python_doc_address_system, NULL},
    {"session", (getter)ta_python_address_session, NULL, ta_python_doc_address_session, NULL},
    {"name", (getter)ta_python_address_name, NULL, ta_python_doc_address_name, NULL},
    {NULL, NULL, NULL, NULL, NULL}
};

static PyType_Slot ta_python_address_slots[] = {
    {Py_tp_doc, (void *)ta_python_doc_address},
    {Py_tp_new, ta_python_address_new},
    {Py_tp_dealloc, ta_python_address_dealloc},
    {Py_tp_traverse, ta_python_address_traverse},
    {Py_tp_clear, ta_python_address_clear},
    {Py_tp_getset, ta_python_address_properties},
    {Py_tp_methods, ta_python_address_methods},
    {Py_tp_str, ta_python_address_str},
    {Py_tp_repr, ta_python_address_repr},
    {Py_tp_hash, ta_python_address_hash},
    {Py_tp_richcompare, ta_python_address_compare},
    {0, NULL}
};

static PyType_Spec ta_python_address_spec = {
    "tinyactors.Address", sizeof(ta_python_address), 0,
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC | Py_TPFLAGS_IMMUTABLETYPE,
    ta_python_address_slots
};

/* The sentinel has no fields, but its heap type still participates in GC. */
static int ta_python_undefined_traverse(PyObject *self, visitproc visit, void *arg) {
    Py_VISIT((PyObject *)Py_TYPE(self));
    return 0;
}

/* Match the GC allocation and release the sentinel's owned heap type. */
static void ta_python_undefined_dealloc(PyObject *self) {
    PyTypeObject *type = Py_TYPE(self);
    PyObject_GC_UnTrack(self);
    freefunc ta_python_release = (freefunc)PyType_GetSlot(type, Py_tp_free);
    ta_python_release(self);
    Py_DECREF(type);
}

/* Only module initialization allocates this singleton, via GenericAlloc.
 * Even type(UNDEFINED)() must refuse rather than manufacture another value. */
static PyObject *ta_python_undefined_new(PyTypeObject *type, PyObject *args, PyObject *kwargs) {
    (void)type;
    (void)args;
    (void)kwargs;
    PyErr_SetString(PyExc_TypeError, "use tinyactors.UNDEFINED; undefined cannot be constructed");
    return NULL;
}

/* Stable diagnostic spelling; compare the sentinel by identity, not repr. */
static PyObject *ta_python_undefined_repr(PyObject *self) {
    (void)self;
    return PyUnicode_FromString("UNDEFINED");
}

static PyType_Slot ta_python_undefined_slots[] = {
    {Py_tp_doc, (void *)ta_python_doc_undefined},
    {Py_tp_new, ta_python_undefined_new},
    {Py_tp_dealloc, ta_python_undefined_dealloc},
    {Py_tp_traverse, ta_python_undefined_traverse},
    {Py_tp_repr, ta_python_undefined_repr},
    {0, NULL}
};

static PyType_Spec ta_python_undefined_spec = {
    "tinyactors._Undefined", sizeof(PyObject), 0,
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC | Py_TPFLAGS_IMMUTABLETYPE,
    ta_python_undefined_slots
};

/* Install owned identities before exposing them. AddObjectRef retains its
 * own module-dictionary reference; module state retains the original one.
 * Partial initialization remains traversable and clearable on failure. */
int ta_python_value_types_init(PyObject *module) {
    ta_python_module_state *state = PyModule_GetState(module);
    state->address_type = PyType_FromModuleAndSpec(module, &ta_python_address_spec, NULL);
    if (state->address_type == NULL) return -1;
    state->undefined_type = PyType_FromModuleAndSpec(module, &ta_python_undefined_spec, NULL);
    if (state->undefined_type == NULL) return -1;
    state->undefined = PyType_GenericAlloc((PyTypeObject *)state->undefined_type, 0);
    if (state->undefined == NULL) return -1;
    if (PyModule_AddObjectRef(module, "Address", state->address_type) < 0) return -1;
    return PyModule_AddObjectRef(module, "UNDEFINED", state->undefined);
}
