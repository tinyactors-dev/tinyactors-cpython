/* tinyactors.decode_traces(data): the spans of an ExportTraceServiceRequest
 * (the bytes a telemetry handler gets for "traces", or any OTLP exporter's)
 * as Python values, with no host. Core reads the request
 * (ta_otlp_next_span and its walkers, every view into the bytes given);
 * this writes each span as one CBOR document through a writer, and the
 * value table reads it back (ta_python_value_read), so a span's values are
 * the ones a typed method's reply would give. Each span is a dict:
 * trace, span, parent (lowercase hex; parent None for a root span), name,
 * kind, start, end (nanoseconds since the Unix epoch), status,
 * statusMessage, flags, attributes, events [{name, time, attributes}],
 * links [{trace, span, attributes}], resource (its attributes) and scope
 * {name, version, attributes}; an attribute's array or key-value list is
 * a list or a dict, to the value table's depth. */
#include "extension.h"
#include "docs.h"

/* An attribute value nests no deeper than this; deeper ones are None. */
#define TA_PYTHON_TRACES_DEPTH 32u

typedef struct ta_python_traces {
    ta_cbor_writer writer;
} ta_python_traces;

static void ta_python_traces_key(ta_python_traces *out, const char *key) {
    ta_cbor_write_text(&out->writer, ta_string(key));
}

static void ta_python_traces_id(ta_python_traces *out, ta_trace_id trace) {
    char text[TA_TRACE_ID_TEXT_LENGTH];
    ta_buffer buffer = {text, sizeof text, 0};
    ta_trace_id_format(trace, &buffer);
    ta_cbor_write_text(&out->writer, (ta_string_view){text, (uint32_t)buffer.length});
}

static void ta_python_traces_span_id(ta_python_traces *out, ta_span_id span) {
    char text[16];
    ta_buffer buffer = {text, sizeof text, 0};
    ta_span_id_format(span, &buffer);
    ta_cbor_write_text(&out->writer, (ta_string_view){text, (uint32_t)buffer.length});
}

static void ta_python_traces_attributes(ta_python_traces *out, ta_otlp_fields attributes, uint32_t depth);

static void ta_python_traces_value(ta_python_traces *out, const ta_otlp_value *value, uint32_t depth) {
    ta_cbor_writer *writer = &out->writer;
    switch (value->kind) {
    case TA_OTLP_VALUE_STRING: ta_cbor_write_text(writer, value->bytes); return;
    case TA_OTLP_VALUE_BOOL: ta_cbor_write_simple(writer, value->integer ? 21u : 20u); return;
    case TA_OTLP_VALUE_INT: ta_cbor_write_int(writer, value->integer); return;
    case TA_OTLP_VALUE_DOUBLE: ta_cbor_write_float(writer, value->number); return;
    case TA_OTLP_VALUE_BYTES: ta_cbor_write_bytes(writer, (ta_bytes){value->bytes.pointer, value->bytes.length}); return;
    default: break;
    }
    if (depth >= TA_PYTHON_TRACES_DEPTH || (value->kind != TA_OTLP_VALUE_ARRAY && value->kind != TA_OTLP_VALUE_LIST)) {
        ta_cbor_write_simple(writer, 22u); /* null */
        return;
    }
    if (value->kind == TA_OTLP_VALUE_LIST) {
        ta_python_traces_attributes(out, value->items, depth + 1);
        return;
    }
    ta_otlp_value item;
    ta_cbor_write_array(writer, TA_CBOR_COUNT_LATER);
    for (uint32_t at = 0; ta_otlp_next_value(value->items, &at, &item) == 1;)
        ta_python_traces_value(out, &item, depth + 1);
    ta_cbor_write_end(writer);
}

static void ta_python_traces_attributes(ta_python_traces *out, ta_otlp_fields attributes, uint32_t depth) {
    ta_otlp_key_value pair;
    ta_cbor_write_map(&out->writer, TA_CBOR_COUNT_LATER);
    for (uint32_t at = 0; ta_otlp_next_attribute(attributes, &at, &pair) == 1;) {
        ta_cbor_write_text(&out->writer, pair.key);
        ta_python_traces_value(out, &pair.value, depth);
    }
    ta_cbor_write_end(&out->writer);
}

static void ta_python_traces_events(ta_python_traces *out, ta_otlp_fields events) {
    ta_otlp_event event;
    ta_cbor_write_array(&out->writer, TA_CBOR_COUNT_LATER);
    for (uint32_t at = 0; ta_otlp_next_event(events, &at, &event) == 1;) {
        ta_cbor_write_map(&out->writer, 3);
        ta_python_traces_key(out, "name");
        ta_cbor_write_text(&out->writer, event.name);
        ta_python_traces_key(out, "time");
        ta_cbor_write_uint(&out->writer, event.time);
        ta_python_traces_key(out, "attributes");
        ta_python_traces_attributes(out, event.attributes, 0);
    }
    ta_cbor_write_end(&out->writer);
}

static void ta_python_traces_links(ta_python_traces *out, ta_otlp_fields links) {
    ta_otlp_link link;
    ta_cbor_write_array(&out->writer, TA_CBOR_COUNT_LATER);
    for (uint32_t at = 0; ta_otlp_next_link(links, &at, &link) == 1;) {
        ta_cbor_write_map(&out->writer, 3);
        ta_python_traces_key(out, "trace");
        ta_python_traces_id(out, link.trace);
        ta_python_traces_key(out, "span");
        ta_python_traces_span_id(out, link.span);
        ta_python_traces_key(out, "attributes");
        ta_python_traces_attributes(out, link.attributes, 0);
    }
    ta_cbor_write_end(&out->writer);
}

static void ta_python_traces_span(ta_python_traces *out, const ta_otlp_span *span) {
    ta_cbor_writer *writer = &out->writer;
    ta_cbor_write_map(writer, 15);
    ta_python_traces_key(out, "trace");
    ta_python_traces_id(out, span->trace);
    ta_python_traces_key(out, "span");
    ta_python_traces_span_id(out, span->span);
    ta_python_traces_key(out, "parent");
    if (span->parent) ta_python_traces_span_id(out, span->parent);
    else ta_cbor_write_simple(writer, 22u);
    ta_python_traces_key(out, "name");
    ta_cbor_write_text(writer, span->name);
    ta_python_traces_key(out, "kind");
    ta_cbor_write_uint(writer, span->kind);
    ta_python_traces_key(out, "start");
    ta_cbor_write_uint(writer, span->start);
    ta_python_traces_key(out, "end");
    ta_cbor_write_uint(writer, span->end);
    ta_python_traces_key(out, "status");
    ta_cbor_write_uint(writer, span->status);
    ta_python_traces_key(out, "statusMessage");
    ta_cbor_write_text(writer, span->status_message);
    ta_python_traces_key(out, "flags");
    ta_cbor_write_uint(writer, span->flags);
    ta_python_traces_key(out, "attributes");
    ta_python_traces_attributes(out, span->attributes, 0);
    ta_python_traces_key(out, "events");
    ta_python_traces_events(out, span->events);
    ta_python_traces_key(out, "links");
    ta_python_traces_links(out, span->links);
    ta_python_traces_key(out, "resource");
    ta_python_traces_attributes(out, span->resource, 0);
    ta_python_traces_key(out, "scope");
    ta_cbor_write_map(writer, 3);
    ta_python_traces_key(out, "name");
    ta_cbor_write_text(writer, span->scope_name);
    ta_python_traces_key(out, "version");
    ta_cbor_write_text(writer, span->scope_version);
    ta_python_traces_key(out, "attributes");
    ta_python_traces_attributes(out, span->scope_attributes, 0);
}

/* One span as a dict: written, read back, the writer reset for the next. */
static PyObject *ta_python_traces_one(ta_python_module_state *state, const ta_otlp_span *span) {
    ta_python_traces out;
    ta_python_cbor_init(&out.writer, &(ta_python_room){{NULL, 0, 0}, NULL});
    ta_python_traces_span(&out, span);
    ta_bytes bytes;
    ta_status status = ta_cbor_writer_bytes(&out.writer, &bytes);
    PyObject *value = NULL;
    if (status == TA_OK) value = ta_python_value_read(state, bytes);
    else ta_python_raise_status(state, status);
    ta_cbor_writer_free(&out.writer);
    return value;
}

PyObject *ta_python_decode_traces(PyObject *module, PyObject *data) {
    ta_python_module_state *state = ta_python_codec_state(module);
    if (state == NULL) return NULL;
    Py_buffer view;
    if (PyObject_GetBuffer(data, &view, PyBUF_SIMPLE) < 0) return NULL;
    ta_bytes request = {view.buf, (size_t)view.len};
    PyObject *spans = PyList_New(0);
    ta_otlp_cursor cursor = {0};
    ta_otlp_span span;
    int32_t got = 0;
    while (spans && (got = ta_otlp_next_span(request, &cursor, &span)) == 1) {
        PyObject *one = ta_python_traces_one(state, &span);
        if (!one || PyList_Append(spans, one) < 0) Py_CLEAR(spans);
        Py_XDECREF(one);
    }
    PyBuffer_Release(&view);
    if (spans && got != 0) { /* malformed: InvalidError, as TinyactorsError makes it for the status */
        Py_CLEAR(spans);
        PyObject *arguments = Py_BuildValue("(s)", "the data is not an ExportTraceServiceRequest");
        PyObject *keywords = Py_BuildValue("{s:s}", "status", "TA_ERROR_INVALID");
        PyObject *error = arguments && keywords ? PyObject_Call(state->error, arguments, keywords) : NULL;
        if (error) PyErr_SetObject((PyObject *)Py_TYPE(error), error);
        Py_XDECREF(error);
        Py_XDECREF(arguments);
        Py_XDECREF(keywords);
    }
    return spans;
}
