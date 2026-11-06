/* SPDX-License-Identifier: MIT — real target ABI smoke test for dependencies. */
#include <ffi.h>
#include <glib.h>
#include <stdint.h>
#include <stdio.h>
static uint64_t combine(uint32_t a, double b, uint64_t c) { return a + (uint64_t)b + c; }
static void closure(ffi_cif *cif, void *out, void **args, void *data)
{
    (void)cif;
    *(uint64_t *)out = combine(*(uint32_t *)args[0], *(double *)args[1], *(uint64_t *)args[2]) + *(uint64_t *)data;
}
static gpointer threaded(gpointer p) { g_atomic_int_inc((gint *)p); return NULL; }
int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    puts("Checking libffi mixed arguments");
    ffi_cif cif; ffi_type *types[] = {&ffi_type_uint32, &ffi_type_double, &ffi_type_uint64};
    uint32_t a = 5; double b = 7; uint64_t c = 30, result = 0; void *values[] = {&a, &b, &c};
    if (ffi_prep_cif(&cif, FFI_DEFAULT_ABI, 3, &ffi_type_uint64, types) != FFI_OK) return 1;
    ffi_call(&cif, FFI_FN(combine), &result, values); if (result != 42) return 2;
    puts("Checking libffi executable closure");
    void *code = NULL; ffi_closure *object = ffi_closure_alloc(sizeof(*object), &code); if (!object) return 3;
    uint64_t extra = 11;
    if (ffi_prep_closure_loc(object, &cif, closure, &extra, code) != FFI_OK) return 4;
    uint64_t (*function)(uint32_t, double, uint64_t); _Static_assert(sizeof(function) == sizeof(code), "function pointer size");
    __builtin_memcpy(&function, &code, sizeof(function));
    if (function(a, b, c) != 53) return 5; ffi_closure_free(object);
    puts("Checking GLib hash/thread/atomic");
    GHashTable *table = g_hash_table_new(g_str_hash, g_str_equal); g_hash_table_insert(table, "answer", GINT_TO_POINTER(42));
    if (GPOINTER_TO_INT(g_hash_table_lookup(table, "answer")) != 42) return 6; g_hash_table_destroy(table);
    gint count = 0; GThread *thread = g_thread_new("probe", threaded, &count); g_thread_join(thread);
    if (g_atomic_int_get(&count) != 1) return 7;
    puts("PASS real target dependencies: GLib hash/thread/atomic and libffi mixed-argument call/executable closure");
    return 0;
}
