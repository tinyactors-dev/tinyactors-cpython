/* The binding's single Python <-> CBOR conversion, behind public
 * tinyactors.encode/decode. The conformance adapter is the current user;
 * later config, control-plane and process-boundary paths must reuse this
 * conversion rather than add another codec. This does not implement SCXML
 * evaluation, payload tables or in-process event delivery.
 *
 * Core owns CBOR syntax, wire widths and cursor validation (cbor.h).
 * This file owns the Python type mapping (docs/values.md). Each call owns
 * its writer/reader; recursive helpers borrow their arguments. Python
 * objects allocated while reading are released on error or returned as
 * new references. No core-owned byte view escapes to Python. */
#include "extension.h"
#include <string.h>

/* Group the per-call stream with borrowed interpreter identities. This
 * keeps recursive helpers independent of mutable module export names and
 * avoids a global singleton/type pointer or a fourth function parameter. */
typedef struct ta_python_encoder {
    ta_cbor_writer *writer;
    ta_python_module_state *state;
} ta_python_encoder;

typedef struct ta_python_decoder {
    ta_cbor_reader reader;
    ta_python_module_state *state;
} ta_python_decoder;

static int ta_python_write(ta_python_encoder *encoder, PyObject *value, unsigned depth);

/* Write children of an array already opened by ta_python_write. items must
 * be that call's private list snapshot, held alive by the caller, not the
 * original mutable input. depth is the children's nesting depth.
 * Return 0 on success or -1 with an exception set; the caller closes the
 * array only on success and discards the writer on failure. */
static int ta_python_write_sequence(ta_python_encoder *encoder, PyObject *items, unsigned depth) {
    Py_ssize_t count = PyList_Size(items);
    for (Py_ssize_t i = 0; i < count; i++) {
        PyObject *item = PyList_GetItemRef(items, i);
        if (item == NULL) return -1;
        int status = ta_python_write(encoder, item, depth);
        Py_DECREF(item);
        if (status < 0) return -1;
    }
    return 0;
}

/* Recursively append one Python value to an initialized writer. This is
 * the private encoder used by tinyactors.encode, not an adapter-only
 * helper. Start at depth 0 for the whole message; recursive calls append
 * inside the parent's open container and increase depth by one.
 *
 * value is borrowed and must remain alive throughout this call. Exact
 * built-in types avoid invoking arbitrary conversion hooks. Each list or
 * dict is snapshotted before visiting its children; that snapshot holds
 * child references, but does not freeze a whole nested value atomically.
 * Shared subtrees are encoded on each occurrence; cycles reach the depth
 * limit and raise. Bytes and text are copied by the writer during the
 * write, so their borrowed views need not survive this call.
 *
 * Return 0 on success; -1 with an exception on conversion or writer error.
 * An error can leave partial output: stop, discard the writer and free it.
 * Do not resume the writer, retry conversion into it, or serialize a
 * payload handle here. In-process actor sends keep their struct/handle
 * path; this codec belongs at actual CBOR boundaries. */
static int ta_python_write(ta_python_encoder *encoder, PyObject *value, unsigned depth) {
    ta_cbor_writer *writer = encoder->writer;
    if (depth > TA_CBOR_DEPTH_MAX) {
        PyErr_SetString(PyExc_ValueError, "CBOR nesting exceeds 64 levels");
        return -1;
    }
    if (value == encoder->state->undefined) ta_cbor_write_simple(writer, TA_CBOR_UNDEFINED);
    else if (Py_IS_TYPE(value, (PyTypeObject *)encoder->state->address_type)) {
        ta_cbor_write_tag(writer, TA_CBOR_TAG_URI);
        if (ta_python_write(encoder, ((ta_python_address *)value)->text, depth + 1) < 0) return -1;
    } else if (value == Py_None) ta_cbor_write_simple(writer, TA_CBOR_NULL);
    else if (PyBool_Check(value)) ta_cbor_write_simple(writer, value == Py_True ? TA_CBOR_TRUE : TA_CBOR_FALSE);
    else if (PyLong_CheckExact(value)) {
        PyObject *zero = PyLong_FromLong(0);
        if (zero == NULL) return -1;
        int negative = PyObject_RichCompareBool(value, zero, Py_LT);
        Py_DECREF(zero);
        if (negative < 0) return -1;
        PyObject *magnitude = negative ? PyNumber_Invert(value) : Py_NewRef(value);
        if (magnitude == NULL) return -1;
        unsigned long long number = PyLong_AsUnsignedLongLong(magnitude);
        Py_DECREF(magnitude);
        if (PyErr_Occurred()) return -1;
        if (negative) ta_cbor_write_negative(writer, number);
        else ta_cbor_write_uint(writer, number);
    } else if (PyFloat_CheckExact(value)) ta_cbor_write_float(writer, PyFloat_AsDouble(value));
    else if (PyUnicode_CheckExact(value)) {
        Py_ssize_t size;
        const char *text = PyUnicode_AsUTF8AndSize(value, &size);
        if (text == NULL) return -1;
        if ((size_t)size > UINT32_MAX) {
            PyErr_SetString(PyExc_OverflowError, "CBOR text exceeds the core string length limit");
            return -1;
        }
        ta_cbor_write_text(writer, (ta_string_view){text, (uint32_t)size});
    } else if (PyBytes_CheckExact(value)) {
        char *bytes;
        Py_ssize_t size;
        if (PyBytes_AsStringAndSize(value, &bytes, &size) < 0) return -1;
        ta_cbor_write_bytes(writer, (ta_bytes){bytes, (size_t)size});
    } else if (PyList_CheckExact(value)) {
        PyObject *items = PySequence_List(value);
        if (items == NULL) return -1;
        ta_cbor_write_array(writer, TA_CBOR_COUNT_LATER);
        int status = ta_python_write_sequence(encoder, items, depth + 1);
        Py_DECREF(items);
        if (status < 0) return -1;
        ta_cbor_write_end(writer);
    } else if (PyDict_CheckExact(value)) {
        PyObject *pairs = PyDict_Items(value);
        if (pairs == NULL) return -1;
        ta_cbor_write_map(writer, TA_CBOR_COUNT_LATER);
        for (Py_ssize_t i = 0; i < PyList_Size(pairs); i++) {
            PyObject *pair = PyList_GetItem(pairs, i);
            PyObject *key = PyTuple_GetItem(pair, 0);
            if (!PyUnicode_CheckExact(key)) {
                PyErr_SetString(PyExc_TypeError, "CBOR map keys must be str");
                Py_DECREF(pairs);
                return -1;
            }
            if (ta_python_write(encoder, key, depth + 1) < 0 ||
                ta_python_write(encoder, PyTuple_GetItem(pair, 1), depth + 1) < 0) {
                Py_DECREF(pairs);
                return -1;
            }
        }
        Py_DECREF(pairs);
        ta_cbor_write_end(writer);
    } else {
        PyErr_SetString(PyExc_TypeError, "unsupported Python value for CBOR");
        return -1;
    }
    if (writer->status != TA_OK) {
        if (writer->status == TA_ERROR_NO_MEMORY) PyErr_NoMemory();
        else PyErr_SetString(PyExc_ValueError, "invalid CBOR value");
        return -1;
    }
    return 0;
}

/* Public METH_O adapter: see extension.h and encode's Python help text.
 * Copy the finished writer view into independent Python bytes before
 * freeing the writer. Both success and failure release the raw block. */
int ta_python_value_write(ta_python_module_state *state, ta_cbor_writer *writer, PyObject *value) {
    ta_python_encoder encoder = {writer, state};
    return ta_python_write(&encoder, value, 0);
}

PyObject *ta_python_encode(PyObject *module, PyObject *value) {
    ta_cbor_writer writer;
    ta_python_encoder encoder = {&writer, ta_python_codec_state(module)};
    if (encoder.state == NULL) return NULL;
    unsigned char room[256];
    ta_python_cbor_init(&writer, &(ta_python_room){{room, sizeof room, 0}, NULL});
    PyObject *result = NULL;
    if (ta_python_write(&encoder, value, 0) == 0) {
        ta_bytes bytes;
        if (ta_cbor_writer_bytes(&writer, &bytes) == TA_OK)
            result = PyBytes_FromStringAndSize(bytes.pointer, (Py_ssize_t)bytes.length);
        else PyErr_SetString(PyExc_ValueError, "incomplete CBOR value");
    }
    ta_cbor_writer_free(&writer);
    return result;
}

/* Consume one item, including its children, from a call-local reader.
 * The reader's immutable input bytes must remain alive throughout all
 * recursive calls. Return a new Python reference, or NULL with an
 * exception. Strings/bytes are copied; containers own inserted children.
 * Partial containers are released on error.
 *
 * This helper can read a child of an open array/map, so it does not check
 * for trailing bytes. The top-level ta_python_decode must do that after
 * its one successful read. On failure abandon the reader, whose cursor
 * may already have advanced; do not use it to recover a frame boundary.
 * Core validates syntax and wire nesting; this helper rejects wire types
 * the bootstrap has no Python mapping for. */
static PyObject *ta_python_read(ta_python_decoder *decoder) {
    ta_cbor_reader *reader = &decoder->reader;
    ta_cbor_item item;
    if (ta_cbor_read(reader, &item) != 1) {
        PyErr_SetString(PyExc_ValueError, "malformed or incomplete CBOR");
        return NULL;
    }
    switch (item.kind) {
    case TA_CBOR_UINT: return PyLong_FromUnsignedLongLong(item.value);
    case TA_CBOR_NEGINT: {
        PyObject *magnitude = PyLong_FromUnsignedLongLong(item.value);
        if (magnitude == NULL) return NULL;
        PyObject *result = PyNumber_Invert(magnitude);
        Py_DECREF(magnitude);
        return result;
    }
    case TA_CBOR_FLOAT: {
        double number;
        memcpy(&number, &item.value, sizeof(number));
        return PyFloat_FromDouble(number);
    }
    case TA_CBOR_BYTES: return PyBytes_FromStringAndSize(item.bytes.pointer, (Py_ssize_t)item.bytes.length);
    case TA_CBOR_TEXT: return PyUnicode_DecodeUTF8(item.bytes.pointer, (Py_ssize_t)item.bytes.length, "strict");
    case TA_CBOR_TAG: {
        /* A reference is exactly tag 32 followed by text, not a generic
         * tagged Python value. Read the child directly so nested tags or
         * other child kinds cannot be normalized into an Address. */
        ta_cbor_item child;
        if (item.value != TA_CBOR_TAG_URI || ta_cbor_read(reader, &child) != 1 || child.kind != TA_CBOR_TEXT) {
            PyErr_SetString(PyExc_ValueError, "actor references require tag 32 on text");
            return NULL;
        }
        PyObject *text = PyUnicode_DecodeUTF8(child.bytes.pointer, (Py_ssize_t)child.bytes.length, "strict");
        if (text == NULL) return NULL;
        PyObject *result = ta_python_address_from_text(decoder->state, text);
        Py_DECREF(text);
        return result;
    }
    case TA_CBOR_SIMPLE:
        if (item.value == TA_CBOR_UNDEFINED) return Py_NewRef(decoder->state->undefined);
        if (item.value == TA_CBOR_NULL) return Py_NewRef(Py_None);
        if (item.value == TA_CBOR_FALSE || item.value == TA_CBOR_TRUE)
            return PyBool_FromLong(item.value == TA_CBOR_TRUE);
        break;
    case TA_CBOR_ARRAY: {
        PyObject *result = PyList_New(0);
        if (result == NULL) return NULL;
        for (uint32_t i = 0; i < item.count; i++) {
            PyObject *value = ta_python_read(decoder);
            if (value == NULL) { Py_DECREF(result); return NULL; }
            int status = PyList_Append(result, value);
            Py_DECREF(value);
            if (status < 0) { Py_DECREF(result); return NULL; }
        }
        return result;
    }
    case TA_CBOR_MAP: {
        PyObject *result = PyDict_New();
        if (result == NULL) return NULL;
        for (uint32_t i = 0; i < item.count; i++) {
            PyObject *key = ta_python_read(decoder);
            if (key == NULL) { Py_DECREF(result); return NULL; }
            if (!PyUnicode_CheckExact(key)) {
                PyErr_SetString(PyExc_ValueError, "CBOR map keys must be text");
                Py_DECREF(key); Py_DECREF(result); return NULL;
            }
            PyObject *value = ta_python_read(decoder);
            int status = value == NULL ? -1 : PyDict_SetItem(result, key, value);
            Py_DECREF(key);
            Py_XDECREF(value);
            if (status < 0) { Py_DECREF(result); return NULL; }
        }
        return result;
    }
    }
    PyErr_SetString(PyExc_ValueError, "unsupported CBOR tag or simple value");
    return NULL;
}

/* Public METH_O adapter: see extension.h and decode's Python help text.
 * The Python call holds value alive while the reader borrows its bytes.
 * Requiring immutable bytes also avoids retaining a view of mutable input.
 * Only return a decoded object once core confirms the message is exhausted. */
PyObject *ta_python_value_read(ta_python_module_state *state, ta_bytes cbor) {
    ta_python_decoder decoder = {.state = state};
    ta_cbor_reader_init(&decoder.reader, cbor);
    PyObject *result = ta_python_read(&decoder);
    if (result != NULL) {
        ta_cbor_item extra;
        if (ta_cbor_read(&decoder.reader, &extra) != 0) {
            Py_DECREF(result);
            result = NULL;
            PyErr_SetString(PyExc_ValueError, "CBOR input must contain exactly one item");
        }
    }
    return result;
}

/* A copy for another actor (bindings.md, section 8: actors share no
 * mutable value): lists and dicts copied, the immutable values of the
 * table shared, anything else refused. Exact types only, so no user code
 * runs. */
static PyObject *ta_python_copy(ta_python_module_state *state, PyObject *value, unsigned depth) {
    if (depth > TA_CBOR_DEPTH_MAX) {
        PyErr_SetString(PyExc_ValueError, "nesting exceeds 64 levels");
        return NULL;
    }
    if (value == Py_None || PyBool_Check(value) || PyLong_CheckExact(value) || PyFloat_CheckExact(value) ||
        PyUnicode_CheckExact(value) || PyBytes_CheckExact(value) || value == state->undefined ||
        Py_IS_TYPE(value, (PyTypeObject *)state->address_type))
        return Py_NewRef(value);
    if (PyList_CheckExact(value)) {
        PyObject *items = PySequence_List(value);
        if (!items) return NULL;
        for (Py_ssize_t i = 0; i < PyList_Size(items); i++) {
            PyObject *copy = ta_python_copy(state, PyList_GetItem(items, i), depth + 1);
            if (!copy) {
                Py_DECREF(items);
                return NULL;
            }
            PyList_SetItem(items, i, copy);
        }
        return items;
    }
    if (PyDict_CheckExact(value)) {
        PyObject *result = PyDict_New(), *pairs = result ? PyDict_Items(value) : NULL;
        for (Py_ssize_t i = 0; pairs && i < PyList_Size(pairs); i++) {
            PyObject *pair = PyList_GetItem(pairs, i), *key = PyTuple_GetItem(pair, 0);
            PyObject *copy = PyUnicode_CheckExact(key) ? ta_python_copy(state, PyTuple_GetItem(pair, 1), depth + 1) : NULL;
            if (!copy && !PyErr_Occurred()) PyErr_SetString(PyExc_TypeError, "a map's keys are str");
            if (!copy || PyDict_SetItem(result, key, copy) < 0) Py_CLEAR(result);
            Py_XDECREF(copy);
            if (!result) break;
        }
        Py_XDECREF(pairs);
        if (!pairs) Py_CLEAR(result);
        return result;
    }
    PyErr_Format(PyExc_TypeError, "a value crossing between actors is of the value table (docs/values.md), not %R",
                 (PyObject *)Py_TYPE(value));
    return NULL;
}

PyObject *ta_python_value_copy(ta_python_module_state *state, PyObject *value) {
    return ta_python_copy(state, value, 0);
}

PyObject *ta_python_decode(PyObject *module, PyObject *value) {
    ta_python_decoder decoder = {.state = ta_python_codec_state(module)};
    if (decoder.state == NULL) return NULL;
    if (!PyBytes_CheckExact(value)) {
        PyErr_SetString(PyExc_TypeError, "decode requires bytes in the bootstrap codec");
        return NULL;
    }
    char *bytes;
    Py_ssize_t size;
    if (PyBytes_AsStringAndSize(value, &bytes, &size) < 0) return NULL;
    ta_cbor_reader_init(&decoder.reader, (ta_bytes){bytes, (size_t)size});
    PyObject *result = ta_python_read(&decoder);
    if (result != NULL) {
        ta_cbor_item extra;
        if (ta_cbor_read(&decoder.reader, &extra) != 0) {
            Py_DECREF(result);
            result = NULL;
            PyErr_SetString(PyExc_ValueError, "CBOR input must contain exactly one item");
        }
    }
    return result;
}
