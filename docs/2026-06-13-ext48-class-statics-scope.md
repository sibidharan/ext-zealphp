# ext#48 — per-coroutine class-static isolation: scope, leak window, and recommended fix

**Status:** analytical scoping (no build, no rig). Code read against `zealphp.c`
@ `PHP_ZEALPHP_VERSION "0.3.51"`. Probe: `tests/065-class-statics-concurrent-window-probe.phpt`.

Issue #48 ("coroutine-legacy: per-coroutine class-static isolation — the
OOP-static-cache frontier") reports that class `static` properties leak across
concurrent coroutines: CakePHP (`TableSchema::$_validConstraintTypes`,
`Router::$_collection`), Kirby (`App::$instance`/`$site`), Piwigo. The issue's
one-line root cause — *"Class statics are reset, not isolated"* — is **half
right as of 0.3.51**: there now IS a per-coroutine class-static snapshot/restore
stage (call it **S5b**), but it has a **deliberate object/resource hole** that is
exactly the singleton/DI-container shape those three apps trip on. This note
pins what is isolated, what is only reset, the exact leak window, and the
recommended fix with its implementation locus.

## 1. What exists today

Three distinct mechanisms touch `ce->static_members_table`:

| Mechanism | Function (zealphp.c) | When | Scope |
|---|---|---|---|
| **S5b** per-coroutine class-static *snapshot/restore* | `zealphp_statics_snapshot_save` (~L1107) / `_restore` (~L1165) / `_delete` (~L1195) | every coroutine **yield/resume/close** | isolates **scalar/array** statics per coroutine; **objects/resources EXCLUDED** |
| **S11c** per-request class-static *reset* | `zealphp_reset_request_class_statics` (~L5499) → `zealphp_reset_class_statics_inplace` (~L5470) | once at **request end** (framework `finally`) | resets ALL non-snapshot user-class statics (incl. objects) to the boot template, **in place** (stable table address) |
| boot exemption | `zealphp_snapshot_classes` / `zealphp_state_snapshotted` | snapshot at boot | classes declared before the boot snapshot (`App::$routes`, middleware, Store/Counter) are skipped by S11c |

### S5b wiring (confirmed, not gated off)

`zealphp_on_yield` (~L2204) calls `zealphp_statics_snapshot_save(arg)` at **~L2266**,
and `zealphp_on_resume` (~L2287) calls `zealphp_statics_snapshot_restore(arg)` at
**~L2349**. Both are gated only by the request-state owner gate
(`zp_req_owner_ok` on save, `zp_restore_ok` on resume) — **not** by
`zealphp_fn_statics_active` (that flag only gates the *function*-static stage at
L2267/L2351). So under `coroutine-legacy` (which calls
`zealphp_coroutine_globals(true)`), the class-static snapshot lane is live.

### The object hole (the heart of #48)

`zealphp_statics_snapshot_save` filters each static slot through
`zealphp_globals_isolatable(&statics[i])` (~L1147). That predicate (def ~L1539)
returns **false for `IS_OBJECT` and `IS_RESOURCE`** (deref'ing `IS_REFERENCE`
first). The in-code rationale (~L1138): deep-copying an object on save would
`ZVAL_DUP`/incref it, and dropping its last ref on restore would fire
`__destruct` **inside the C `on_resume` callback** (where `os_get_cid() == -1`),
which can re-enter the executor / yield mid-resume and corrupt the scheduler —
the same hazard the object-**globals** stage solved with the in-coroutine
request-end drain (0.3.23 / 0.3.47). For class statics that drain contract was
**never built**, so objects were simply left out.

Net: **scalar/array class statics are isolated per coroutine; object/singleton
class statics are process-shared and only get reset at request end (S11c).**

## 2. The exact leak window

Two overlapping requests A and B share one process-global `static_members_table`
per class. Timeline (true concurrency, single worker, cooperative yields):

```
A: Class::$singleton = new App();   // OBJECT static  -> NOT snapshotted (S5b skips it)
A: Class::$cache['k'] = 'A';        // ARRAY static   -> will be snapshotted on A's next yield
A: <I/O yield>                      // on_yield(A): snapshot_save(A) copies $cache; $singleton untouched
   ────────────────────────────────────────────────────────────────────────
B resumes:                          // on_resume(B): snapshot_restore(B) rewrites $cache to B's value
B: read Class::$singleton           //   -> sees A's object  ★ LEAK (object path, no snapshot)
B: read Class::$cache['k']          //   -> sees 'B' IF B had a prior snapshot of $cache;
                                    //      else sees the live 'A' (B never snapshotted that slot)
```

Two sub-windows, by static value type:

- **Object/resource statics (primary, the #48 symptoms).** Never enter any
  snapshot. A's write is visible to B for the entire span between A's write and
  A's request-end `zealphp_reset_request_class_statics()`. B reads A's singleton
  (`App::$instance` → wrong site/null after A's reset; `Router::$_collection`
  "must not be accessed before initialization" when A reset it to UNDEF while B
  renders; `$_validConstraintTypes` int-vs-array type confusion when A re-typed
  it). **A's request-end reset is itself a second hazard**: it zeroes the shared
  object while B is mid-flight (the "count(): int|float given at request 2+",
  Piwigo).

- **Scalar/array statics (secondary, narrower).** S5b isolates them *only for a
  coroutine that already has its own snapshot of that class slot* — i.e. it
  wrote the static and yielded at least once, so `snapshot_restore` re-establishes
  its value on resume. The residual leak is the **first-touch / never-wrote**
  case: a coroutine that *reads* a scalar/array static it never wrote (and thus
  has no snapshot entry for) reads whatever a concurrent peer last left live.
  `snapshot_restore` only rewrites slots present in the reader's own snapshot
  (`ZEND_HASH_FOREACH_NUM_KEY_VAL` over the reader's class snapshot, ~L1186), so
  an absent entry is a no-op and the live (peer-written) value stands.

The probe `tests/065-...` pins both legs deterministically (relay-channel
ordering): `B_sees_obj=A` (object leak) and `B_sees_cache=B` (array isolated,
because B wrote+yielded first).

### Why "reset, not isolated" was the field diagnosis

From an app's perspective the object hole is indistinguishable from "no
isolation at all": the only thing that ever cleans an object static is the
per-request reset (S11c), which is a request-*boundary* event, not a
per-*coroutine* one. So under concurrency the object static behaves exactly as
issue #48 describes — shared, then reset out from under a peer.

## 3. Fix options

The bar set by the rest of the stack: **isolate per coroutine via the
snapshot/restore model already proven for superglobals, $GLOBALS, constants, ini,
cwd/locale/umask/tz/mb/libxml, and fn-statics (S5a).** Three candidates:

### Option A — extend S5b to objects, with an in-coroutine drain (RECOMMENDED)

Mirror the object-**globals** solution (0.3.23/0.3.47) for class statics:

1. **Snapshot objects too.** In `zealphp_statics_snapshot_save`, replace the
   `zealphp_globals_isolatable()` filter with the **object-including** variant
   the $GLOBALS path already uses — `zealphp_globals_isolatable_obj()` (def
   ~L1578, the "ALSO isolates OBJECTS, resources still excluded" predicate;
   already consumed by the object-globals snapshot at ~L1702/L1793). On save, the snapshot holds the object ref (refcount held by
   the per-coroutine snapshot for the request's life — so the per-yield restore
   never drops it to zero mid-switch → no `__destruct`-in-`on_resume` UAF, the
   exact safety argument from the object-globals review).
2. **Restore write-through** unchanged (`zval_ptr_dtor(old)` + `ZVAL_DUP`), but
   because the displaced value's ref is held by *some* coroutine's snapshot, the
   dtor at restore never fires a user `__destruct` in the C callback.
3. **In-coroutine final drain.** The object's last ref is released at request
   end **in coroutine context**, not in `on_close`. There is already such a
   hook for globals (`zealphp_coroutine_globals_request_end`); add the class-
   static analog (drain this coroutine's `zealphp_coro_static_snapshots[cid]`
   object entries) and call it from the same `CoSessionManager`/`SessionManager`
   `finally` that runs the request-end resets. `on_close` stays the
   fatal-path backstop (it already calls `zealphp_statics_snapshot_delete`,
   ~L2430 — that delete must defer object frees to coroutine context the same
   way the constants path defers, see ~L470).
4. **First-touch isolation.** To close the scalar/array first-touch leg AND give
   objects a clean baseline, the restore must also cover slots the reader never
   wrote. Cheapest correct approach: on the **first** time a request coroutine
   is seen (its `on_resume` with no snapshot yet), seed its snapshot from the
   **boot template** (`ce->default_static_members_table`) for every instantiated
   class, so a never-written read returns the template default (PHP-FPM parity),
   not a peer's value. This is the class-static equivalent of the $GLOBALS
   parent-baseline park.

**Implementation locus (zealphp.c):**
- `zealphp_statics_snapshot_save` ~L1107–L1163 — swap the filter to the
  object-including predicate; hold object refs in the snapshot.
- `zealphp_statics_snapshot_restore` ~L1165–L1194 — unchanged write-through;
  add the first-touch template seed (new branch keyed on "no prior snapshot for
  cid").
- `zealphp_statics_snapshot_delete` ~L1195 — defer object frees to coroutine
  context (model on `zealphp_constants_snapshot_delete` ~L520 / the object-global
  `cid→ptr` bridge ~L2278).
- New `PHP_FUNCTION(zealphp_coroutine_class_statics_request_end)` (or fold into
  the existing globals request-end) — final in-coroutine drain; wire in the two
  session managers' `finally`.
- Keep S11c (`zealphp_reset_request_class_statics`) as the per-request reset for
  the **boot-exempt-but-mutated** and **non-isolated-leftover** cases; once S5b
  covers objects, S11c's role narrows to "reset object statics of classes that
  were never snapshotted because the coroutine never touched them," which the
  first-touch template seed largely subsumes.

**Cost.** Per-yield: today S5b already walks `EG(class_table)` and deep-copies
every isolatable static slot of every class with an instantiated
`static_members_table`. Adding objects is **+1 incref per object static per
yield** (ref-hold, not deep-copy) plus the per-request drain. The dominant cost
is unchanged: the full-table walk. If that walk shows up under load, add a
**touched-set registry** exactly like S5a's `zealphp_fn_static_registry`
(`ZEND_BIND_STATIC`-style hook, here on the static-property *write* path / first
`zend_class_init_statics`) so per-yield cost scales with classes-that-have-
mutated-statics, not all classes. That registry is the second-phase optimization;
correctness lands with the object inclusion + drain alone.

### Option B — touched-set registry first (S5a-style), then objects

Build the registry up front (hook the static-property store opcode /
`zend_std_write_static_property` path), then snapshot only registered classes.
Lower per-yield cost from day one, but more engine-surface to hook and the same
object-drain contract still has to be built. **Defer to Option A's phase 2** —
it doesn't change the correctness fix, only its cost profile.

### Option C — leave isolation off, harden S11c only

Status quo + document object statics as a permanent boundary (apps stay
sequential-only: mixed/legacy-cgi). Rejected: it forfeits exactly the tier #48
targets (CakePHP/Kirby/Piwigo), and the rest of the stack has already paid the
snapshot/restore cost for every other request-state primitive — class statics
are the lone unfinished member.

## 4. Recommendation

**Option A.** Extend the existing S5b snapshot/restore to objects using the
object-including isolatable predicate + the in-coroutine request-end drain
contract already proven for object-globals (0.3.23/0.3.47), plus a first-touch
template seed on `on_resume`. This reuses machinery the stack already trusts,
matches the "isolate per coroutine via snapshot/restore" discipline of every
sibling stage, and keeps the `__destruct`-in-`on_resume` UAF closed by the same
ref-held-by-snapshot + drain-in-coroutine argument that made object-globals safe.
Add the S5a-style touched-set registry as a follow-up optimization if the
full-table walk shows under load. Until shipped, CakePHP/Kirby/Piwigo remain
mixed/legacy-cgi-only — the per-request reset (S11c) suffices there because
those modes have no per-request coroutine overlap.

**Verdict the probe pins:** current behavior leaks the object static
(`B_sees_obj=A`) while isolating the array static for a reader with its own
snapshot (`B_sees_cache=B`). The future fix flips `B_sees_obj` to `B`'s own /
template value (and the probe's EXPECT) — bidirectional regression value.
