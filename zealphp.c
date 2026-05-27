/*
 * ext-zealphp — per-request function overrides for long-running PHP servers.
 *
 * Replaces ~15 PHP built-in functions (header, session_start, exec, etc.)
 * with user-supplied callbacks so each coroutine/request gets its own
 * response/session state. Drop-in replacement for uopz_set_return() with
 * a much smaller attack surface — allowlist-only, no class manipulation.
 *
 * API:
 *   zealphp_override(string $name, callable $cb): bool
 *   zealphp_restore(string $name): bool
 *   zealphp_restore_all(): void
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "php.h"
#include "php_ini.h"
#include "ext/standard/info.h"
#include "zend_exceptions.h"
#include "php_zealphp.h"
#include <dlfcn.h>

/* ── Storage ─────────────────────────────────────────────────────────── */

/* original handler pointers, keyed by lowercase function name */
static HashTable zealphp_orig_handlers;

/* PHP callable callbacks, keyed by lowercase function name */
static HashTable zealphp_callbacks;

/* ── Per-coroutine superglobal isolation ─────────────────────────────── */

/* Per-coroutine snapshots: coro_id → zval (array of 6 superglobal arrays) */
static HashTable zealphp_coro_snapshots;
static bool zealphp_coro_hooks_active = false;

/* OpenSwoole C-level hook typedefs (resolved via dlsym at runtime) */
typedef void (*coro_switch_fn_t)(void (*)(void *));
typedef long (*coro_get_cid_fn_t)(void);

static coro_switch_fn_t os_set_on_yield  = NULL;
static coro_switch_fn_t os_set_on_resume = NULL;
static coro_switch_fn_t os_set_on_close  = NULL;
static coro_get_cid_fn_t os_get_cid      = NULL;

/* Original OpenSwoole callbacks — must be chained, not replaced.
 * set_on_yield REPLACES the callback; PHPCoroutine::on_yield handles
 * PHP executor context switching (EG/CG swap). Without chaining,
 * PHP state is corrupted on coroutine resume → SIGSEGV. */
typedef void (*coro_callback_fn_t)(void *);
static coro_callback_fn_t orig_on_yield  = NULL;
static coro_callback_fn_t orig_on_resume = NULL;
static coro_callback_fn_t orig_on_close  = NULL;

static const char *sg_names[] = {"_GET","_POST","_COOKIE","_SERVER","_FILES","_REQUEST","_SESSION", NULL};

/* ── Per-request define() isolation ─────────────────────────────────── */

/* Track constants defined during the current request so they can be
 * removed on request end. Keys = constant name (case-sensitive zend_string). */
static HashTable zealphp_request_constants;
static bool zealphp_define_hooked = false;
static zif_handler zealphp_orig_define_handler = NULL;

/* ── Per-request $GLOBALS isolation ─────────────────────────────────── */

/* Snapshot of EG(symbol_table) keys at boot. Keys added after the snapshot
 * are considered request-scoped and removed by zealphp_globals_clean(). */
static HashTable zealphp_globals_snapshot;
static bool zealphp_globals_snapshotted = false;

static void zealphp_snapshot_save(long cid)
{
    /* Guard: EG(symbol_table) may be invalid during coroutine teardown.
     * Check that the table has a valid nTableMask (non-zero when initialized). */
    if (!EG(symbol_table).nTableMask) return;

    zval snapshot;
    array_init(&snapshot);
    for (const char **n = sg_names; *n; n++) {
        zval *sg = zend_hash_str_find(&EG(symbol_table), *n, strlen(*n));
        if (sg && Z_TYPE_P(sg) == IS_ARRAY) {
            /* Deep copy the array to avoid sharing zvals with the live
             * symbol table — the original may be modified or freed between
             * yield and resume/close. */
            zval copy;
            ZVAL_DUP(&copy, sg);
            add_assoc_zval(&snapshot, *n, &copy);
        }
    }
    zend_hash_index_update(&zealphp_coro_snapshots, (zend_ulong)cid, &snapshot);
}

static void zealphp_snapshot_restore(long cid)
{
    zval *snapshot = zend_hash_index_find(&zealphp_coro_snapshots, (zend_ulong)cid);
    if (!snapshot || Z_TYPE_P(snapshot) != IS_ARRAY) return;

    for (const char **n = sg_names; *n; n++) {
        zval *val = zend_hash_str_find(Z_ARRVAL_P(snapshot), *n, strlen(*n));
        if (val) {
            /* Delegate to zealphp_set_superglobal which handles both
             * EG(symbol_table) in-place swap AND PG(http_globals) update. */
            zealphp_set_superglobal(*n, strlen(*n), val);
        }
    }
}

/* Extract coroutine ID from the Coroutine* arg.
 * OpenSwoole's Coroutine class stores `long cid` after the vtable pointer
 * and a few base-class fields. We probe common offsets. */
static long zealphp_cid_from_arg(void *arg)
{
    if (!arg) return -1;
    /* Try reading cid at common struct offsets (bytes).
     * OpenSwoole Coroutine inherits from a base with vtable (8 bytes on x64),
     * then typically: long cid at offset 8, 16, or 24. */
    long *p = (long *)arg;
    /* Offset 0 (vtable ptr) — skip */
    /* Offset 8 (first member after vtable) */
    if (p[1] > 0 && p[1] < 1000000) return p[1];
    /* Offset 16 */
    if (p[2] > 0 && p[2] < 1000000) return p[2];
    /* Offset 24 */
    if (p[3] > 0 && p[3] < 1000000) return p[3];
    return -1;
}

/* OpenSwoole scheduler callbacks — called from C, no PHP stack needed.
 * Use the Coroutine* arg pointer itself as the unique key — guaranteed
 * unique per coroutine, no need to extract cid from the struct. */
static void zealphp_on_yield(void *arg)
{
    if (!arg) return;
    zealphp_snapshot_save((zend_long)(uintptr_t)arg);
    /* Chain to OpenSwoole's PHPCoroutine::on_yield — handles EG/CG swap */
    if (orig_on_yield) orig_on_yield(arg);
}

static void zealphp_on_resume(void *arg)
{
    /* Chain to OpenSwoole's PHPCoroutine::on_resume FIRST — restores EG/CG
     * so EG(symbol_table) is valid when we read/write superglobals */
    if (orig_on_resume) orig_on_resume(arg);
    if (!arg) return;
    zealphp_snapshot_restore((zend_long)(uintptr_t)arg);
}

static void zealphp_on_close(void *arg)
{
    /* Chain to OpenSwoole's PHPCoroutine::on_close FIRST */
    if (orig_on_close) orig_on_close(arg);
    if (!arg) return;
    zend_hash_index_del(&zealphp_coro_snapshots, (zend_ulong)(uintptr_t)arg);
}

/* ── Allowlist ───────────────────────────────────────────────────────── */

static const char *zealphp_allowed[] = {
    /* response */
    "header",
    "header_remove",
    "headers_list",
    "headers_sent",
    "setcookie",
    "setrawcookie",
    "http_response_code",
    "header_register_callback",
    /* output control */
    "flush",
    "ob_flush",
    "ob_end_flush",
    "ob_implicit_flush",
    "output_add_rewrite_var",
    "output_reset_rewrite_vars",
    /* process/connection */
    "set_time_limit",
    "ignore_user_abort",
    "connection_status",
    "connection_aborted",
    "register_shutdown_function",
    /* error handling */
    "error_log",
    "error_reporting",
    "set_error_handler",
    "restore_error_handler",
    "set_exception_handler",
    "restore_exception_handler",
    /* file upload */
    "is_uploaded_file",
    "move_uploaded_file",
    /* info */
    "phpinfo",
    "php_sapi_name",
    /* input filtering */
    "filter_input",
    "filter_input_array",
    /* session */
    "session_start",
    "session_id",
    "session_status",
    "session_name",
    "session_write_close",
    "session_destroy",
    "session_unset",
    "session_regenerate_id",
    "session_get_cookie_params",
    "session_set_cookie_params",
    "session_cache_limiter",
    "session_cache_expire",
    "session_commit",
    "session_abort",
    "session_encode",
    "session_decode",
    "session_save_path",
    "session_module_name",
    /* exec family */
    "shell_exec",
    "exec",
    "system",
    "passthru",
    NULL
};

static bool zealphp_is_allowed(const char *name, size_t len)
{
    for (const char **p = zealphp_allowed; *p; p++) {
        if (strlen(*p) == len && memcmp(*p, name, len) == 0) {
            return true;
        }
    }
    return false;
}

/* ── Generic handler ─────────────────────────────────────────────────── */

static ZEND_NAMED_FUNCTION(zealphp_dispatch)
{
    zend_string *fname = execute_data->func->common.function_name;
    zend_string *lc = zend_string_tolower(fname);

    zval *cb = zend_hash_find(&zealphp_callbacks, lc);
    zend_string_release(lc);

    if (!cb || Z_TYPE_P(cb) == IS_UNDEF) {
        RETURN_NULL();
    }

    uint32_t argc = ZEND_CALL_NUM_ARGS(execute_data);
    zval *args = NULL;
    if (argc > 0) {
        args = ZEND_CALL_ARG(execute_data, 1);
    }

    zval retval;
    ZVAL_UNDEF(&retval);

    if (call_user_function(NULL, NULL, cb, &retval, argc, args) == SUCCESS) {
        if (Z_TYPE(retval) != IS_UNDEF) {
            ZVAL_COPY_VALUE(return_value, &retval);
        } else {
            zval_ptr_dtor(&retval);
        }
    } else {
        zval_ptr_dtor(&retval);
        if (EG(exception)) {
            return;
        }
        php_error_docref(NULL, E_WARNING,
            "ext-zealphp: callback for %s failed", ZSTR_VAL(fname));
    }
}

/* ── zealphp_override(string $name, callable $cb): bool ──────────── */

PHP_FUNCTION(zealphp_override)
{
    zend_string *func_name;
    zend_fcall_info fci;
    zend_fcall_info_cache fcc;

    ZEND_PARSE_PARAMETERS_START(2, 2)
        Z_PARAM_STR(func_name)
        Z_PARAM_FUNC(fci, fcc)
    ZEND_PARSE_PARAMETERS_END();

    zend_string *lc = zend_string_tolower(func_name);

    /* allowlist check */
    if (!zealphp_is_allowed(ZSTR_VAL(lc), ZSTR_LEN(lc))) {
        php_error_docref(NULL, E_WARNING,
            "ext-zealphp: overriding '%s' is not allowed", ZSTR_VAL(func_name));
        zend_string_release(lc);
        RETURN_FALSE;
    }

    /* already overridden? */
    if (zend_hash_exists(&zealphp_orig_handlers, lc)) {
        php_error_docref(NULL, E_WARNING,
            "ext-zealphp: '%s' is already overridden — restore first",
            ZSTR_VAL(func_name));
        zend_string_release(lc);
        RETURN_FALSE;
    }

    /* find original */
    zend_function *func = zend_hash_find_ptr(CG(function_table), lc);
    if (!func) {
        php_error_docref(NULL, E_WARNING,
            "ext-zealphp: function '%s' not found", ZSTR_VAL(func_name));
        zend_string_release(lc);
        RETURN_FALSE;
    }

    if (func->type != ZEND_INTERNAL_FUNCTION) {
        php_error_docref(NULL, E_WARNING,
            "ext-zealphp: can only override internal (C) functions, not user functions");
        zend_string_release(lc);
        RETURN_FALSE;
    }

    /* save original handler */
    zend_hash_add_new_ptr(&zealphp_orig_handlers, lc,
                          (void *)func->internal_function.handler);

    /* save callback */
    zval cb_copy;
    ZVAL_COPY(&cb_copy, &fci.function_name);
    zend_hash_update(&zealphp_callbacks, lc, &cb_copy);

    /* swap handler */
    func->internal_function.handler = zealphp_dispatch;

    zend_string_release(lc);
    RETURN_TRUE;
}

/* ── zealphp_restore(string $name): bool ─────────────────────────── */

PHP_FUNCTION(zealphp_restore)
{
    zend_string *func_name;

    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_STR(func_name)
    ZEND_PARSE_PARAMETERS_END();

    zend_string *lc = zend_string_tolower(func_name);

    void *orig = zend_hash_find_ptr(&zealphp_orig_handlers, lc);
    if (!orig) {
        zend_string_release(lc);
        RETURN_FALSE;
    }

    zend_function *func = zend_hash_find_ptr(CG(function_table), lc);
    if (func) {
        func->internal_function.handler = (zif_handler)orig;
    }

    zend_hash_del(&zealphp_orig_handlers, lc);
    zend_hash_del(&zealphp_callbacks, lc);

    zend_string_release(lc);
    RETURN_TRUE;
}

/* ── zealphp_restore_all(): void ─────────────────────────────────── */

PHP_FUNCTION(zealphp_restore_all)
{
    zend_string *key;
    void *orig;

    ZEND_PARSE_PARAMETERS_NONE();

    ZEND_HASH_FOREACH_STR_KEY_PTR(&zealphp_orig_handlers, key, orig) {
        zend_function *func = zend_hash_find_ptr(CG(function_table), key);
        if (func) {
            func->internal_function.handler = (zif_handler)orig;
        }
    } ZEND_HASH_FOREACH_END();

    zend_hash_clean(&zealphp_orig_handlers);
    zend_hash_clean(&zealphp_callbacks);
}

/* ── Superglobals management ─────────────────────────────────────── */

/* Map superglobal name to PG(http_globals) index.
 * Returns -1 for _SESSION/_ENV (no TRACK_VARS slot). */
static int zealphp_track_vars_index(const char *name)
{
    if (name[0] != '_') return -1;
    if (strcmp(name, "_GET") == 0)     return TRACK_VARS_GET;
    if (strcmp(name, "_POST") == 0)    return TRACK_VARS_POST;
    if (strcmp(name, "_COOKIE") == 0)  return TRACK_VARS_COOKIE;
    if (strcmp(name, "_SERVER") == 0)  return TRACK_VARS_SERVER;
    if (strcmp(name, "_FILES") == 0)   return TRACK_VARS_FILES;
    if (strcmp(name, "_REQUEST") == 0) return TRACK_VARS_REQUEST;
    return -1;
}

/* Helper: overwrite a superglobal in the executor symbol table AND
 * update PG(http_globals) so PHP's auto-global JIT resolves correctly.
 *
 * PG(http_globals)[TRACK_VARS_*] is what PHP reads when the auto-global
 * JIT fires. In OpenSwoole coroutine mode, PG() is process-wide (not
 * per-coroutine). Without this update, the JIT returns stale data from
 * a prior coroutine. Updating PG(http_globals) on every set ensures
 * the next auto-global resolution sees the current request's data.
 *
 * ZVAL_DUP creates a full copy with refcount=1 (no COW on write).
 * Safe ordering: copy new → dtor old (survives GC rehash). */
static void zealphp_set_superglobal(const char *name, size_t name_len, zval *value)
{
    zval *existing = zend_hash_str_find(&EG(symbol_table), name, name_len);
    if (existing) {
        zval old;
        ZVAL_COPY_VALUE(&old, existing);
        ZVAL_DUP(existing, value);
        zval_ptr_dtor(&old);
    } else {
        zval copy;
        ZVAL_DUP(&copy, value);
        zend_hash_str_add_new(&EG(symbol_table), name, name_len, &copy);
        existing = zend_hash_str_find(&EG(symbol_table), name, name_len);
    }

    /* Update PG(http_globals) so auto-global JIT resolves to current data */
    int idx = zealphp_track_vars_index(name);
    if (idx >= 0 && existing) {
        zval_ptr_dtor(&PG(http_globals)[idx]);
        ZVAL_COPY(&PG(http_globals)[idx], existing);
    }
}

/* zealphp_superglobals_set(array $g, $p, $c, $s, $f, $r, $sess): void
 * Overwrites $_GET, $_POST, $_COOKIE, $_SERVER, $_FILES, $_REQUEST, $_SESSION
 * with the supplied arrays. Called by ZealPHP at request start. */
PHP_FUNCTION(zealphp_superglobals_set)
{
    zval *get, *post, *cookie, *server, *files, *request, *session;

    ZEND_PARSE_PARAMETERS_START(7, 7)
        Z_PARAM_ARRAY(get)
        Z_PARAM_ARRAY(post)
        Z_PARAM_ARRAY(cookie)
        Z_PARAM_ARRAY(server)
        Z_PARAM_ARRAY(files)
        Z_PARAM_ARRAY(request)
        Z_PARAM_ARRAY(session)
    ZEND_PARSE_PARAMETERS_END();

    zealphp_set_superglobal("_GET",     sizeof("_GET")-1,     get);
    zealphp_set_superglobal("_POST",    sizeof("_POST")-1,    post);
    zealphp_set_superglobal("_COOKIE",  sizeof("_COOKIE")-1,  cookie);
    zealphp_set_superglobal("_SERVER",  sizeof("_SERVER")-1,  server);
    zealphp_set_superglobal("_FILES",   sizeof("_FILES")-1,   files);
    zealphp_set_superglobal("_REQUEST", sizeof("_REQUEST")-1, request);
    zealphp_set_superglobal("_SESSION", sizeof("_SESSION")-1, session);
}

/* zealphp_superglobals_clear(): void
 * Resets all superglobals to empty arrays. Called at request end
 * to prevent cross-request leakage in coroutine mode. */
PHP_FUNCTION(zealphp_superglobals_clear)
{
    ZEND_PARSE_PARAMETERS_NONE();

    for (const char **n = sg_names; *n; n++) {
        zval empty;
        array_init(&empty);
        zealphp_set_superglobal(*n, strlen(*n), &empty);
        zval_ptr_dtor(&empty);
    }
}

/* zealphp_superglobals_save(): array
 * Snapshots all 6 superglobals into one array for coroutine storage. */
PHP_FUNCTION(zealphp_superglobals_save)
{
    ZEND_PARSE_PARAMETERS_NONE();

    array_init(return_value);
    for (const char **n = sg_names; *n; n++) {
        zval *sg = zend_hash_str_find(&EG(symbol_table), *n, strlen(*n));
        if (sg) {
            zval copy;
            ZVAL_COPY(&copy, sg);
            add_assoc_zval(return_value, *n, &copy);
        }
    }
}

/* zealphp_superglobals_restore(array $snapshot): void
 * Restores superglobals from a snapshot saved by zealphp_superglobals_save(). */
PHP_FUNCTION(zealphp_superglobals_restore)
{
    zval *snapshot;

    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_ARRAY(snapshot)
    ZEND_PARSE_PARAMETERS_END();

    for (const char **n = sg_names; *n; n++) {
        zval *val = zend_hash_str_find(Z_ARRVAL_P(snapshot), *n, strlen(*n));
        if (val) {
            zealphp_set_superglobal(*n, strlen(*n), val);
        }
    }
}

/* ── zealphp_coroutine_superglobals(bool $enable): bool ──────────── */
/* Activates per-coroutine superglobal save/restore via OpenSwoole's
 * yield/resume/close hooks. Returns true if hooks were registered,
 * false if OpenSwoole scheduler hooks aren't available. */
PHP_FUNCTION(zealphp_coroutine_superglobals)
{
    bool enable;

    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_BOOL(enable)
    ZEND_PARSE_PARAMETERS_END();

    if (!enable) {
        zealphp_coro_hooks_active = false;
        zend_hash_clean(&zealphp_coro_snapshots);
        RETURN_TRUE;
    }

    if (!os_set_on_yield || !os_set_on_resume || !os_set_on_close || !os_get_cid) {
        php_error_docref(NULL, E_WARNING,
            "ext-zealphp: OpenSwoole coroutine scheduler hooks not found. "
            "Per-coroutine superglobal isolation requires OpenSwoole.");
        RETURN_FALSE;
    }

    if (!zealphp_coro_hooks_active) {
        /* Save existing OpenSwoole callbacks (PHPCoroutine::on_yield/resume/close)
         * via the Coroutine::on_yield/on_resume global variables. These handle
         * PHP executor context switching — we MUST chain to them, not replace. */
        void *handle = dlopen(NULL, RTLD_LAZY);
        if (handle) {
            static const char *yield_var_names[] = {
                "_ZN10openswoole9Coroutine8on_yieldE",
                "_ZN6swoole9Coroutine8on_yieldE", NULL
            };
            static const char *resume_var_names[] = {
                "_ZN10openswoole9Coroutine9on_resumeE",
                "_ZN6swoole9Coroutine9on_resumeE", NULL
            };
            static const char *close_var_names[] = {
                "_ZN10openswoole9Coroutine8on_closeE",
                "_ZN6swoole9Coroutine8on_closeE", NULL
            };
            for (const char **n = yield_var_names; *n && !orig_on_yield; n++) {
                coro_callback_fn_t *p = (coro_callback_fn_t *)dlsym(handle, *n);
                if (p) orig_on_yield = *p;
            }
            for (const char **n = resume_var_names; *n && !orig_on_resume; n++) {
                coro_callback_fn_t *p = (coro_callback_fn_t *)dlsym(handle, *n);
                if (p) orig_on_resume = *p;
            }
            for (const char **n = close_var_names; *n && !orig_on_close; n++) {
                coro_callback_fn_t *p = (coro_callback_fn_t *)dlsym(handle, *n);
                if (p) orig_on_close = *p;
            }
            dlclose(handle);
        }

        os_set_on_yield(zealphp_on_yield);
        os_set_on_resume(zealphp_on_resume);
        os_set_on_close(zealphp_on_close);
        zealphp_coro_hooks_active = true;
    }

    RETURN_TRUE;
}

/* ── Process-state snapshot/clean (included_files + classes + functions) ── */

/* Full process-state snapshot for pool workers. Captures:
 * - EG(included_files) keys — require_once cache
 * - CG(class_table) user-class keys
 * - CG(function_table) user-function keys
 * Cleaning removes entries added after the snapshot, giving the pool worker
 * fresh-process semantics without the proc_open cost. */
static HashTable zealphp_snapshot_files;
static HashTable zealphp_snapshot_classes;
static HashTable zealphp_snapshot_functions;
static bool zealphp_state_snapshotted = false;

PHP_FUNCTION(zealphp_process_state_snapshot)
{
    zend_string *key;

    if (zealphp_state_snapshotted) {
        zend_hash_clean(&zealphp_snapshot_files);
        zend_hash_clean(&zealphp_snapshot_classes);
        zend_hash_clean(&zealphp_snapshot_functions);
    }

    zval one;
    ZVAL_LONG(&one, 1);

    /* Included files */
    ZEND_HASH_FOREACH_STR_KEY(&EG(included_files), key) {
        if (key) zend_hash_update(&zealphp_snapshot_files, key, &one);
    } ZEND_HASH_FOREACH_END();

    /* User classes only */
    zval *cls_zv;
    ZEND_HASH_FOREACH_STR_KEY_VAL(CG(class_table), key, cls_zv) {
        if (key) {
            zend_class_entry *ce = Z_PTR_P(cls_zv);
            if (ce && ce->type == ZEND_USER_CLASS) {
                zend_hash_update(&zealphp_snapshot_classes, key, &one);
            }
        }
    } ZEND_HASH_FOREACH_END();

    /* User functions only */
    zval *fn_zv;
    ZEND_HASH_FOREACH_STR_KEY_VAL(CG(function_table), key, fn_zv) {
        if (key) {
            zend_function *func = Z_PTR_P(fn_zv);
            if (func && func->type == ZEND_USER_FUNCTION) {
                zend_hash_update(&zealphp_snapshot_functions, key, &one);
            }
        }
    } ZEND_HASH_FOREACH_END();

    zealphp_state_snapshotted = true;
}

PHP_FUNCTION(zealphp_process_state_clean)
{
    zend_string *key;
    zval *val;

    if (!zealphp_state_snapshotted) return;

    /* --- Included files: remove entries not in snapshot --- */
    zend_string **del_files = NULL;
    uint32_t df_count = 0, df_cap = 64;
    del_files = emalloc(sizeof(zend_string *) * df_cap);

    ZEND_HASH_FOREACH_STR_KEY(&EG(included_files), key) {
        if (key && !zend_hash_exists(&zealphp_snapshot_files, key)) {
            if (df_count >= df_cap) { df_cap *= 2; del_files = erealloc(del_files, sizeof(zend_string *) * df_cap); }
            del_files[df_count++] = key;
        }
    } ZEND_HASH_FOREACH_END();
    for (uint32_t i = 0; i < df_count; i++) zend_hash_del(&EG(included_files), del_files[i]);
    efree(del_files);

    /* --- User classes: remove entries not in snapshot --- */
    /* SAFETY: skip classes that have initialized static members (runtime
     * copy of default_static_members_table). Removing such classes via
     * zend_hash_del leaves a zombie entry (class_exists returns true but
     * statics are inaccessible → segfault). See ext-zealphp#1.
     * Classes without statics or with never-accessed statics are safe. */
    zend_string **del_cls = NULL;
    uint32_t dc_count = 0, dc_cap = 64;
    del_cls = emalloc(sizeof(zend_string *) * dc_cap);

    ZEND_HASH_FOREACH_STR_KEY_VAL(CG(class_table), key, val) {
        if (key && !zend_hash_exists(&zealphp_snapshot_classes, key)) {
            zend_class_entry *ce = Z_PTR_P(val);
            if (ce && ce->type == ZEND_USER_CLASS) {
                /* Skip if statics have been initialized at runtime */
                if (ce->default_static_members_count > 0
                    && CE_STATIC_MEMBERS(ce) != NULL
                    && CE_STATIC_MEMBERS(ce) != ce->default_static_members_table) {
                    continue;  /* unsafe to remove — zombie class risk */
                }
                if (dc_count >= dc_cap) { dc_cap *= 2; del_cls = erealloc(del_cls, sizeof(zend_string *) * dc_cap); }
                del_cls[dc_count++] = key;
            }
        }
    } ZEND_HASH_FOREACH_END();
    for (uint32_t i = 0; i < dc_count; i++) zend_hash_del(CG(class_table), del_cls[i]);
    efree(del_cls);

    /* --- User functions: remove entries not in snapshot --- */
    zend_string **del_fn = NULL;
    uint32_t dfn_count = 0, dfn_cap = 64;
    del_fn = emalloc(sizeof(zend_string *) * dfn_cap);

    ZEND_HASH_FOREACH_STR_KEY_VAL(CG(function_table), key, val) {
        if (key && !zend_hash_exists(&zealphp_snapshot_functions, key)) {
            zend_function *func = Z_PTR_P(val);
            if (func && func->type == ZEND_USER_FUNCTION) {
                if (dfn_count >= dfn_cap) { dfn_cap *= 2; del_fn = erealloc(del_fn, sizeof(zend_string *) * dfn_cap); }
                del_fn[dfn_count++] = key;
            }
        }
    } ZEND_HASH_FOREACH_END();
    for (uint32_t i = 0; i < dfn_count; i++) zend_hash_del(CG(function_table), del_fn[i]);
    efree(del_fn);
}

/* ── $GLOBALS snapshot/clean ─────────────────────────────────────── */

/* zealphp_globals_snapshot(): void — save current EG(symbol_table) keys */
PHP_FUNCTION(zealphp_globals_snapshot)
{
    zend_string *key;

    if (zealphp_globals_snapshotted) {
        zend_hash_clean(&zealphp_globals_snapshot);
    }

    ZEND_HASH_FOREACH_STR_KEY(&EG(symbol_table), key) {
        if (key) {
            zval one;
            ZVAL_LONG(&one, 1);
            zend_hash_update(&zealphp_globals_snapshot, key, &one);
        }
    } ZEND_HASH_FOREACH_END();

    zealphp_globals_snapshotted = true;
}

/* zealphp_globals_clean(): void — remove keys not in the snapshot */
PHP_FUNCTION(zealphp_globals_clean)
{
    zend_string *key;
    zval *val;

    if (!zealphp_globals_snapshotted) {
        return;
    }

    /* Collect keys to delete (can't delete while iterating) */
    zend_string **to_delete = NULL;
    uint32_t delete_count = 0;
    uint32_t delete_cap = 64;
    to_delete = emalloc(sizeof(zend_string *) * delete_cap);

    ZEND_HASH_FOREACH_STR_KEY_VAL(&EG(symbol_table), key, val) {
        if (key && !zend_hash_exists(&zealphp_globals_snapshot, key)) {
            if (delete_count >= delete_cap) {
                delete_cap *= 2;
                to_delete = erealloc(to_delete, sizeof(zend_string *) * delete_cap);
            }
            to_delete[delete_count++] = key;
        }
    } ZEND_HASH_FOREACH_END();

    for (uint32_t i = 0; i < delete_count; i++) {
        zend_hash_del(&EG(symbol_table), to_delete[i]);
    }

    efree(to_delete);
}

/* ── define() interception ───────────────────────────────────────── */

/* Intercept define() to track per-request constants. The real define()
 * runs first; if it succeeds, we record the name so zealphp_constants_clear()
 * can remove it at request end. */
static ZEND_NAMED_FUNCTION(zealphp_define_intercept)
{
    /* Forward to the real define() first */
    zealphp_orig_define_handler(INTERNAL_FUNCTION_PARAM_PASSTHRU);

    /* If define() succeeded (returned true), track the constant name */
    if (Z_TYPE_P(return_value) == IS_TRUE) {
        zval *arg1 = ZEND_CALL_ARG(execute_data, 1);
        if (arg1 && Z_TYPE_P(arg1) == IS_STRING) {
            zval one;
            ZVAL_LONG(&one, 1);
            zend_hash_update(&zealphp_request_constants, Z_STR_P(arg1), &one);
        }
    }
}

/* ── zealphp_constants_clear(): void ─────────────────────────────── */

PHP_FUNCTION(zealphp_constants_clear)
{
    zend_string *name;
    ZEND_HASH_FOREACH_STR_KEY(&zealphp_request_constants, name) {
        if (name) {
            /* zend_hash_del on EG(zend_constants) removes the constant.
             * Case-sensitive constants use the exact name as key. */
            zend_hash_del(EG(zend_constants), name);
        }
    } ZEND_HASH_FOREACH_END();
    zend_hash_clean(&zealphp_request_constants);
}

/* ── zealphp_define_hook(bool $enable): bool ─────────────────────── */

PHP_FUNCTION(zealphp_define_hook)
{
    bool enable;
    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_BOOL(enable)
    ZEND_PARSE_PARAMETERS_END();

    if (enable && !zealphp_define_hooked) {
        zend_string *name = zend_string_init("define", sizeof("define") - 1, 0);
        zend_function *func = zend_hash_find_ptr(CG(function_table), name);
        if (func && func->type == ZEND_INTERNAL_FUNCTION) {
            zealphp_orig_define_handler = func->internal_function.handler;
            func->internal_function.handler = zealphp_define_intercept;
            zealphp_define_hooked = true;
        }
        zend_string_release(name);
    } else if (!enable && zealphp_define_hooked) {
        zend_string *name = zend_string_init("define", sizeof("define") - 1, 0);
        zend_function *func = zend_hash_find_ptr(CG(function_table), name);
        if (func && zealphp_orig_define_handler) {
            func->internal_function.handler = zealphp_orig_define_handler;
            zealphp_orig_define_handler = NULL;
            zealphp_define_hooked = false;
        }
        zend_string_release(name);
    }

    RETURN_BOOL(zealphp_define_hooked);
}

/* ── Module lifecycle ────────────────────────────────────────────── */

PHP_MINIT_FUNCTION(zealphp)
{
    zend_hash_init(&zealphp_orig_handlers, 32, NULL, NULL, 1);
    zend_hash_init(&zealphp_callbacks, 32, NULL, ZVAL_PTR_DTOR, 1);
    zend_hash_init(&zealphp_coro_snapshots, 256, NULL, ZVAL_PTR_DTOR, 0);
    zend_hash_init(&zealphp_request_constants, 64, NULL, ZVAL_PTR_DTOR, 1);
    zend_hash_init(&zealphp_globals_snapshot, 128, NULL, ZVAL_PTR_DTOR, 1);
    zend_hash_init(&zealphp_snapshot_files, 256, NULL, ZVAL_PTR_DTOR, 1);
    zend_hash_init(&zealphp_snapshot_classes, 256, NULL, ZVAL_PTR_DTOR, 1);
    zend_hash_init(&zealphp_snapshot_functions, 256, NULL, ZVAL_PTR_DTOR, 1);

    /* Try to hook into OpenSwoole's coroutine scheduler for per-coroutine
     * superglobal isolation. Resolved at runtime via dlsym — if OpenSwoole
     * isn't loaded or the symbols aren't found, we silently skip. */
    void *handle = dlopen(NULL, RTLD_LAZY);
    if (handle) {
        /* Resolve coroutine scheduler hooks. Try multiple symbol patterns
         * to support different OpenSwoole/Swoole versions:
         * 1. Clean C exports (future/proposed)
         * 2. OpenSwoole C++ mangled (current 22.x/26.x)
         * 3. Swoole C++ mangled (for Swoole compat) */
        static const char *yield_names[] = {
            "openswoole_coroutine_set_on_yield",
            "_ZN10openswoole9Coroutine12set_on_yieldEPFvPvE",
            "_ZN6swoole9Coroutine12set_on_yieldEPFvPvE",
            NULL
        };
        static const char *resume_names[] = {
            "openswoole_coroutine_set_on_resume",
            "_ZN10openswoole9Coroutine13set_on_resumeEPFvPvE",
            "_ZN6swoole9Coroutine13set_on_resumeEPFvPvE",
            NULL
        };
        static const char *close_names[] = {
            "openswoole_coroutine_set_on_close",
            "_ZN10openswoole9Coroutine12set_on_closeEPFvPvE",
            "_ZN6swoole9Coroutine12set_on_closeEPFvPvE",
            NULL
        };
        for (const char **n = yield_names; *n && !os_set_on_yield; n++)
            os_set_on_yield = (coro_switch_fn_t)dlsym(handle, *n);
        for (const char **n = resume_names; *n && !os_set_on_resume; n++)
            os_set_on_resume = (coro_switch_fn_t)dlsym(handle, *n);
        for (const char **n = close_names; *n && !os_set_on_close; n++)
            os_set_on_close = (coro_switch_fn_t)dlsym(handle, *n);
        os_get_cid = (coro_get_cid_fn_t)dlsym(handle, "openswoole_coroutine_get_current_id");
        if (!os_get_cid)
            os_get_cid = (coro_get_cid_fn_t)dlsym(handle, "swoole_coroutine_get_current_id");
        dlclose(handle);
    }

    return SUCCESS;
}

PHP_MSHUTDOWN_FUNCTION(zealphp)
{
    /* Process is exiting — skip ALL cleanup. On PHP 8.5+ the shutdown order
     * changed: CG(function_table) and refcounted objects (Closures stored in
     * zealphp_callbacks) may already be freed by the time MSHUTDOWN runs.
     * Calling zend_hash_destroy on tables whose zval dtors touch freed memory
     * causes SIGSEGV. The OS reclaims all process memory on exit — this is
     * the standard pattern used by opcache and other core extensions. */
    return SUCCESS;
}

PHP_MINFO_FUNCTION(zealphp)
{
    php_info_print_table_start();
    php_info_print_table_row(2, "ext-zealphp", "enabled");
    php_info_print_table_row(2, "Version", PHP_ZEALPHP_VERSION);
    php_info_print_table_row(2, "Purpose",
        "Per-request function overrides for long-running PHP servers");
    php_info_print_table_end();
}

/* ── Function entries ────────────────────────────────────────────── */

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_zealphp_override, 0, 2, _IS_BOOL, 0)
    ZEND_ARG_TYPE_INFO(0, function_name, IS_STRING, 0)
    ZEND_ARG_TYPE_INFO(0, callback, IS_CALLABLE, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_zealphp_restore, 0, 1, _IS_BOOL, 0)
    ZEND_ARG_TYPE_INFO(0, function_name, IS_STRING, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_zealphp_restore_all, 0, 0, IS_VOID, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_zealphp_superglobals_set, 0, 7, IS_VOID, 0)
    ZEND_ARG_TYPE_INFO(0, get, IS_ARRAY, 0)
    ZEND_ARG_TYPE_INFO(0, post, IS_ARRAY, 0)
    ZEND_ARG_TYPE_INFO(0, cookie, IS_ARRAY, 0)
    ZEND_ARG_TYPE_INFO(0, server, IS_ARRAY, 0)
    ZEND_ARG_TYPE_INFO(0, files, IS_ARRAY, 0)
    ZEND_ARG_TYPE_INFO(0, request, IS_ARRAY, 0)
    ZEND_ARG_TYPE_INFO(0, session, IS_ARRAY, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_zealphp_superglobals_clear, 0, 0, IS_VOID, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_zealphp_superglobals_save, 0, 0, IS_ARRAY, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_zealphp_superglobals_restore, 0, 1, IS_VOID, 0)
    ZEND_ARG_TYPE_INFO(0, snapshot, IS_ARRAY, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_zealphp_coroutine_superglobals, 0, 1, _IS_BOOL, 0)
    ZEND_ARG_TYPE_INFO(0, enable, _IS_BOOL, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_zealphp_constants_clear, 0, 0, IS_VOID, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_zealphp_globals_snapshot, 0, 0, IS_VOID, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_zealphp_globals_clean, 0, 0, IS_VOID, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_zealphp_process_state_snapshot, 0, 0, IS_VOID, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_zealphp_process_state_clean, 0, 0, IS_VOID, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_zealphp_define_hook, 0, 1, _IS_BOOL, 0)
    ZEND_ARG_TYPE_INFO(0, enable, _IS_BOOL, 0)
ZEND_END_ARG_INFO()

static const zend_function_entry zealphp_functions[] = {
    PHP_FE(zealphp_override,               arginfo_zealphp_override)
    PHP_FE(zealphp_restore,                arginfo_zealphp_restore)
    PHP_FE(zealphp_restore_all,            arginfo_zealphp_restore_all)
    PHP_FE(zealphp_superglobals_set,       arginfo_zealphp_superglobals_set)
    PHP_FE(zealphp_superglobals_clear,     arginfo_zealphp_superglobals_clear)
    PHP_FE(zealphp_superglobals_save,      arginfo_zealphp_superglobals_save)
    PHP_FE(zealphp_superglobals_restore,   arginfo_zealphp_superglobals_restore)
    PHP_FE(zealphp_coroutine_superglobals, arginfo_zealphp_coroutine_superglobals)
    PHP_FE(zealphp_constants_clear,        arginfo_zealphp_constants_clear)
    PHP_FE(zealphp_define_hook,            arginfo_zealphp_define_hook)
    PHP_FE(zealphp_globals_snapshot,       arginfo_zealphp_globals_snapshot)
    PHP_FE(zealphp_globals_clean,          arginfo_zealphp_globals_clean)
    PHP_FE(zealphp_process_state_snapshot, arginfo_zealphp_process_state_snapshot)
    PHP_FE(zealphp_process_state_clean,    arginfo_zealphp_process_state_clean)
    PHP_FE_END
};

/* ── Module entry ────────────────────────────────────────────────── */

zend_module_entry zealphp_module_entry = {
    STANDARD_MODULE_HEADER,
    "zealphp",
    zealphp_functions,
    PHP_MINIT(zealphp),
    PHP_MSHUTDOWN(zealphp),
    NULL, /* RINIT */
    NULL, /* RSHUTDOWN */
    PHP_MINFO(zealphp),
    PHP_ZEALPHP_VERSION,
    STANDARD_MODULE_PROPERTIES
};

#ifdef COMPILE_DL_ZEALPHP
ZEND_GET_MODULE(zealphp)
#endif
