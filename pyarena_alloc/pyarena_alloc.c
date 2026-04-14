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
 *   - 16-byte allocation header packs both fields with a split magic:
 *       word 0: high 16 bits = SIZE_MAGIC, low 48 bits = payload size
 *       word 1: high 16 bits = INTERP_MAGIC, low 48 bits = interp id
 *     Header stays 16 bytes (design spec) while still carrying a 32-bit
 *     combined magic for distinguishing our allocations from pre-install
 *     ones on free.
 *
 *   - Host interpreter is exempt. If find_entry returns NULL for the
 *     calling interpreter id, the allocation passes straight through to
 *     the original allocator with NO header — the pointer returned is
 *     indistinguishable from an untouched call, and on free we detect
 *     that via the magic check.
 *
 *   - install() must run before any subinterpreter is created. Allocations
 *     made before install() have no header; on free we detect that via
 *     the magic and pass through without touching any counter.
 *
 *   - Table access uses a pthread mutex. The critical section is tiny
 *     (linear scan of 64 entries + counter check/update). The mutex
 *     keeps the hook safe against concurrent set_quota / clear_quota
 *     under per-interpreter GIL (different interpreters, different
 *     threads, no shared GIL). Lock order is always
 *     (GIL → table_lock); no Python APIs are called while holding the
 *     table_lock, so no deadlock risk.
 *
 * ----------------------------------------------------------------------
 * CHECKPOINT STATUS — CP3 (hook installed, counters live)
 * ----------------------------------------------------------------------
 *
 *   CP1 — [done] module skeleton that loads.
 *   CP2 — [done] host-side quota table + bindings (no hook).
 *   CP3 — [this file] real install() that swaps the Object domain
 *         allocator, header-based free routing, host pass-through via
 *         magic check, per-interp counter increment/decrement with
 *         check-before-update under table mutex. After CP3:
 *           - set_quota(interp_id, N) causes allocations from that
 *             interpreter to be tracked; exceeding N returns NULL,
 *             which Python turns into MemoryError.
 *           - get_used / get_peak return live data.
 *           - host and un-quota'd interpreters are unaffected.
 *   CP4 — extensive C-level integration tests (large alloc, cross-
 *         interpreter isolation, host passthrough, peak tracking).
 * ----------------------------------------------------------------------
 */

#define PY_SSIZE_T_CLEAN
#include <Python.h>

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* ==================================================================== */
/* Quota table                                                          */
/* ==================================================================== */

#define PYARENA_MAX_QUOTA_ENTRIES 64

typedef struct {
    bool             active;
    int64_t          interp_id;
    uint64_t         limit;
    _Atomic uint64_t used;
    _Atomic uint64_t peak;
} QuotaEntry;

static QuotaEntry    g_quotas[PYARENA_MAX_QUOTA_ENTRIES];
static pthread_mutex_t g_table_lock = PTHREAD_MUTEX_INITIALIZER;

static bool              g_installed = false;
static PyMemAllocatorEx  g_old_object_allocator;

/* ==================================================================== */
/* Header layout                                                         */
/* ==================================================================== */

/*
 * Header is 16 bytes = two uint64_t. Each word carries 16 high bits of
 * magic and 48 low bits of payload (size or interp_id). Combined magic
 * space is 32 bits → ~1 in 4 billion chance a random byte pattern would
 * match on both words. Size up to 2^48 bytes (280 TiB) and interp_id up
 * to 2^48 both sufficient for realistic use.
 *
 * Alignment: 16 bytes = 2 × 8 = multiple of 16, preserves 16-byte
 * alignment of the payload pointer that pymalloc expects.
 */
#define PAYLOAD_MASK       0x0000FFFFFFFFFFFFULL
#define MAGIC_MASK         0xFFFF000000000000ULL
#define SIZE_MAGIC         0xA11C000000000000ULL  /* "A1 1C" — allocate */
#define INTERP_MAGIC       0xBEEF000000000000ULL  /* "BE EF" — beef */

typedef struct {
    uint64_t size_and_magic;
    uint64_t interp_and_magic;
} Header;

static inline bool
hdr_is_ours(const Header *h)
{
    return (h->size_and_magic   & MAGIC_MASK) == SIZE_MAGIC
        && (h->interp_and_magic & MAGIC_MASK) == INTERP_MAGIC;
}

static inline uint64_t
hdr_size(const Header *h)
{
    return h->size_and_magic & PAYLOAD_MASK;
}

static inline int64_t
hdr_interp(const Header *h)
{
    return (int64_t)(h->interp_and_magic & PAYLOAD_MASK);
}

static inline void
hdr_write(Header *h, uint64_t size, int64_t interp_id)
{
    h->size_and_magic   = SIZE_MAGIC   | (size & PAYLOAD_MASK);
    h->interp_and_magic = INTERP_MAGIC | ((uint64_t)interp_id & PAYLOAD_MASK);
}

/* ==================================================================== */
/* Table helpers (caller holds g_table_lock unless noted)               */
/* ==================================================================== */

static QuotaEntry *
find_entry_locked(int64_t interp_id)
{
    for (int i = 0; i < PYARENA_MAX_QUOTA_ENTRIES; i++) {
        if (g_quotas[i].active && g_quotas[i].interp_id == interp_id) {
            return &g_quotas[i];
        }
    }
    return NULL;
}

static QuotaEntry *
acquire_entry_locked(int64_t interp_id)
{
    QuotaEntry *e = find_entry_locked(interp_id);
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
release_entry_locked(QuotaEntry *e)
{
    e->interp_id = 0;
    e->limit     = 0;
    atomic_store(&e->used, 0);
    atomic_store(&e->peak, 0);
    e->active = false;
}

/* ==================================================================== */
/* Allocator hook functions                                              */
/* ==================================================================== */

/*
 * Common check-and-reserve path. Returns:
 *   1  = tracked: caller should prepend header and use (size + 16) bytes
 *   0  = untracked: caller should pass through to old allocator
 *  -1  = quota exceeded: caller should return NULL (→ MemoryError)
 *
 * The table lock is held across the check/update so the entry cannot be
 * released under us. Caller must not hold the lock on entry.
 */
static int
reserve_quota(int64_t interp_id, size_t size, size_t *out_total)
{
    size_t total = size + sizeof(Header);

    pthread_mutex_lock(&g_table_lock);
    QuotaEntry *e = find_entry_locked(interp_id);
    if (e == NULL) {
        pthread_mutex_unlock(&g_table_lock);
        return 0;  /* host / un-quota'd → pass-through */
    }

    uint64_t cur = atomic_load(&e->used);
    uint64_t next = cur + total;
    if (next > e->limit) {
        pthread_mutex_unlock(&g_table_lock);
        return -1;  /* quota exceeded */
    }
    atomic_store(&e->used, next);

    uint64_t peak = atomic_load(&e->peak);
    if (next > peak) {
        atomic_store(&e->peak, next);
    }
    pthread_mutex_unlock(&g_table_lock);

    *out_total = total;
    return 1;
}

/* Roll back a prior reserve_quota() call — used when the real allocator
 * fails *after* we already reserved. Silently no-ops if the entry has
 * since been released (stale interp id); see release discipline note. */
static void
release_quota(int64_t interp_id, size_t total)
{
    pthread_mutex_lock(&g_table_lock);
    QuotaEntry *e = find_entry_locked(interp_id);
    if (e != NULL) {
        uint64_t cur = atomic_load(&e->used);
        uint64_t next = (cur >= total) ? cur - total : 0;
        atomic_store(&e->used, next);
    }
    pthread_mutex_unlock(&g_table_lock);
}

static void *
pyarena_malloc(void *ctx, size_t size)
{
    (void)ctx;
    PyInterpreterState *is = PyInterpreterState_Get();
    int64_t iid = (is != NULL) ? (int64_t)PyInterpreterState_GetID(is) : -1;

    size_t total = 0;
    int tracked = reserve_quota(iid, size, &total);
    if (tracked == -1) {
        return NULL;  /* → MemoryError */
    }
    if (tracked == 0) {
        /* host / un-quota'd: pass through without header */
        return g_old_object_allocator.malloc(g_old_object_allocator.ctx, size);
    }

    Header *hdr = (Header *)g_old_object_allocator.malloc(
        g_old_object_allocator.ctx, total);
    if (hdr == NULL) {
        /* Real allocator ran out — roll back the reservation. */
        release_quota(iid, total);
        return NULL;
    }
    hdr_write(hdr, (uint64_t)size, iid);
    return (void *)(hdr + 1);
}

static void *
pyarena_calloc(void *ctx, size_t nelem, size_t elsize)
{
    /* Overflow check. `calloc(0, N)` and `calloc(N, 0)` are valid and
     * return a 0-length allocation. Only guard against multiplication
     * overflow. */
    if (elsize != 0 && nelem > (size_t)-1 / elsize) {
        return NULL;
    }
    size_t size = nelem * elsize;
    void *ptr = pyarena_malloc(ctx, size);
    if (ptr != NULL && size > 0) {
        memset(ptr, 0, size);
    }
    return ptr;
}

static void
pyarena_free(void *ctx, void *ptr)
{
    (void)ctx;
    if (ptr == NULL) {
        return;
    }
    Header *hdr = ((Header *)ptr) - 1;
    if (!hdr_is_ours(hdr)) {
        /* pre-install, untracked interp, or stray — pass-through at the
         * original pointer. Reading the 16 bytes at ptr-16 is safe in
         * practice (any pre-install allocation sits inside a larger
         * arena or glibc-managed region; ptr-16 stays within mapped
         * memory). The magic is 32 effective bits so the false-positive
         * probability is ~1/4e9 per free. */
        g_old_object_allocator.free(g_old_object_allocator.ctx, ptr);
        return;
    }
    uint64_t size = hdr_size(hdr);
    int64_t  iid  = hdr_interp(hdr);

    /* Decrement the interpreter's counter. If the entry has been
     * released between the matching malloc and this free, the find
     * returns NULL and the counter update is silently skipped. Per the
     * discipline in Worker._run, clear_quota is only called *after*
     * interp.close() — at which point no further frees from that
     * interpreter are possible. This keeps the race theoretical. */
    pthread_mutex_lock(&g_table_lock);
    QuotaEntry *e = find_entry_locked(iid);
    if (e != NULL) {
        uint64_t cur = atomic_load(&e->used);
        uint64_t dec = size + sizeof(Header);
        uint64_t next = (cur >= dec) ? cur - dec : 0;
        atomic_store(&e->used, next);
    }
    pthread_mutex_unlock(&g_table_lock);

    /* Clear the magic so a double-free is caught as pass-through rather
     * than double-decrement. */
    hdr->size_and_magic   = 0;
    hdr->interp_and_magic = 0;

    g_old_object_allocator.free(g_old_object_allocator.ctx, hdr);
}

static void *
pyarena_realloc(void *ctx, void *old_ptr, size_t new_size)
{
    if (old_ptr == NULL) {
        return pyarena_malloc(ctx, new_size);
    }
    if (new_size == 0) {
        pyarena_free(ctx, old_ptr);
        return NULL;
    }

    Header *old_hdr = ((Header *)old_ptr) - 1;
    if (!hdr_is_ours(old_hdr)) {
        /* Pre-install / untracked → delegate to old realloc at original
         * ptr. This is the *only* path where realloc delegates without
         * going through our check — consequently the resulting pointer
         * also has no header, which stays consistent with free(). */
        return g_old_object_allocator.realloc(
            g_old_object_allocator.ctx, old_ptr, new_size);
    }

    /* Ours: simple allocate-new → copy → free-old. CPython obmalloc does
     * similar internally when the size class changes. The extra copy is
     * acceptable given the ~µs granularity of large reallocs. */
    size_t old_size = hdr_size(old_hdr);
    void *new_ptr = pyarena_malloc(ctx, new_size);
    if (new_ptr == NULL) {
        return NULL;
    }
    size_t copy_size = (old_size < new_size) ? old_size : new_size;
    memcpy(new_ptr, old_ptr, copy_size);
    pyarena_free(ctx, old_ptr);
    return new_ptr;
}

/* ==================================================================== */
/* Python bindings                                                       */
/* ==================================================================== */

PyDoc_STRVAR(install_doc,
"install() -> None\n\n"
"Install the Object domain allocator hook. Idempotent — subsequent "
"calls after the first have no effect.\n\n"
"Must be called before any subinterpreter that needs quota tracking is "
"created. Allocations that existed before install() have no pyarena "
"header; free() routes them through the original allocator untouched "
"via a magic-number check.");

static PyObject *
py_install(PyObject *self, PyObject *args)
{
    (void)self;
    (void)args;

    pthread_mutex_lock(&g_table_lock);
    if (g_installed) {
        pthread_mutex_unlock(&g_table_lock);
        Py_RETURN_NONE;
    }

    /* Snapshot the current Object domain allocator so we can chain into
     * it for real allocation. This captures whatever allocator was in
     * place at install time — typically pymalloc or the system malloc. */
    PyMem_GetAllocator(PYMEM_DOMAIN_OBJ, &g_old_object_allocator);

    PyMemAllocatorEx new_alloc = {
        .ctx     = NULL,
        .malloc  = pyarena_malloc,
        .calloc  = pyarena_calloc,
        .realloc = pyarena_realloc,
        .free    = pyarena_free,
    };
    PyMem_SetAllocator(PYMEM_DOMAIN_OBJ, &new_alloc);
    g_installed = true;
    pthread_mutex_unlock(&g_table_lock);
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

    pthread_mutex_lock(&g_table_lock);
    QuotaEntry *e = acquire_entry_locked((int64_t)interp_id);
    if (e == NULL) {
        pthread_mutex_unlock(&g_table_lock);
        PyErr_Format(PyExc_RuntimeError,
                     "pyarena_alloc: quota table full (max %d entries)",
                     PYARENA_MAX_QUOTA_ENTRIES);
        return NULL;
    }
    e->limit = (uint64_t)bytes;
    pthread_mutex_unlock(&g_table_lock);
    Py_RETURN_NONE;
}

PyDoc_STRVAR(clear_quota_doc,
"clear_quota(interp_id) -> None\n\n"
"Remove any quota set for the given subinterpreter id. After this call "
"the interpreter is treated like the host — allocations pass through "
"without counting. Silent no-op if no entry exists.\n\n"
"Discipline: call this only after the subinterpreter has been closed. "
"Clearing while allocations are still live leads to counter drift — "
"PyArena's Worker._run enforces the right order.");

static PyObject *
py_clear_quota(PyObject *self, PyObject *args)
{
    (void)self;
    long long interp_id;
    if (!PyArg_ParseTuple(args, "L", &interp_id)) {
        return NULL;
    }
    pthread_mutex_lock(&g_table_lock);
    QuotaEntry *e = find_entry_locked((int64_t)interp_id);
    if (e != NULL) {
        release_entry_locked(e);
    }
    pthread_mutex_unlock(&g_table_lock);
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
    pthread_mutex_lock(&g_table_lock);
    QuotaEntry *e = find_entry_locked((int64_t)interp_id);
    uint64_t v = (e != NULL) ? atomic_load(&e->used) : 0;
    pthread_mutex_unlock(&g_table_lock);
    return PyLong_FromUnsignedLongLong(v);
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
    pthread_mutex_lock(&g_table_lock);
    QuotaEntry *e = find_entry_locked((int64_t)interp_id);
    uint64_t v = (e != NULL) ? atomic_load(&e->peak) : 0;
    pthread_mutex_unlock(&g_table_lock);
    return PyLong_FromUnsignedLongLong(v);
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

PyDoc_STRVAR(is_installed_doc,
"is_installed() -> bool\n\n"
"Return whether install() has been called and the allocator hook is "
"active. Testing helper.");

static PyObject *
py_is_installed(PyObject *self, PyObject *args)
{
    (void)self;
    (void)args;
    if (g_installed) {
        Py_RETURN_TRUE;
    }
    Py_RETURN_FALSE;
}

/* ==================================================================== */
/* Module definition                                                     */
/* ==================================================================== */

static PyMethodDef pyarena_alloc_methods[] = {
    {"install",      py_install,      METH_NOARGS,  install_doc},
    {"set_quota",    py_set_quota,    METH_VARARGS, set_quota_doc},
    {"clear_quota",  py_clear_quota,  METH_VARARGS, clear_quota_doc},
    {"get_used",     py_get_used,     METH_VARARGS, get_used_doc},
    {"get_peak",     py_get_peak,     METH_VARARGS, get_peak_doc},
    {"max_entries",  py_max_entries,  METH_NOARGS,  max_entries_doc},
    {"is_installed", py_is_installed, METH_NOARGS,  is_installed_doc},
    {NULL, NULL, 0, NULL},
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
    memset(g_quotas, 0, sizeof(g_quotas));
    return PyModule_Create(&pyarena_alloc_module);
}
