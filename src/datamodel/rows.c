/* The "python" datamodel's tables (AGENTS.md, "The binding's memory"):
 * charts, sessions, payloads and <foreach> loops, each core's ta_table
 * grown through the host's allocator and freed with the host. A row holds
 * Python references the collector sees (ta_python_rows_traverse). The last
 * reference of a payload, and every reference a removed row held, is
 * dropped later, at a flush after the outer call into core returns:
 * dropping it can run Python code (bindings.md, section 8). */
#include "datamodel.h"

static void ta_python_table(ta_python_datamodel *datamodel, ta_table *table, uint32_t row_size) {
    ta_table_config config = {sizeof config, row_size, &datamodel->host->allocator};
    ta_table_init(table, &config);
}

int ta_python_rows_init(ta_python_datamodel *datamodel) {
    ta_python_table(datamodel, &datamodel->charts, sizeof(ta_python_chart_row));
    ta_python_table(datamodel, &datamodel->sessions, sizeof(ta_python_session_row));
    ta_python_table(datamodel, &datamodel->payloads, sizeof(ta_python_payload_row));
    ta_python_table(datamodel, &datamodel->loops, sizeof(ta_python_loop_row));
    datamodel->drops = PyList_New(0);
    return datamodel->drops ? 0 : -1;
}

void ta_python_drop(ta_python_datamodel *datamodel, PyObject *object) {
    if (!object) return;
    if (!datamodel->drops || PyList_Append(datamodel->drops, object) < 0) PyErr_Clear(); /* then it goes now */
    Py_DECREF(object);
}

void ta_python_datamodel_flush(ta_python_datamodel *datamodel) {
    if (!datamodel->drops || PyList_Size(datamodel->drops) == 0) return;
    PyObject *raised = PyErr_GetRaisedException(); /* a finalizer does not replace what is being raised */
    PyList_SetSlice(datamodel->drops, 0, PyList_Size(datamodel->drops), NULL);
    PyErr_Clear();
    PyErr_SetRaisedException(raised);
}

/* Every row's references, then the tables: after ta_host_free, which
 * called destroy and release for what it held. */
void ta_python_rows_free(ta_python_datamodel *datamodel) {
    for (uint32_t slot = 0; slot < datamodel->charts.capacity; slot++) {
        ta_python_chart_row *row = ta_table_at(&datamodel->charts, slot);
        if (!row) continue;
        ta_python_drop(datamodel, row->chart);
        ta_python_drop(datamodel, row->functions);
        ta_python_drop(datamodel, row->data);
        ta_python_drop(datamodel, row->names);
        ta_python_drop(datamodel, row->invokeids);
        ta_python_drop(datamodel, row->origins);
    }
    for (uint32_t slot = 0; slot < datamodel->sessions.capacity; slot++) {
        ta_python_session_row *row = ta_table_at(&datamodel->sessions, slot);
        if (!row) continue;
        ta_python_drop(datamodel, row->instance);
        ta_python_drop(datamodel, row->extra);
    }
    for (uint32_t slot = 0; slot < datamodel->payloads.capacity; slot++) {
        ta_python_payload_row *row = ta_table_at(&datamodel->payloads, slot);
        if (row) ta_python_drop(datamodel, row->value);
    }
    for (uint32_t slot = 0; slot < datamodel->loops.capacity; slot++) {
        ta_python_loop_row *row = ta_table_at(&datamodel->loops, slot);
        if (row) ta_python_drop(datamodel, row->items);
    }
    ta_table_free(&datamodel->charts);
    ta_table_free(&datamodel->sessions);
    ta_table_free(&datamodel->payloads);
    ta_table_free(&datamodel->loops);
    ta_python_datamodel_flush(datamodel);
    Py_CLEAR(datamodel->drops);
}

int ta_python_rows_traverse(ta_python_datamodel *datamodel, visitproc visit, void *arg) {
    Py_VISIT(datamodel->drops);
    for (uint32_t slot = 0; slot < datamodel->charts.capacity; slot++) {
        ta_python_chart_row *row = ta_table_at(&datamodel->charts, slot);
        if (!row) continue;
        Py_VISIT(row->chart);
        Py_VISIT(row->functions);
        Py_VISIT(row->data);
        Py_VISIT(row->names);
        Py_VISIT(row->invokeids);
        Py_VISIT(row->origins);
    }
    for (uint32_t slot = 0; slot < datamodel->sessions.capacity; slot++) {
        ta_python_session_row *row = ta_table_at(&datamodel->sessions, slot);
        if (!row) continue;
        Py_VISIT(row->instance);
        Py_VISIT(row->extra);
    }
    for (uint32_t slot = 0; slot < datamodel->payloads.capacity; slot++) {
        ta_python_payload_row *row = ta_table_at(&datamodel->payloads, slot);
        if (row) Py_VISIT(row->value);
    }
    for (uint32_t slot = 0; slot < datamodel->loops.capacity; slot++) {
        ta_python_loop_row *row = ta_table_at(&datamodel->loops, slot);
        if (row) Py_VISIT(row->items);
    }
    return 0;
}

/* ---- payloads ------------------------------------------------------------------- */

int ta_python_payload_add(ta_python_datamodel *datamodel, PyObject *value, ta_payload *out) {
    uint64_t handle = 0;
    ta_python_payload_row *row = ta_table_add(&datamodel->payloads, &handle);
    if (!row) {
        Py_DECREF(value);
        PyErr_NoMemory();
        return -1;
    }
    row->value = value;
    row->references = 1;
    *out = handle;
    return 0;
}

int ta_python_payload_new(ta_python_datamodel *datamodel, PyObject *value, ta_payload *out) {
    PyObject *copy = ta_python_value_copy(datamodel->state, value);
    if (!copy) return -1;
    return ta_python_payload_add(datamodel, copy, out);
}

PyObject *ta_python_payload_value(ta_python_datamodel *datamodel, ta_payload payload) {
    ta_python_payload_row *row = ta_table_get(&datamodel->payloads, payload);
    return row ? row->value : NULL;
}

void ta_python_payload_retain(ta_python_datamodel *datamodel, ta_payload payload) {
    ta_python_payload_row *row = ta_table_get(&datamodel->payloads, payload);
    if (row) row->references++;
}

void ta_python_payload_release(ta_python_datamodel *datamodel, ta_payload payload) {
    ta_python_payload_row *row = ta_table_get(&datamodel->payloads, payload);
    if (!row || --row->references > 0) return;
    PyObject *value = row->value;
    ta_table_remove(&datamodel->payloads, payload);
    ta_python_drop(datamodel, value); /* later: it can run Python code */
}
