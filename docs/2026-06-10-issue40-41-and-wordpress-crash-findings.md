# ext-zealphp — #40/#41 bisect + WordPress coroutine-legacy crash root cause (2026-06-10)

Investigated in a clean, single-app `zext` Docker container (PHP 8.4.21, OpenSwoole 26.2.0,
coroutine-legacy). All results are from version-verified binaries (see "Build gotchas").

## TL;DR

- **#40/#41 (server-array contamination after yield) is REAL in v0.3.41 and FIXED by the
  v0.3.42 three-patch superglobals-ownership rework. Ship v0.3.42 for #40/#41.**
- **The "v0.3.42 SIGSEGV" reported earlier was a misattribution.** The constants-snapshot
  path is byte-identical between v0.3.41 and v0.3.42 (the #40 diff never touches it), and the
  t40 repro defines no request constants so it can never reach `zealphp_constants_snapshot_delete`.
- **WordPress on coroutine-legacy has two crash layers, both PRE-EXISTING in v0.3.41 AND v0.3.42**
  (confirmed: WordPress crashes identically on both binaries). v0.3.42 introduces no crash regression.

## #40 / #41 bisect (decisive)

| Binary | gate | t40 `/dash` post-yield |
|---|---|---|
| v0.3.41 (`so_v341.so`, SKIP(owned)) | single-owner | **contaminated**: `g_phpself=/app.php`, `s_uri=/ping`, `server_n=21` (the internal `/ping` sub-request's `$_SERVER` leaked into the resumed `/dash` coroutine) |
| v0.3.42 (`so_v342.so`, SKIP(not-request-ptr)) | ptr-set membership | **correct**: `/dash.php`, `server_n=22` |

The `go()` child still shows `g_phpself=NIL` (RequestContext not propagated into a nested child
coroutine) but its `$_SERVER` is correct — that residual is a PHP-layer RequestContext concern,
not the C-ext superglobal isolation.

## WordPress crash — two layers (reproduce with `ab -c20` on a freshly-booted worker)

Sequential requests = 200; concurrent load = crash. Both layers pre-existing in v0.3.41 + v0.3.42.

### Layer 1 — cold-autoload duplicate class entry (status=255, signal=0)

```
Uncaught TypeError: App::{closure registerOnRequest():8931}():
  Argument #1 ($request) must be of type ZealPHP\HTTP\Request, ZealPHP\HTTP\Request given
```

Same class name, different class-entry pointer = two CEs of `ZealPHP\HTTP\Request`.

**Root cause:** `App::preloadRequestPathClasses()` runs in **onWorkerStart, which is a coroutine**.
`ZealPHP\HTTP\Request` is not loaded in the master, so warming it triggers a file `include`
that **yields under HOOK_ALL** — the worker then accepts a request mid-warmup → cold concurrent
compile → first-wins merge orphans the loser CE → the escaped loser-CE object fails the closure's
winner-CE type-hint.

**Mitigation (verified):** warm the request-path classes in the **master** (before
`$app->run()`, pre-fork, no scheduler → blocking+atomic → COW-forked warm into every worker),
exactly as `warmBulkPreloads()` already does for the classmap. Adding `class_exists()` for the
request-path classes to wp5app's `app.php` eliminated the TypeError. Candidate framework fix:
move `preloadRequestPathClasses()` out of the onWorkerStart coroutine into the master alongside
`warmBulkPreloads()` (or load it before HOOK_ALL is active).

### Layer 2 — `zend_mm_heap corrupted` (signal=6/11) — the real frontier

Unmasked once Layer 1 is mitigated. Symbolized backtrace (gdb-as-parent, follow-fork):

```
zend_mm_panic ("zend_mm_heap corrupted")
 _efree  <-  _php_stream_free(close_options=27)
 mysqlnd_vio_close_stream_pub        (mysqlnd_vio.c:680)
 mysqlnd_conn_data_send_close_pub    (mysqlnd_connection.c:1107)
 mysqlnd_conn_close_pub (MYSQLND_CLOSE_EXPLICIT)
 php_mysqli_close [mysqli.so]
 zend_object_dtor_property  <-  zend_object_std_dtor
 rc_dtor_func  (refcount hit 0)
 zval_ptr_dtor
 zealphp_globals_reset_to_parent()   at zealphp.c:1190
 zend_closure_internal_handler
```

`zealphp_globals_reset_to_parent()` drops the **last** ref to a `$wpdb` object global → its
mysqli `__destruct` closes the connection → mysqlnd stream double-free → heap corruption.
WordPress is **not** using a persistent connection (no `p:` prefix), so it is not a shared-stream
double-free across requests. The save path correctly addrefs the object into the per-coroutine
delta (`ZVAL_DUP` → addref for objects, line ~1290) **before** `reset_to_parent` (line ~1322),
so this is a subtle **refcount double-free in the object-globals isolation ↔ mysqlnd teardown
lifecycle under concurrency** — the documented "mysqlnd/libtasn1 connection-teardown heap-overflow"
frontier.

**Falsified hypothesis (do not re-try without new evidence):** a yield-mid-iteration timing bug
(the dtor at 1190 yielding mid-scan of the shared `EG(symbol_table)` while a peer's reinstall
resizes the table). A deferred-dtor patch (collect displaced values, dtor only after the scan +
reinstall complete) was built and tested — **crash unchanged**, so timing is not the cause. Patch
reverted. A correct fix needs ASAN refcount tracing of the object-global delta lifecycle.
Until then, `legacy-cgi` / `cgi-pool` remain the conservative home for production WordPress.

## Separate latent bug found (leak, not the crash)

`zealphp_coro_constant_deferred` is **written** keyed by `os_get_cid()` (small int) in
`zealphp_constants_clear()` (lines ~2884/2888) but **read/freed** keyed by `(uintptr_t)arg`
(pointer) in `zealphp_on_close()` (lines ~1767/1777). The two key spaces never collide, so
request constants orphaned by `constants_clear()` are never freed by `on_close` → a per-request
leak of deferred-orphaned `zend_constant` structs (bounded by worker recycle). Fix: in
`on_close`, look up the deferred table by `os_get_cid()` (valid in a coroutine's own close
callback, as already used a few lines above) to match the producer key. Validate with the phpt
suite + ASAN before shipping.

## Build / debug gotchas (for the next session)

- **Versioned builds:** `/build/ext341` and `/build/ext41` Makefiles both bake in
  `srcdir=/build/ext41` — a `cp -r` build dir compiles ext41's source and installs to ext41/modules.
  Build in `/build/ext41` and verify the **live `.so`** via
  `strings zealphp.so | grep 'SKIP(owned)'` (v0.3.41) or `'SKIP(not-request-ptr)'` (v0.3.42).
  The `PHP_ZEALPHP_VERSION` macro lied (0.3.41 source built reported 0.3.42).
- **Robust restart:** `php app.php stop; pkill -9 -f 'php app.php'; wait-for-:PORT-free` — else
  duplicate-start detection serves the old worker and the "new" binary never boots
  ("ZealPHP is already running" / HTTP 000).
- **gdb in the container:** attach is blocked (no CAP_SYS_PTRACE, ptrace_scope=1, core_pattern
  read-only). gdb-as-PARENT works (PTRACE_TRACEME):
  `gdb -batch -ex 'set follow-fork-mode child' -ex 'set detach-on-fork on'
  -ex 'catch signal SIGABRT SIGSEGV' -ex run --args php app.php` rides the
  master→manager→worker fork chain to the worker. Use `-ex run` (not `-ex 'run start'`, which
  replaces the program args).
- Stable repro binaries left in place: `/build/so_v341.so`, `/build/so_v342.so`,
  `/build/so_v342fix.so` (the falsified deferred-dtor build). Repro harnesses:
  `/t40/run_test.sh`, `/t40/start_load.sh`, `/wp5app/wptest.sh`, `/wp5app/gdbparent.sh`.
