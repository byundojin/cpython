/*
 * pyarena_alloc — per-interpreter memory quota via PyMem_SetAllocator.
 *
 * Part of the PyArena Unit 3 implementation. This file lives inside the
 * CPython fork checkout (/workspaces/PyArena/CPython/pyarena_alloc/) so
 * that Python.h and pyconfig.h from the locally-built interpreter are
 * directly reachable. The PyArena repository itself does not track any
 * .c source — this whole subdirectory is excluded via the CPython fork's
 * local .git/info/exclude, keeping both git histories clean.
 *
 * Design (docs/pyarena_기술_설계.md, pyarena_기술_선택과_이유.md):
 *   - Hook the Object domain allocator only.
 *       Raw domain is called without the GIL → PyInterpreterState_Get()
 *       can be NULL or unsafe there → hooking it risks crashes.
 *       Mem domain has low traffic and limited attack surface.
 *       Object domain carries ~all Python object allocation → full coverage
 *       of the attacks we care about (single huge alloc like "a" * 10**9).
 *   - 16-byte allocation header `{uint64_t size; uint64_t interp_id;}`
 *     prefixed to every tracked allocation. On free(ptr) we look at
 *     ptr - 16 to recover size and interpreter id so we can decrement
 *     the right quota counter.
 *   - Quota check + increment uses a CAS loop: fetch_add alone is not
 *     check-then-add atomic — two threads could race past the limit.
 *   - Host interpreter is exempt: when the lookup finds no quota entry
 *     for the calling interpreter, allocation passes straight through
 *     to the underlying allocator without a header.
 *   - install() must run before any subinterpreter is created. The hook
 *     leaves pre-install allocations untouched via a magic-number check
 *     in the header: if the magic field is wrong, this is not our memory
 *     (pre-install or leaked through another path) and we pass-through.
 *
 * ------------------------------------------------------------------------
 * CHECKPOINT STATUS — CP1 (skeleton)
 * ------------------------------------------------------------------------
 * This file currently contains only the module init stub. It loads as a
 * Python extension and exposes no functions — subsequent checkpoints add:
 *   CP2: quota table + set_quota/clear_quota/get_used/get_peak bindings
 *        (host-side data structures, no hook installation yet)
 *   CP3: Object domain allocator hook functions + install() + pre-install
 *        magic-based pass-through
 *   CP4: CAS loop quota enforcement + single large-alloc → NULL →
 *        MemoryError path
 * ------------------------------------------------------------------------
 */

#define PY_SSIZE_T_CLEAN
#include <Python.h>

static PyMethodDef pyarena_alloc_methods[] = {
    /* CP2 adds: install, set_quota, clear_quota, get_used, get_peak. */
    {NULL, NULL, 0, NULL}, /* sentinel */
};

static struct PyModuleDef pyarena_alloc_module = {
    PyModuleDef_HEAD_INIT,
    .m_name = "pyarena_alloc",
    .m_doc = "Per-interpreter memory quota via PyMem_SetAllocator hook.",
    .m_size = -1,
    .m_methods = pyarena_alloc_methods,
};

PyMODINIT_FUNC
PyInit_pyarena_alloc(void)
{
    return PyModule_Create(&pyarena_alloc_module);
}
