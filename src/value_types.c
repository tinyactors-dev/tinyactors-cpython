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

static PyGetSetDef ta_python_address_properties[] = {
    {"text", (getter)ta_python_address_text, NULL, ta_python_doc_address_text, NULL},
    {NULL, NULL, NULL, NULL, NULL}
};

static PyType_Slot ta_python_address_slots[] = {
    {Py_tp_doc, (void *)ta_python_doc_address},
    {Py_tp_new, ta_python_address_new},
    {Py_tp_dealloc, ta_python_address_dealloc},
    {Py_tp_traverse, ta_python_address_traverse},
    {Py_tp_clear, ta_python_address_clear},
    {Py_tp_getset, ta_python_address_properties},
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
