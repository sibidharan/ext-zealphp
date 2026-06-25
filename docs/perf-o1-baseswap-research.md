# Coroutine-legacy O(1) isolation via map_ptr base-swap — research findings

**Status:** validated concept / NOT shippable · **Date:** 2026-06-25

This records a deep investigation into whether coroutine-legacy's per-yield
isolation overhead can be made O(1) by swapping PHP's `CG(map_ptr_base)` per
coroutine instead of copying static state. The concept is validated; making it
correct + safe is a dedicated multi-week engine project. Captured so the dead
ends and the real path aren't re-tread.

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

1. **Concurrency correctness.** Under c≥16 one of N concurrent requests **hangs**
   (orphaned coroutine), not a crash. The base must be the exact right coroutine's
   table at *every* PHP-execution window under OpenSwoole's scheduler — including
   go() children, timer/I/O-completion callbacks, and the framework's own
   response handling between yields. The yield/resume callbacks don't cleanly
   bracket all of these.
2. **Table growth.** `map_ptr_last` (dynamic) AND the static CE-cache region grow
   whenever a request lazily compiles a path or resolves a new type. Fixed-size
   per-coroutine tables then go OOB. Needs growth re-sync (re-alloc all live coro
   tables on extend) — the warmup + stability-gate workaround can't cover real
   apps.
3. **run_time_cache lifecycle.** Zeroed per-coroutine slots → the engine
   re-allocates caches per coroutine; freeing the table leaks them. Needs explicit
   per-coroutine cache lifecycle.
4. **Boot-vs-request statics.** A fresh table re-binds boot-function statics from
   template (diverges from the current design's deliberate boot-static
   preservation). Needs per-slot boot/request partitioning (copy boot, zero
   request).

## 5. Recommendation

The O(1) base-swap is the right long-term direction — it's how a native engine
impl would do per-coroutine isolation — and the core mechanism is **proven**. But
it's a scoped multi-week project gated on opcache (#22214) + Option A (#12), and
must hook OpenSwoole's actual context save/restore (not the yield/resume
callbacks) for concurrency correctness, with ASAN/Valgrind/TrustBar as the gates.
Until then: coroutine-legacy's overhead is the price of the compatibility runtime;
the practical levers are use coroutine mode where legacy compat isn't needed,
fewer yields per request, and more workers.
