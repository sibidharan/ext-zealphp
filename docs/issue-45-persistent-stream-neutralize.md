# ext#45 — persistent userland streams heap-corrupt under HOOK_ALL

**Status:** SHIPPED in ext-zealphp 0.3.55.
**Symptom:** `zend_mm_heap corrupted` (or, under `USE_ZEND_ALLOC=0`, a glibc
`free()`/`corrupted` abort) at script/worker teardown whenever a **persistent
userland socket** is created while OpenSwoole's coroutine transport hooks are
active (`Runtime::enableCoroutine(HOOK_ALL)`).

Two userland entry points create a persistent-keyed stream:

- `pfsockopen($host, $port, …)` — always persistent (it is literally
  `fsockopen` with `persistent = 1`).
- `stream_socket_client($addr, …, flags)` with `STREAM_CLIENT_PERSISTENT` set in
  `flags`.

## Root cause

This is the **generic-path sibling** of the mysqlnd vio crash (ext#44). The
mechanism is identical, one layer down:

1. A persistent stream asks the generic xport layer to remember its address:
   `stream->orig_path = pestrdup(path, persistent)` — with `persistent = 1` it is
   allocated with **malloc** (the persistent/`pemalloc` allocator).
2. Under `HOOK_ALL`, OpenSwoole replaces the tcp/unix transport factory. The
   hooked factory hands back an **emalloc'd, NON-persistent** `php_stream`.
3. At teardown `php_stream_free()` does `pefree(stream->orig_path, stream->is_persistent)`.
   Because the *stream* is non-persistent, `is_persistent = 0`, so the malloc'd
   `orig_path` is freed with **`_efree`** → freeing a malloc pointer through the
   Zend allocator → heap metadata corruption → SIGABRT.

The pointer is malloc-valid, so the crash is purely an allocator mismatch —
exactly the ext#44 signature, but in the generic stream path.

### Why a factory/vtable shim cannot fix it (unlike ext#44)

ext#44 was fixed by re-pairing `orig_path` with the stream's real allocator
inside a shim on mysqlnd's `mysqlnd_vio` **method table**. The generic userland
path has **no method vtable to interpose**, and crucially `orig_path` is set by
the *caller* of the factory **after the factory returns** — so even a factory
wrapper sees the stream before `orig_path` exists and cannot re-pair it.

## Fix — neutralize persistence (the doctrine-correct outcome)

A persistent socket is **already unsupported** under coroutines for a second,
independent reason: it lives in `EG(persistent_list)` and is **reused across
coroutines**, so two coroutines sharing the one fd interleave wire frames — the
same hazard as sharing a DB handle ("one connection per coroutine"). So the
correct behaviour under the coroutine hooks is simply: **don't make the socket
persistent.** That removes the crash *and* the cross-coroutine reuse in one move.

When the coroutine hooks install (`zealphp_install_coro_hooks()`, the same
activation that installs the mysqlnd vio shim), the ext saves + chains the two
internal `zif_handler`s:

- **`pfsockopen`** → dispatched to **`fsockopen`'s** handler. They share one C
  implementation (`php_fsockopen_stream`) with an identical arg layout —
  `fsockopen` just passes `persistent = 0` — so the result is a correct
  non-persistent socket.
- **`stream_socket_client`** → the wrapper clears `STREAM_CLIENT_PERSISTENT`
  (bit 0) from the **flags arg (5th positional)** before chaining to the real
  handler. The flag is a by-value `LONG` in the call frame, so the rewrite
  touches only the frame slot, never the caller's variable.

Default **on** (rides the hook activation like the mysqlnd shim). Opt out with
`ZEALPHP_PERSIST_NEUTRALIZE_DISABLE=1` for the rare expert who knows their usage
is single-coroutine and genuinely wants OS-level persistence (and accepts the
teardown crash risk under HOOK_ALL).

## Validation (PHP 8.4.21, openswoole 26.2.0)

| Scenario | Result |
|---|---|
| Fixed build (neutralize on, default) | 3×`pfsockopen` + 3×`stream_socket_client(PERSISTENT)` → **6 distinct connections** (server `accept()`s 6, not 2), clean teardown, rc=0 |
| `ZEALPHP_PERSIST_NEUTRALIZE_DISABLE=1` | `zend_mm_heap corrupted`, rc=134 (crash returns — fix is necessary) |
| Raw baseline (no zealphp hooks) | `zend_mm_heap corrupted`, rc=134 (the underlying bug) |
| `USE_ZEND_ALLOC=0` × 30 iterations | 30/30 clean `accepts=6` (no glibc free abort) |
| Full ext suite (66 tests, openswoole loaded) | 66/66 PASS, 0 failed/skipped/warned |

The behavioural observable — **server accept count** — is what makes the phpt
deterministic and bidirectional: a neutralized (non-persistent) client opens a
fresh connection per call (6 accepts); a still-persistent client reuses one fd
per kind (2 accepts) and/or crashes. Pinned by `tests/069-persistent-stream-neutralize.phpt`.
