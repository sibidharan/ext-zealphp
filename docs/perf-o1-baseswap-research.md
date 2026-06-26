# Coroutine-legacy O(1) isolation via map_ptr base-swap — research findings

**Status:** INVESTIGATED TO CONCLUSION — **rtcache-unsafe by construction; not
viable. S5a remains the mechanism.** · **Date:** 2026-06-25 (concluded 2026-06-26)

This records a deep investigation into whether coroutine-legacy's per-yield
isolation overhead can be made O(1) by swapping PHP's `CG(map_ptr_base)` per
coroutine instead of copying static state. Captured so the dead ends and the
verdict aren't re-tread.

> **TL;DR / verdict (read §6f first).** The base-swap *does* isolate per-coroutine
> state in the sequential case, but it is **memory-unsafe under concurrency by
> construction**: one flat `CG(map_ptr_base)` cannot give `run_time_cache` its
> "warm, never-NULL, shared" contract AND give `static_variables` their "fresh
> per-coroutine" contract — both are read through the same `base+offset`. Zeroing
> the per-coroutine table crashes frequently (NULL rtcache deref); full-table
> memcpy makes it rare but **not gone** (inherits master's NULL slots for
> compiled-but-never-called functions → SIGSEGV on first touch). A clean A/B
> (`NOBASE` = bookkeeping only, base never swapped → **zero crashes**) proves the
> *base swap itself* is the cause. **Conclusion: the O(1) base-swap is off the
> table for safety reasons; `S5a` (the O(Y·S) static-only snapshot) is correct
> precisely because it never repoints the rtcache base.** §1–§5 are the original
> design/exploration (some hypotheses later refuted); §6a–§6f are the 2026-06-25/26
> empirical investigation that reached this verdict — §6f is the bottom line.

## 1. The cost being attacked (measured)

coroutine-legacy is **4.7×** slower than plain coroutine mode on a 20-yield
route (2,839 vs 13,309 req/s, c=100). Stage attribution: **S5a function-static
snapshot = +28%** (dominant), the rest is S1 superglobals + the on_yield/on_resume
dispatch. Empirically the per-request cost is **O(Y·S)** — linear in *yields* ×
*isolation-state size*: growing the fn-static registry with N dummy functions,
latency ≈ `27ms + 0.073ms·N` at Y=20 (dead linear in N). A bigger app is slower
per request even at the same yield count.

The snapshot is conservative-by-necessity: the runtime can't predict a coroutine's
static working-set, and it can't cheaply detect writes (PHP exposes no
write-barrier; `ZEND_BIND_STATIC` only fires on first bind). So it snapshots ALL
static-using functions every yield. Fidelity is non-negotiable (dropping it =
cross-coroutine state leaks), so the O(Y·S) can't be shrunk by scoping.

## 2. The O(1) idea

PHP stores per-process op_array state (function statics, class static members,
run_time_cache, CE-cache) behind a single relocatable base pointer
`CG(map_ptr_base)` — offset slots read `*(base + offset)`. Give each coroutine its
own map_ptr table and swap the base on yield/resume: **one pointer write isolates
all of it at once.** O(1) per yield, replacing the O(Y·S) snapshot AND the
O(symbols) per-request reset. FPM-faithful (a fresh table = a fresh process's
empty statics + cold caches).

## 3. What was PROVEN (probes built + run on PHP 8.4.5)

- **Relocate primitive works.** Swapping `CG(map_ptr_base)` to a malloc'd copy at
  runtime preserves correct static + class access, no crash (opcache on/off).
- **Immutable op_arrays are offset-based.** In an opcache server a function shows
  `immutable:true, static_offset:1, rtcache_offset:1` — statics + run_time_caches
  ARE swappable via the base. (Non-immutable op_arrays use **direct pointers** —
  `static_offset:0` — so they're immune to the swap. This is why **opcache is a
  hard prerequisite**, which in turn requires **Option A / #12** so Stage-7
  re-executed files stay cached/immutable instead of recompiling non-immutable per
  request; and **#22214** so opcache + coroutine-legacy works at all.)
- **The base-swap isolates function statics correctly.** With a per-coroutine
  swap in on_yield/on_resume, a `static $x` returns `1` on every sequential
  request (`val: 1 1 1`) — fresh table per coroutine, FPM-faithful. The O(1)
  isolation MECHANISM is real and correct for the sequential case.

## 4. Why it is NOT shippable (the frontier)

A prototype (`ZEALPHP_MPTR_SWAP`, default off) works sequentially but fails under
concurrency. The open problems, each non-trivial:

1. **Concurrency correctness — ROOT-CAUSED 2026-06-25 (see §6): the first-run
   window.** Under c≥16 one of N concurrent requests **hangs** (orphaned
   coroutine), not a crash. The base must be the right coroutine's table at every
   PHP-execution window. The naive prototype swaps only in `on_resume`/`on_yield`,
   but **OpenSwoole does NOT fire `on_resume` on a coroutine's FIRST run**
   (`Coroutine::run()` calls `ctx.swap_in()` directly) — so a brand-new coroutine
   executes its first slice with whatever base the previously-running context left
   (master, or a peer's table that may have been realloc'd/freed) → garbage slot
   read → hang. The fix is a per-coroutine *start* hook, not a deeper swap hook
   (§6).
2. **Table growth.** `map_ptr_last` (dynamic) AND the static CE-cache region grow
   whenever a request lazily compiles a path or resolves a new type. The prototype
   fixes `zealphp_mptr_total` at activation, so a later growth makes per-coroutine
   tables go OOB. Needs growth re-sync (re-alloc the coro table on resume/start
   when `CG(map_ptr_size)` grew) — the warmup + stability-gate workaround can't
   cover real apps.
3. **run_time_cache lifecycle + the warm-vs-cold tension (the perf-win question).**
   The prototype *zeroes* the dynamic region per coroutine → fresh statics (✓
   correct, FPM-faithful) BUT every coroutine then re-resolves **all** its
   run_time_caches cold each request — exactly what opcache exists to avoid. For a
   large app (big `F` = functions-touched) that O(F)-per-request cold-resolve can
   **wash out or exceed** the O(Y·S) static-snapshot cost it is meant to replace,
   so the base-swap is not obviously a net win. Keeping rtcache *warm* (memcpy the
   master table) is unsafe wherever an rtcache slot caches a pointer **into** a
   per-coroutine static slot (the same coupling the S11 `run_time_cache` reset
   already manages). Because `run_time_cache` and `static_variables_ptr` are both
   `ZEND_MAP_PTR_DEF` (one base, different offsets, interleaved — confirmed in
   `zend_compile.h`), a single base cannot separate "want-shared rtcache" from
   "want-per-coro statics" without per-slot semantic classification — which
   reintroduces O(slots) work and partially defeats the O(1) goal. **This is the
   central open question, not just a lifecycle detail.**
4. **Boot-vs-request statics.** A fresh table re-binds boot-function statics from
   template (diverges from the deliberate boot-static preservation). Needs per-slot
   boot/request partitioning (copy boot, zero request) — same region-granularity
   problem as #3.

## 5. Recommendation

The O(1) base-swap is the right long-term direction — it's how a native engine
impl would do per-coroutine isolation — and the core mechanism is **proven**. The
2026-06-25 OpenSwoole study (§6) sharpens the path:

- **The callbacks ARE a viable carrier** (revises the earlier "must hook the actual
  save/restore" framing). OpenSwoole's `on_resume` callback *is* the EG restore and
  runs `restore_task` synchronously **before** `ctx.swap_in()`, so swapping
  `CG(map_ptr_base)` right after `orig_on_resume` is **atomic with the EG swap — no
  window**. ext-zealphp already proves CG-state can ride the callbacks correctly
  (the HAZARD-2 `CG(function_table)`/`CG(class_table)` stash/reapply at
  `zealphp.c:2487-2495`/`2573-2585`).
- **But the resume path alone is insufficient** — close the first-run window with
  `OSW_GLOBAL_HOOK_ON_CORO_START` (+ `ON_CORO_STOP`), and add table-growth re-sync.
- **And resolve the perf-win question (§4.3) BEFORE investing** — measure whether a
  zero-the-table swap actually beats S5a on a real (rtcache-heavy) app, or whether
  the cold-rtcache tax eats the win. If it does, the swap needs a warm-rtcache /
  per-coro-statics split that the single base can't give cheaply.

Gated on opcache (#22214) + Option A (#12, shipped in v0.3.59), with
ASAN/Valgrind/TrustBar as the gates. Until then: coroutine-legacy's overhead is the
price of the compatibility runtime; the practical levers are coroutine mode where
legacy compat isn't needed, fewer yields per request, and more workers.

## 6. OpenSwoole v26.2.0 context-switch findings (2026-06-25)

Source study of `openswoole/ext-openswoole@v26.2.0`. The hooks
`Coroutine::set_on_yield/on_resume/on_close` are **single global function
pointers** (not a stack); OpenSwoole installs its own EG-save/restore there, so
ext-zealphp must chain (save old ptr, call it) — which it does.

| Q | Finding | Source |
|---|---------|--------|
| **Ordering** | `on_resume` **is** the EG restore: runs `restore_task(target)` synchronously **before** `ctx.swap_in()`. Chained code after `orig_on_resume` sees EG already = target's, and the jump lands with EG correct. No PHP runs between the write and `jump_fcontext`. | `src/coroutine/base.cc:105-117`; `ext-src/openswoole_coroutine.cc:576-586` |
| **What's swapped** | Only `EG(...)` (vm_stack family, current_execute_data, stack_base/limit, error_handling, exception\*) + whole `OG(...)`. **ZERO `CG(...)`, ZERO map_ptr** (grep-confirmed repo-wide). So `map_ptr_base` is entirely ours to manage. | `ext-src/openswoole_coroutine.cc:470-521` |
| **In-swap hook?** | **None** — `jump_fcontext` is bare. Only hooks: the 3 pre-jump SwapCallbacks + once-per-life `ON_CORO_START`/`ON_CORO_STOP`. The per-coro `PHPContext` struct is the natural snapshot carrier but **can't be extended without forking** the ext → use a **cid-keyed side-table** (`task->co->get_cid()`), which the prototype already does. | `src/coroutine/context.cc:187-214`; `ext-src/php_openswoole_coroutine.h:49-75` |
| **First run** | **`on_resume` does NOT fire on first run** — `Coroutine::run()` calls `ctx.swap_in()` directly, skipping the callback. There **is** a first-run window. This is almost certainly the hang. | `include/openswoole_coroutine.h:235-243` vs `base.cc:105-117` |
| **go()/timers** | Identical paths — one yield / one resume path for all kinds. First-run-no-`on_resume` is the only variant. | `base.cc:75-77` |

**Corrected hook scheme (the design to validate when the repro rig is up):**

- `on_yield` (before `orig_on_yield`): re-park `CG(map_ptr_base)` to master.
- `on_resume` (after `orig_on_resume`): set base to this coroutine's table — atomic
  with the EG restore, no window.
- **`ON_CORO_START`** (fires on the child's own stack before any user PHP,
  `ext-src/openswoole_coroutine.cc:760`): create + set the coroutine's table —
  **this closes the first-run window (frontier #1).**
- **`ON_CORO_STOP`/`on_close`**: free the coroutine table and re-park the base to
  master (an inline child that returns without yielding leaves its base live for
  the resuming origin otherwise).
- On `on_resume`/`ON_CORO_START`: if `CG(map_ptr_size)` grew past the table's
  capacity, realloc + re-copy the static region (frontier #2).

Open before any of this is worth shipping: the **warm-vs-cold rtcache** question
(§4.3) — the swap must demonstrably beat S5a on a real app, not just isolate
correctly.

### 6a. ON_CORO_START/STOP recipe (buildable on installed OpenSwoole 26.2.0)

Confirmed against the installed `openswoole.so` (exports `openswoole_add_hook` @
0x10cab0). Unlike `set_on_yield/resume/close` (single global fn-ptr → must chain),
the coro-start/stop hooks are a **`std::list<Callback>` — additive, no clobber**.

- **Enum** (`include/openswoole_c_api.h`, `enum swGlobalHookType`):
  `OSW_GLOBAL_HOOK_ON_CORO_START` = 3, `OSW_GLOBAL_HOOK_ON_CORO_STOP` = 4
  (0-based declaration order). dlsym can't see a compile-time enum, so the C ext
  uses the literal values with a version comment (already hardcodes OpenSwoole ABI
  assumptions elsewhere).
- **Register** (once at worker init; dlsym `openswoole_add_hook`):
  `int openswoole_add_hook(enum swGlobalHookType, void(*cb)(void*), int push_back)`
  — `push_back=1` appends (runs after OpenSwoole's own cbs); the list is iterated in
  order (`src/core/base.cc:899-904`), so we never clobber.
- **START callback** `void cb(void *arg)` — fires on the **child's own stack, EG
  already the child's, BEFORE user PHP** (`ext-src/openswoole_coroutine.cc:761-763`;
  `main_func` already did `vm_stack_init`/`save_vm_stack` at :657/:730). `arg` is
  `PHPContext*`, but from C the simplest cid source is **`os_get_cid()`** (already
  dlsym'd) — `current` is the child here, so it returns the child's cid. **This is
  where we create + install the child's map_ptr table — closing the first-run
  window.**
- **STOP callback** — fires inside `on_close` BEFORE the final `restore_task`
  (`ext-src/openswoole_coroutine.cc:597-599`), while the dying coro is still
  `current` (so `os_get_cid()` = the dying cid). Free that cid's table; re-park the
  base. NB: the origin resumes via the close path (`current=origin; delete`), **not**
  a scheduler `on_resume`, so the origin gets NO on_resume — STOP must restore the
  origin's base (via `task->pcid` parent lookup) or the parent runs post-child on
  the wrong base. **This parent-resume-without-on_resume is the symmetric half of
  the first-run window and must be validated explicitly.**

Net hook scheme: `on_yield`→re-park master · `on_resume`→set base by cid · **START
→create+set child base** · **STOP→free + restore origin base** · growth re-sync on
START/resume. Minimal correctness test (no load rig needed): a `go()` child that
reads a function `static` on its FIRST slice must see a FRESH value (its own table),
not master's/a peer's — fails without START, passes with it.

### 6b. Empirical validation 2026-06-25 — START fix BUILT and INSUFFICIENT

Implemented the `ON_CORO_START` hook on the dev rig (OpenSwoole 26.2.0, opcache on,
`app_swap.php`: `/y20` = 20 yields, `/val` = a function `static`). The hook
registers via `openswoole_add_hook` (additive `std::list`, no clobber) and **fires
confirmed** (`[MPTR] coro_start arg=… cid=…`). But the hang **persists**:

| Build | 16-way concurrent `/y20` |
|---|---|
| **swap OFF** (baseline) | **16/16 → 200**, 1.6 s — no hang |
| **swap ON + START fix** | **5/16 → 200, 11/16 hang** (6 s timeout, no crash, no cores) |

So the base-swap **causes** the hang and **closing the first-run window does not fix
it** — the "almost certainly why it hangs" hypothesis is refuted as the *sole*
cause. The fix is correct and necessary for one window, but not sufficient.

**Sharpened root cause — base RELOCATION / compile-during-swap (frontier #2, not
#1).** The prototype captures `zealphp_mptr_master = CG(map_ptr_real_base)` ONCE at
activation. But `CG(map_ptr_real_base)` is **`erealloc`-relocatable**: any
post-activation lazy compile (`map_ptr_extend`) moves it → the captured master
dangles (every per-coro `memcpy` source + every re-park target points at freed
memory) → hang/corruption. Worse, while a coroutine runs **swapped onto its own
`calloc`'d table**, an engine compile would `erealloc` *that* table (a malloc'd
pointer handed to the Zend allocator — wrong allocator) and peers/master never see
the growth. The "40-stable-yields" activation gate cannot guarantee no later
compile, so it only narrows the window. This is the real frontier.

**Next-step design (for the relocation frontier):**
1. **Stable master address** — pre-reserve a large `map_ptr` table at activation
   (force `CG(map_ptr_size)` high once, up front) so it never relocates; OR detect
   relocation (`CG(map_ptr_real_base) != zealphp_mptr_master`) on every resume/yield
   and re-sync master + realloc all live coro tables.
2. **No compile while swapped** — re-park the base to master around any
   compile/include (hook the compile boundary, or keep HOOK_FILE off so re-executed
   files don't recompile under the swap). With Option A (#12) caching re-executed
   op_arrays, post-warmup compiles should be rare — but "rare" still corrupts.

Until #1+#2 land, the swap is not shippable. The START hook (6a) is a correct
prerequisite to keep; the hang is a *separate*, harder frontier. Artifacts on the
dev rig: `/root/ext-build` (patched build, `ZEALPHP_MPTR_SWAP` + START hook),
`/root/bug12/app_swap.php` + the swap-off/on A/B above.

### 6c. Deep dive 2026-06-25 (gdb core) — it is a SIGSEGV, root-caused, and FIXED; a separate hang remains

**The §6b "relocation" hypothesis is SUPERSEDED by direct evidence.** ptrace-attach is
blocked in the rig container (no `CAP_SYS_PTRACE`, yama=1), and OpenSwoole's own segv
handler + apport `core_pattern` swallowed cores. Redirecting `core_pattern` to a plain
file path (host-global, restored after) captured a core; **ptrace-free post-mortem**
`gdb php8.4 <core>` gave the smoking gun:

```
#0 execute_ex   (← zend_call_function ← ?? ← execute_ex ← PHPCoroutine::main_func)
fault_addr = (nil)            insn: mov 0x0(%r13),%rax   with r13 = 0   → NULL deref
map_ptr state: zealphp_mptr_on=true  master=0x..cd67760  total=4096
               map_ptr_real_base=0x..dbf2090  map_ptr_size=4096
```

`map_ptr_size (4096) == total (4096)` → **NO table growth / NO relocation** (frontier
#2 ruled out for this app — §6b was wrong). The crash is a **NULL `run_time_cache`
dereference**: the prototype `calloc`s each per-coroutine table **zeroed** (and only
memcpy'd the static region, here empty), so every rtcache slot is NULL. The engine
assumes a once-allocated rtcache stays non-NULL for the op_array's life; a zeroed
per-coro table violates that, and the VM dereferences the NULL slot → SIGSEGV. (This
is exactly the §4.3 warm-vs-cold tension, now proven memory-unsafe — zeroing the
dynamic region is a bug, not just a perf choice.)

**FIX (validated): memcpy the FULL master table** (not just the static region) into
each coro table, so rtcache slots inherit master's resolved, non-NULL caches:

| Build | result |
|---|---|
| zeroed table (prototype) | **30+ SIGSEGV** under c16, workers respawn-storm |
| **full-table memcpy** | **0 SIGSEGV, 0 cores** at c16/c24; c6 → 6/6 → 200 |

Full-memcpy is the *correct* rtcache design, not just a crash patch: inheriting
master's resolved pointers kills (a) the NULL-deref crash, (b) the per-coro
cold-resolve tax, and (c) the per-coro rtcache leak — all three §3/§4.3 concerns at
once. The remaining per-coro divergence (statics) is a separate, smaller surface.

**Remaining: a SEPARATE WAIT hang at c≥16 (NOT the crash, NOT the first-run window).**
With full-memcpy, `signal=11`=0 and 0 cores, but c16 still does not complete: all
workers sit **idle in `ep_poll` (0% CPU, no respawn)** with the requests pending — the
classic "orphaned coroutine". It is **load-threshold**: c6 is clean, c16 hangs, i.e.
it appears only once several coroutines multiplex on one worker (>~2–4 coro/worker).
So the base-swap corrupts something in the **scheduler/resume path under coroutine
interleaving** — orthogonal to the (now-fixed) memory-safety crash. The hints driving
the next pass: *stay strictly inside request-coroutine context* (the swap may be
leaking into internal/service coroutines or the reactor) and *never key on
`os_get_cid()`* (it is −1 in on_resume; key on the `PHPContext*` `arg`).

**Tooling note for the next pass:** the idle-worker hang yields no core, and
DBG-per-event tracing floods the log + stalls under the rig's flaky tunnel. The next
diagnostic is **low-volume per-worker lifecycle counters** (Y/R/S/C + table-creates),
read via a `SIGABRT`-forced core or a `/stats` route, to detect a yield-without-resume
imbalance under multiplexing — data the per-event trace couldn't deliver. Statics
isolation + the perf benchmark (the actual "is it a win" question, §4.3) are
downstream of resolving this hang.

Net status: **crash root-caused + fixed (memory-safe under load); the O(1) goal now
hinges solely on the coroutine-multiplexing hang.**

### 6d. Counter instrumentation — the hang is a timer/scheduler-WAKE subtlety

Added per-worker lifecycle counters (`nstart/nyield/nresume/nclose/ncreate`), read
ptrace-free from a **`SIGABRT`-forced core** of a wedged worker mid-hang (c16):

```
nstart=6  nyield=32  nresume=30  nclose=7  ncreate=8   (mptr_on=true)
worker bt: idle in epoll_wait (start_event_worker → Reactor::wait)
```

Two hypotheses **refuted** by this:
- **Resumes DO fire** — `nresume (30) ≈ nyield (32)`; only ~2 coroutines suspended at
  the snapshot. Not a "coroutines never resume" failure.
- **Args are consistent** — `ncreate (8) ≈ nstart (6)`, NOT `≈ nresume (30)`. So
  `ON_CORO_START`'s `arg` == `on_resume`'s `arg`; the side-table never re-creates a
  table per resume. The "key on `arg`, never `os_get_cid()`" rule is being honored
  and is not the bug.

What's left: a couple of multiplexed coroutines **yield on their `Co::usleep` timer
and the reactor (idle in `epoll_wait`) never wakes to resume them** → the request
never completes. It is load-threshold (c6 clean, c16 hangs) i.e. it only bites once
≥~2–4 coroutines interleave on one worker. So the base-swap perturbs the
timer/scheduler **wake** path under interleaving — NOT base keying, NOT resume firing,
NOT memory safety. (`nyield`=32 is small because activation lands late in warmup; the
hang coincides with activation happening *while* several coroutines are in flight —
a strong lead: the activation/first-swap transition under concurrency.)

**Next diagnostic (dedicated session):** either (a) a fixed-size in-ext **ring buffer**
of `{event, arg, opline?}` dumped from a `SIGABRT` core — to read the exact
yield→(missing)resume tail for one stuck coroutine without log flood; or (b) an
**OpenSwoole built with debug symbols** so the core's timer-wheel + suspended-coroutine
list are inspectable, to see whether the stuck coroutines' timers were ever armed.
Rig state preserved: `/root/ext-build` (full-memcpy + START + counters build),
`/root/bug12/app_swap.php`, `ZEALPHP_MPTR_SWAP=1`.

### 6e. DECISIVE A/B — the hang is the `CG(map_ptr_base)` SWAP itself, not the bookkeeping

Added a `ZEALPHP_MPTR_NOBASE` toggle: do **all** the per-coroutine bookkeeping
(`calloc`+`memcpy`+hash add/find/del, START/yield/resume/close) but make
`zealphp_mptr_set_base()` a **no-op** (never touch `CG(map_ptr_base)`). Measured with a
**direct foreground probe** (see the methodology note below):

| Build (full-memcpy + START + counters) | direct c16 |
|---|---|
| `NOBASE=1` — bookkeeping only, base NEVER swapped | **16/16 → 200 in 19 ms** ✅ |
| `SWAP=1` — real per-coroutine base swap | **HANG** (2-min timeout, `signal=11`=0) ❌ |

**Conclusion: the hang is caused by changing `CG(map_ptr_base)` per coroutine under
multiplexing — full stop.** The table churn / hash ops / `arg` keying / START hook are
all innocent (NOBASE exercises every one of them at c16 with zero hang). This is the
8th and final hypothesis standing after refuting: first-run window, table growth /
relocation, the NULL-rtcache crash (separate, fixed), arg mismatch, resumes-never-fire,
activation timing, and bookkeeping churn. Something in the OpenSwoole scheduler / Zend
VM **assumes `map_ptr_base` is stable across the coroutines time-sharing a worker**, and
swapping it per coroutine wedges request completion once ≥~4 coroutines interleave
(c6 clean, c16 hangs). The crash fix (full-memcpy) is real and orthogonal; the O(1)
goal is blocked solely on this base-stability assumption.

> **⚠️ Methodology correction:** earlier "c16 hangs" runs used a detached
> `nohup` + `for…&;wait` + redirect harness that **itself stalls** (false hang) — the
> NOBASE arm "stalled" in the harness yet the **direct foreground probe returned 16/16
> in 19 ms**. Always measure these with a direct foreground `for i…; do curl -m8 … & done;
> wait` and explicit code counts; do not trust the detached-script stall as a hang
> signal. (The SWAP-arm hang IS real — confirmed by the same direct probe.)

**True next step:** find WHO reads `map_ptr_base` and assumes stability across a worker's
coroutines. Likely candidates: a cached `EX(run_time_cache)`/base-derived pointer that
survives a context switch, or OpenSwoole/Zend scheduler-side op_array access between
switches. Needs an **OpenSwoole + PHP debug-symbol build** to walk a suspended
coroutine's fiber stack in a `SIGABRT` core and see exactly where it is parked. Until
then the base-swap is correct (memory-safe, isolating) but **not concurrency-viable**;
the practical O(Y·S) snapshot path (S5a) remains the shipping mechanism.

### 6f. CORRECTION — it is an intermittent CRASH, full-memcpy is PARTIAL, and the rtcache/statics tension is a SAFETY limit

Bisecting the "hang" (bounded direct probes) corrected two of my own framings:

1. **It is not a hang — it is a rare intermittent CRASH.** Bounded c16 probes returned
   `13×200 + 3×000` with **`signal=11`=2**; c24 returned `24×200`. The earlier "hang"
   was (a) crash-induced unresponsiveness (a dying worker leaves the connection unanswered)
   plus (b) the detached-harness false-stall. With a **direct probe + a core**, the failure
   is unambiguous: a worker SIGSEGVs.

2. **Full-memcpy is a PARTIAL fix, not a complete one.** The residual core is the *same*
   crash as §6c — `execute_ex+6406`, `fault=(nil)`, `mov 0x0(%r13),%rax` with `r13=0`,
   **no growth** (`map_ptr_size==total`). Full-memcpy cut frequency ~30+ → ~1–2 per c16
   burst (the earlier `signal=11`=0 was one lucky run), but did not eliminate it.

**Root cause of the residual (the important part):** full-memcpy inherits master's table,
but **master has NULL run_time_cache slots for every function it *compiled but never
called* during warmup** (the offset is assigned at compile; the cache value stays NULL
until first call). A coroutine that is the FIRST to call such a function reads a NULL
rtcache slot and dereferences it → SIGSEGV. `NOBASE` (do all bookkeeping, never change
the base) **never crashes**; `NOYIELDPARK` (don't re-park on yield) does NOT change it
(re-park innocent). So the crash is intrinsic to per-coroutine base-swapping.

**This is the §4.3 tension as a SAFETY limit, not just perf.** One flat `map_ptr` base
cannot simultaneously give rtcache its "warm, never-NULL, shared" contract AND statics
their "fresh per-coroutine" contract:
- **zero the table** → every rtcache NULL → frequent crash;
- **full-memcpy** → inherits master's NULL slots (compiled-not-called fns) → rare crash;
- there is no per-slot split because the engine reads both rtcache and `static_variables`
  through the same `CG(map_ptr_base)+offset`.

So the base-swap is **not just concurrency-fragile, it is rtcache-unsafe by construction**
under per-coroutine isolation. Making it viable requires one of: (a) the engine to
robustly self-heal a NULL rtcache slot under an active base swap (so a first-touch by a
coroutine never crashes) — engine-level work with a real window to close; (b) abandon the
single-flat-base swap and isolate statics by a mechanism that leaves rtcache shared (i.e.
back to a targeted per-yield approach — which is what S5a already is); or (c) pre-warm
master so it has NO NULL slots before activation (impractical for real apps — can't call
every compiled function). 

**Net verdict:** the O(1) base-swap, even with the crash made rare, is **not memory-safe
under per-coroutine isolation** for the rtcache region. S5a (the O(Y·S) snapshot) remains
the correct shipping mechanism precisely because it touches ONLY the static working set
and never repoints the rtcache base. The base-swap is parked here with the failure mode
fully understood, not mysterious.
