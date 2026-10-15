/* The "python" datamodel's own declarations (AGENTS.md, "The "python"
 * datamodel"): the rows of its tables and what the vtable's entries share.
 * Of core it calls only ta_datamodel_environment_*, ta_image_*, ta_cbor_*
 * and ta_table_* (tools/lint/rules/datamodel-calls-environment-and-python.yml). */
#ifndef TA_PYTHON_DATAMODEL_H
#define TA_PYTHON_DATAMODEL_H

#include "../extension.h"

/* A chart, attached: its class, its compiled expressions by id, its
 * <data> ids, and which system variables its code reads. */
enum { TA_PYTHON_READS_SESSIONID = 1u, TA_PYTHON_READS_NAME = 2u, TA_PYTHON_READS_IOPROCESSORS = 4u };
typedef struct ta_python_chart_row {
    PyObject *chart;     /* the class: an instance per session */
    PyObject *functions; /* tuple: per expression, a function, (getter, setter), or a SyntaxError */
    PyObject *data;      /* tuple of the variables, as str: the <data> ids, then what <foreach> and <script> declare */
    PyObject *names;     /* list by the image's event-name node: the name's str, made at its first
                            event and shared by every session (None until then) */
    PyObject *invokeids; /* lists by the chart's <invoke> slot: the invoke's id and the origin of */
    PyObject *origins;   /* the events from it (#_<id>), made at its first event (None until then) */
    uint32_t segment;    /* the host's segment of its attachment and the chart's index: how a */
    uint32_t index;      /* snapshot's sessions find their chart after a restore attached the charts */
    uint32_t reads;      /* TA_PYTHON_READS_* */
    uint32_t pad;
} ta_python_chart_row;

/* A resident session: its instance (a hibernated one has no row: its
 * state is in the blob). `extra` is the spawn's or invoke's params until
 * the first event, or the blob's values until migrate takes them or an
 * event comes. */
typedef struct ta_python_session_row {
    PyObject *instance;
    uint64_t chart; /* the chart's row */
    PyObject *extra;
} ta_python_session_row;

/* A payload: a value and its references (core's retain and release). */
typedef struct ta_python_payload_row {
    PyObject *value;
    uint32_t references;
    uint32_t pad;
} ta_python_payload_row;

/* A <foreach> in progress: its items, a shallow copy. */
typedef struct ta_python_loop_row {
    PyObject *items;
} ta_python_loop_row;

/* rows.c */
int ta_python_rows_init(ta_python_datamodel *datamodel);
void ta_python_rows_free(ta_python_datamodel *datamodel);
int ta_python_rows_traverse(ta_python_datamodel *datamodel, visitproc visit, void *arg);
/* Defer the drop of an object reference (stolen) to the next flush. */
void ta_python_drop(ta_python_datamodel *datamodel, PyObject *object);
/* A new payload holding `value` (stolen) with one reference. */
int ta_python_payload_add(ta_python_datamodel *datamodel, PyObject *value, ta_payload *out);
void ta_python_payload_retain(ta_python_datamodel *datamodel, ta_payload payload);

/* expressions.c: a chart's class and functions (support.attach). */
typedef struct ta_python_attach {
    const ta_image *image;
    uint32_t chart;
} ta_python_attach;
int ta_python_chart_compile(ta_python_datamodel *datamodel, const ta_python_attach *attach, ta_python_chart_row *row);

/* expressions.c: the globals of a host's chart code. */
int ta_python_namespace_init(ta_python_datamodel *datamodel);

/* datamodel.c: the Event type of _event. */
int ta_python_event_init_type(PyObject *module, ta_python_module_state *state);

#endif
