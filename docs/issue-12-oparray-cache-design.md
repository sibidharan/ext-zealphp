# Issue #12 — Safe op_array cache: design scoping

**Status:** design / not implemented · **Owner:** TBD · **Date:** 2026-06-05
**Tracks:** [#12 — Orphaned inherited "loser" classes are never reclaimed](https://github.com/sibidharan/ext-zealphp/issues/12)

This document scopes the **only genuinely viable fix** for the #12 leak and records
the full investigation so the dead ends are not re-tread. It is a design task, not
an implementation — the recommended route is a major engine-integration effort and
should be costed before it is started.

---

## 1. Problem

Under `coroutine-legacy` (Stage 7 `includeIsolation` + silent-redeclare), a
`require_once`/`include`'d file that declares an **inherited** class (`extends` /
`implements` / `use`) is **re-compiled every request**. The Stage-4 first-wins merge
keeps the first definition (the "winner") and discards the re-compiled duplicate (the
"loser"). For an inherited loser, destroying it corrupts the live winner, so v0.3.24
**orphans** it instead — and the orphan is **never freed**.

- **Leak rate:** ~4–7 KB per inherited-class re-exec (measured on PHP 8.4 + ASAN).
- **Bound:** worker lifetime — RSS climbs until the worker recycles at `max_requests`.
- **Who hits it:** `require_once`-bootstrap apps with inherited classes on a hot path
  (WordPress, Drupal 7, MediaWiki, phpBB).

## 2. Root cause (proven — do not re-litigate)

The re-compiled inherited loser is **always early-bound against the live winner**.
At compile-end, `zend_try_early_binding` (`Zend/zend_compile.c`) resolves the parent
via **`EG(class_table)`** — where the winner lives — regardless of silent-redeclare's
CG-table swap. So the loser's resolved inheritance (method op_arrays, default-property
slots, property_info) **points into the winner's structures**. `destroy_zend_class`
(or any sub-free) on the loser frees structures the winner still uses →
`SEGV in zend_gc_addref / _object_properties_init` on the next `new` (the v0.3.24 crash).

**The orphan-and-leak is the forced, correct tradeoff** — not a shortcut.

### 2.1 Investigation log (5 ASAN iterations, all dead ends)

| # | Approach | Result |
|---|----------|--------|
| 1 | Track loser → null `parent` → `destroy_zend_class` at request-end | **SEGV** (property table) |
| 2 | + UNDEF the inherited default-property slots first | **SEGV** |
| 3 | Free only the loser's *own* method op_arrays (`scope == lce`) | **SEGV** — even "own" methods share opcodes with the winner |
| 4 | Force `ZEND_COMPILE_DELAYED_BINDING` during the compile | **No-op** — `zend_try_early_binding` still binds against EG; loser stays `linked=1` |
| 5 | Separate `require_once` files (the real scenario), DELAYED_BINDING on | **Still `linked=1`** |

The crash pointer in attempts 1–3 decoded to reused heap bytes (`"text/htm…"`),
confirming a use-after-free of winner-shared structures. Attempt 4/5 instrumentation
printed `loser linked=1 parent=<winner>` at merge time in every case.

### 2.2 Why the obvious mitigations don't apply

- **opcache:** measured only ~16% reduction (4366 → 3654 B/re-exec). opcache caches the
  *op_array compile* but the bind into the scratch CG still creates the loser.
- **Swapping `EG(class_table)` during compile** (so early-bind can't find the winner →
  loser stays unlinked → safely destroyable): **forbidden** by the silent-redeclare
  design invariant — "Stage 4 swaps CG only, NEVER EG; swapping EG corrupts coroutine
  state" (load-bearing comment at the swap site). Off the table.

## 3. The one genuine fix: don't re-compile

If the file is **not re-compiled** on re-include, **no new loser CE is created** → no
leak. This is exactly what opcache does (cache the op_array, reuse on hit). The
challenge is doing it safely — the **removed Stage-6 cache** attempted it and caused a
UAF.

### 3.1 Why the naive cache (removed Stage-6) was a UAF

> Stage-6 cached the compiled op_array and, on a later compile of the same file,
> returned a `memcpy`'d shell with `(*shell->refcount)++`. **The engine destroys the
> include op_array after executing it**, so the cached `refcount` pointer dangles — the
> `++` is a use-after-free → worker SIGSEGV under load (phpMyAdmin, 50-app sweep).

The lesson: an include op_array is **request-allocated and freed after execution**. A
cache must make it **persistent** (engine won't free it) — a shallow copy can't.

## 4. Design options

### A. Persistent op_array cache (opcache-style) — **recommended, HIGH effort**
On first compile of a redeclaration-prone file, deep-copy the op_array into a
per-worker **persistent** allocation, mark it `ZEND_ACC_IMMUTABLE` (so the engine skips
freeing it), and store it keyed by absolute path. On re-include, return the persistent
op_array — **no re-compile, no loser, no leak**.
- **Pros:** the proven mechanism — opcache's `zend_persist` does exactly this.
- **Cons:** `zend_persist` is large + PHP-version-specific (deep-copies op_array,
  literals, CVs, run-time cache, classes, functions). Replicating a correct scoped
  subset is a real project; immutable op_arrays have strict requirements (interned
  strings, no per-request run-time cache, etc.). Get it wrong → corruption.
- **Open question:** how much of `zend_persist` must be replicated vs. can engine
  helpers be reused? Can we persist only the **classes** the file declares (the leak
  source) and leave the rest request-bound?

### B. Defer to opcache — **MEDIUM-HIGH effort, conditional**
When opcache is loaded, let it own the cache; make silent-redeclare **skip the bind
into the scratch CG on a cache hit** (the file's classes already exist as winners in
EG, so there is nothing to first-wins-merge → no loser).
- **Pros:** reuses opcache's proven persistence; no in-extension persist.
- **Cons:** opcache's bind is internal to its compile hook — intercepting "this was a
  cache hit, don't create losers" cleanly is non-trivial. Requires opcache present
  (+ `dups_fix`, + the patched function-dups for the function case). Today opcache +
  silent-redeclare still leaks because the scratch bind runs regardless.
- **Open question:** can the hook detect a warm opcache hit (e.g. op_array flagged
  `ZEND_ACC_IMMUTABLE` / from SHM) and short-circuit the Stage-4 merge for it?

### C. Per-class persistence — folds into A
Mark the re-compiled CEs immutable so the engine won't free them and reuse on
re-include. Request-compiled CEs are **not** immutable-safe (request-allocated
strings/tables), so this needs the same persist work as A. **Not separately viable.**

### D. Skip the include for pure class-declaration files — **MEDIUM effort, narrow**
If a file's only top-level effect is class declarations **and** all its classes already
exist in EG, skip the re-include entirely (return a no-op op_array).
- **Cons:** unsafe for **mixed** files (require_once-bootstrap interleaves code +
  classes — the exact #12 population). Reliable "pure class file" detection is itself
  a static-analysis problem. Limited applicability; high false-negative risk.

## 5. Recommendation

1. **Short term (ship now):** accept the bounded leak. Document it honestly (correct
   the "reclaimed at request-end" comments to "bounded by worker lifetime"), recommend
   tuning `max_requests` for `require_once`-legacy workloads, and note the EG-swap
   constraint. Ship 0.3.33 with the other fixes (#26 etc.).
2. **Real fix (scoped here):** pursue **Option A** (persistent op_array cache) as a
   dedicated project, with **Option B** evaluated first as a cheaper conditional win
   for opcache deployments. Both are behind an **opt-in flag, default off**, until
   proven on the full validation matrix.

## 6. Implementation + validation plan (for whichever option)

- Land behind a fluent/env flag (`App::oparrayCache(bool)` / `ZEALPHP_OPARRAY_CACHE`),
  **default off**.
- **Correctness gates (all required):**
  - Leak repro: RSS **flat** over ≥500 inherited-class re-execs (both same-file and
    separate-`require_once` shapes).
  - **ASAN + Valgrind clean** on a 120-request burst.
  - Full ext `phpt` suite green; no regression in the silent-redeclare / Stage-4 /
    Stage-7 tests.
  - The **50-app sweep** + WordPress public/login/comment paths under `coroutine-legacy`
    — no new crashes, RSS bounded.
  - Bidirectional: confirm the cache is **HIT** (no re-compile — instrument the compile
    hook) **and** that the winner + all its instances behave identically.
- **Rollback:** the flag default-off means zero behaviour change for existing apps; the
  orphan-leak path remains the fallback.

## 7. Risks / open questions

- A: scope of `zend_persist` replication; immutable-op_array invariants on 8.3/8.4/8.5;
  per-worker persistent allocation lifetime + teardown.
- B: detecting a warm opcache hit from the hook; opcache version coupling; behaviour
  without `dups_fix` / the function-dups patch.
- Both: interaction with the existing **Stage-4 merge** and the **Stage-3
  `DECLARE_CLASS` / `DECLARE_CLASS_DELAYED` handlers**; coroutine-safety of any new
  per-worker cache (must not be crossed by a coroutine switch mid-compile, same
  invariant that governs the CG swap).

---

## 8. Re-validation on 0.3.53 (2026-06-13) — hard source proof of scope

Re-examined against the current code (ext 0.3.53, PHP 8.4.21) to refresh this
0.3.27-era doc and de-risk the eventual build. Three concrete findings, each
backed by source/measurement, all confirming: **Option A is the only safe fix
and it requires re-implementing OPcache's persist + per-request-copy machinery
(version-specific) — a multi-day dedicated project, NOT a multi-hour patch.**

1. **Leak quantified.** Stage-7 re-execution of a minimal `class Child extends
   Base { $x; m(); }` via `zealphp_include_isolation` + `zealphp_silent_redeclare`,
   2000 iterations: `memory_get_usage(true)` grows ~**2 KB per re-exec**
   (2 MB → 6 MB), bounded by `max_request` worker recycle. Matches the
   "~4–7 KB" estimate (real classes are larger). **Validation metric for any
   fix: this growth must go to ~0.**

2. **No refcount/immutable shortcut — the VM frees the struct unconditionally.**
   `ZEND_INCLUDE_OR_EVAL_SPEC_*_HANDLER` (Zend/zend_vm_execute.h, PHP 8.4.21,
   ~line 5263), after executing an include result:
   ```c
   zend_destroy_static_vars(new_op_array);
   destroy_op_array(new_op_array);
   efree_size(new_op_array, sizeof(zend_op_array));   // STRUCT freed, NO immutable guard
   ```
   There is **no `ZEND_ACC_IMMUTABLE` check** in this handler path — the include
   op_array STRUCT is always `efree`'d. This is exactly why the removed Stage-6
   cache (`f728908`: shallow `memcpy` shell + `refcount++`) UAF'd: the body's
   refcount was bumped but the *struct* was freed, so the cached pointer
   dangled. **Any approach that shares the first op_array across requests
   reintroduces this UAF.**

3. **The engine does NOT export the copy/persist primitives.** `function_add_ref`
   is not `ZEND_API` in 8.4; there is no exported `zend_op_array_copy` or
   `zend_persist_op_array` (only `zend_extensions_op_array_persist`, for
   extension-attached data — not the op_array itself). OPcache survives because
   it keeps a **persistent SHM master** and returns a **per-request copy** the VM
   safely `destroy_op_array+efree`s while the master persists. Replicating that
   in zealphp means hand-writing BOTH halves — `zend_persist_op_array` (opcodes,
   literals, CVs, static vars, the `ZEND_DECLARE_CLASS`'d classes, run_time_cache
   via `ZEND_MAP_PTR`) AND the per-request copy — keyed by realpath,
   version-specific for 8.3/8.4/8.5, with immutable invariants. Any wrong field →
   corruption.

**Conclusion:** the fix is **Option A, scoped as a multi-day dedicated build**
(re-implement persist + copy), behind `ZEALPHP_OPARRAY_CACHE` /
`App::oparrayCache()` default-off, ASAN+Valgrind-gated per §6. It is materially
larger and riskier than ext#16 (which had a surgical, validatable fix). Until it
lands, the orphan-leak stays bounded by `max_request` recycle — the documented
operational mitigation.

---

## 9. SHIPPED (0.3.54) — Option D via compiled-op_array inspection

Rather than the multi-day Option-A persist, the leak is closed for the **dominant
case** (pure class/function declaration files — incl. the WordPress
`WP_Block_Parser_Block`-style files the issue cites) by a small, corruption-free
mechanism: **don't re-execute a file that has no side effects.**

- `zealphp_oparray_cache(bool)` / `ZEALPHP_OPARRAY_CACHE=1`, **default OFF**.
- On first compile, if the file's op_array is purely declarations
  (`zealphp_op_array_is_pure_decl` — a precise COMPILED-opcode scan: only
  `NOP`/`RETURN`/`DECLARE_CLASS[_DELAYED]`/`DECLARE_ANON_CLASS`/`DECLARE_FUNCTION`/
  `DECLARE_LAMBDA_FUNCTION`; ANY other opcode ⇒ not pure), its declared symbols
  are recorded per-realpath (keyed off `ce->name` / `function_name`, not the
  NUL-prefixed delayed-binding scratch key).
- Stage 7 then SKIPS re-evicting that file while every recorded symbol is still a
  live winner in EG → it is never re-compiled → **no inherited-loser CE is created
  → no leak.** Mixed files (any side-effecting top-level code) fail the pure check,
  re-execute normally, and remain `max_request`-bounded.

**Why this is safe (no Stage-6/persist surface):** there is NO op_array sharing,
copying, persisting, or freeing — only a decision to leave a file cached. The
worst-case failure of a misclassification is a behavioural bug (a side-effecting
file wrongly skipped), which the conservative opcode scan prevents; it can never
corrupt the heap.

**Validated (PHP 8.4.21):** `tests/068` bidirectional (flag OFF ⇒ ~2 KB/re-exec
leak, flag ON ⇒ **0 B/re-exec**, class stays usable, mixed file re-executes every
request); 25,000-iteration `USE_ZEND_ALLOC=0` stress ⇒ 0 leak, 0 heap errors,
clean MSHUTDOWN; full ext suite 65/65.

**Remaining (Option A territory):** MIXED files (per-request code + an inherited
class) still re-compile and orphan-leak, bounded by `max_request`. Closing that
needs the persist + per-request-copy build scoped in §6–§8 — a separate effort.

---

*Companion analysis: the leak, the entanglement proof, and the 5-iteration log are
also summarised on issue #12.*
