/*
 * pyarena_alloc — per-interpreter memory quota via PyMem_SetAllocator.
 *
 * Part of the PyArena Unit 3 implementation. This file lives inside the
 * CPython fork checkout (/workspaces/PyArena/CPython/pyarena_alloc/) so
 * that Python.h and pyconfig.h from the locally-built interpreter are
 * directly reachable. The PyArena repository itself does not track any
 * .c source — this whole subdirectory has its own independent .git and
 * is further excluded from the CPython fork's tracking via
 * .git/info/exclude, keeping all three git histories clean.
 *
 * Design (docs/pyarena_기술_설계.md, pyarena_기술_선택과_이유.md):
 *
 *   - Hook the Object domain allocator only.
 *       Raw domain is called without the GIL → PyInterpreterState_Get()
 *       can be NULL or unsafe there → hooking it risks crashes.
 *       Mem domain has low traffic and limited attack surface.
 *       Object domain carries ~all Python object allocation → full
 *       coverage of the attacks we care about (single huge alloc like
 *       "a" * 10**9 goes straight through Object domain).
 *
 *   - 16-byte allocation header `{uint64_t size; uint64_t interp_id;}`
 *     prefixed to every tracked allocation. On free(ptr) we look at
 *     ptr - 16 to recover size and interpreter id so we can decrement
 *     the right quota counter. 16 bytes satisfies 64-bit alignment on
 *     all supported platforms.
 *
 *   - Quota check + increment uses a CAS loop: fetch_add alone is not
 *     check-then-add atomic — two threads could race past the limit.
 *
 *   - Host interpreter is exempt: when the lookup finds no quota entry
 *     for the calling interpreter, allocation passes straight through
 *     to the underlying allocator without a header.
 *
 *   - install() must run before any subinterpreter is created. The hook
 *     leaves pre-install allocations untouched via a magic-number check
 *     in the header: if the magic field is wrong, this is not our
 *     memory (pre-install or leaked through another path) and we
 *     pass-through to the real allocator without touching counters.
 *
 * ----------------------------------------------------------------------
 * CHECKPOINT STATUS — CP2 (quota table + bindings, hook NOT yet installed)
 * ----------------------------------------------------------------------
 *
 *   CP1 — [done] module skeleton that loads.
 *   CP2 — [this file] in-process quota table + Python bindings.
 *         install() is a no-op that reserves the API shape; set_quota /
 *         clear_quota / get_used / get_peak operate on a host-side
 *         table only — nothing hooks into the Python allocator yet, so
 *         `used` stays at 0 until CP3 adds the real interceptor.
 *   CP3 — allocator hook function + real install() that flips the
 *         allocator via PyMem_SetAllocator. Header + host pass-through.
 *   CP4 — CAS loop quota enforcement + single large-alloc -> NULL ->
 *         MemoryError test path.
 * ----------------------------------------------------------------------
 */

#define PY_SSIZE_T_CLEAN
#include <Python.h>

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Quota table                                                         */
/* ------------------------------------------------------------------ */

/*
 * Fixed-size linear-probe table indexed only by scan order. The worker
 * pool in PyArena tops out at the physical core count (~14 on this
 * machine), so 64 slots leaves plenty of headroom. Linear scan is
 * cache-friendly at this scale and avoids the complexity of a hash.
 *
 * `active` / `interp_id` / `limit` are mutated only from Python-callable
 * entry points (set_quota / clear_quota), all of which hold the GIL, so
 * those fields need no atomics.
 *
 * `used` and `peak` will be touched by the allocator hook in CP3, which
 * may run from any subinterpreter's thread state concurrently with
 * Python entry points in other interpreters. Those need atomics —
 * `used` uses CAS, `peak` uses a relaxed max-update.
 */
#define PYARENA_MAX_QUOTA_ENTRIES 64

typedef struct {
    bool           active;
    int64_t        interp_id;
    uint64_t       limit;
    _Atomic uint64_t used;
    _Atomic uint64_t peak;
} QuotaEntry;

static QuotaEntry g_quotas[PYARENA_MAX_QUOTA_ENTRIES];

/* Whether install() has been called. CP2 semantics: just a flag.
 * CP3 will actually swap the allocator via PyMem_SetAllocator. */
static bool g_installed = false;

/* ------------------------------------------------------------------ */
/* Table helpers (GIL-protected)                                      */
/* ------------------------------------------------------------------ */

static QuotaEntry *
find_entry(int64_t interp_id)
{
    for (int i = 0; i < PYARENA_MAX_QUOTA_ENTRIES; i++) {
        if (g_quotas[i].active && g_quotas[i].interp_id == interp_id) {
            return &g_quotas[i];
        }
    }
    return NULL;
}

static QuotaEntry *
acquire_entry(int64_t interp_id)
{
    QuotaEntry *e = find_entry(interp_id);
    if (e != NULL) {
        return e;
    }
    for (int i = 0; i < PYARENA_MAX_QUOTA_ENTRIES; i++) {
        if (!g_quotas[i].active) {
            g_quotas[i].active    = true;
            g_quotas[i].interp_id = interp_id;
            g_quotas[i].limit     = 0;
            atomic_store(&g_quotas[i].used, 0);
            atomic_store(&g_quotas[i].peak, 0);
            return &g_quotas[i];
        }
    }
    return NULL;
}

static void
release_entry(QuotaEntry *e)
{
    /* Zero the fields to avoid stale reads if the slot is re-used. */
    e->interp_id = 0;
    e->limit     = 0;
    atomic_store(&e->used, 0);
    atomic_store(&e->peak, 0);
    e->active = false;
}

/* ------------------------------------------------------------------ */
/* Python bindings                                                     */
/* ------------------------------------------------------------------ */

PyDoc_STRVAR(install_doc,
"install() -> None\n\n"
"Install the allocator hook. Idempotent.\n\n"
"CP2 status: no-op placeholder that only flips an internal flag. CP3 "
"wires in the actual PyMem_SetAllocator swap.");

static PyObject *
py_install(PyObject *self, PyObject *args)
{
    (void)self;
    (void)args;
    g_installed = true;
    Py_RETURN_NONE;
}

PyDoc_STRVAR(set_quota_doc,
"set_quota(interp_id, bytes) -> None\n\n"
"Set or update the quota (in bytes) for the given subinterpreter id. "
"Allocates a table slot on first use. Raises RuntimeError if the table "
"is full.");

static PyObject *
py_set_quota(PyObject *self, PyObject *args)
{
    (void)self;
    long long interp_id;
    unsigned long long bytes;
    if (!PyArg_ParseTuple(args, "LK", &interp_id, &bytes)) {
        return NULL;
    }
    QuotaEntry *e = acquire_entry((int64_t)interp_id);
    if (e == NULL) {
        PyErr_Format(PyExc_RuntimeError,
                     "pyarena_alloc: quota table full (max %d entries)",
                     PYARENA_MAX_QUOTA_ENTRIES);
        return NULL;
    }
    e->limit = (uint64_t)bytes;
    Py_RETURN_NONE;
}

PyDoc_STRVAR(clear_quota_doc,
"clear_quota(interp_id) -> None\n\n"
"Remove any quota set for the given subinterpreter id. After this call "
"the interpreter is treated like the host — allocations pass through "
"without counting. Silent no-op if no entry exists.");

static PyObject *
py_clear_quota(PyObject *self, PyObject *args)
{
    (void)self;
    long long interp_id;
    if (!PyArg_ParseTuple(args, "L", &interp_id)) {
        return NULL;
    }
    QuotaEntry *e = find_entry((int64_t)interp_id);
    if (e != NULL) {
        release_entry(e);
    }
    Py_RETURN_NONE;
}

PyDoc_STRVAR(get_used_doc,
"get_used(interp_id) -> int\n\n"
"Return the current bytes-used counter for the given subinterpreter. "
"Returns 0 if no quota is set (entry absent).");

static PyObject *
py_get_used(PyObject *self, PyObject *args)
{
    (void)self;
    long long interp_id;
    if (!PyArg_ParseTuple(args, "L", &interp_id)) {
        return NULL;
    }
    QuotaEntry *e = find_entry((int64_t)interp_id);
    if (e == NULL) {
        return PyLong_FromLong(0);
    }
    return PyLong_FromUnsignedLongLong(atomic_load(&e->used));
}

PyDoc_STRVAR(get_peak_doc,
"get_peak(interp_id) -> int\n\n"
"Return the peak bytes-used value seen by the tracker for the given "
"subinterpreter. Returns 0 if no quota is set (entry absent).");

static PyObject *
py_get_peak(PyObject *self, PyObject *args)
{
    (void)self;
    long long interp_id;
    if (!PyArg_ParseTuple(args, "L", &interp_id)) {
        return NULL;
    }
    QuotaEntry *e = find_entry((int64_t)interp_id);
    if (e == NULL) {
        return PyLong_FromLong(0);
    }
    return PyLong_FromUnsignedLongLong(atomic_load(&e->peak));
}

PyDoc_STRVAR(max_entries_doc,
"max_entries() -> int\n\n"
"Return the compiled-in table capacity. Testing helper — lets the test "
"suite scale up to the exact edge without hard-coding the constant.");

static PyObject *
py_max_entries(PyObject *self, PyObject *args)
{
    (void)self;
    (void)args;
    return PyLong_FromLong(PYARENA_MAX_QUOTA_ENTRIES);
}

/* ------------------------------------------------------------------ */
/* Module definition                                                   */
/* ------------------------------------------------------------------ */

static PyMethodDef pyarena_alloc_methods[] = {
    {"install",     py_install,     METH_NOARGS,  install_doc},
    {"set_quota",   py_set_quota,   METH_VARARGS, set_quota_doc},
    {"clear_quota", py_clear_quota, METH_VARARGS, clear_quota_doc},
    {"get_used",    py_get_used,    METH_VARARGS, get_used_doc},
    {"get_peak",    py_get_peak,    METH_VARARGS, get_peak_doc},
    {"max_entries", py_max_entries, METH_NOARGS,  max_entries_doc},
    {NULL, NULL, 0, NULL}, /* sentinel */
};

static struct PyModuleDef pyarena_alloc_module = {
    PyModuleDef_HEAD_INIT,
    .m_name    = "pyarena_alloc",
    .m_doc     = "Per-interpreter memory quota via PyMem_SetAllocator hook.",
    .m_size    = -1,
    .m_methods = pyarena_alloc_methods,
};

PyMODINIT_FUNC
PyInit_pyarena_alloc(void)
{
    /* Zero the table on first load. The BSS zero-initialization already
     * does this, but being explicit documents intent for future edits. */
    memset(g_quotas, 0, sizeof(g_quotas));
    return PyModule_Create(&pyarena_alloc_module);
}
