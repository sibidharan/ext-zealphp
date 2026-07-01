# coroutine-legacy per-request perf: begin-refresh must exempt snapshot symbols (0.3.60)

**Symptom:** `coroutine-legacy` served small requests ~4.5x slower than plain
`coroutine` mode — `/ping` (no I/O, no yield) at **3,678 rps vs 16,908 rps**.

## Attribution

Differential toggle sweep on `/ping` (isolates per-request setup cost, no yield):

| config | rps |
|---|---|
| `coroutine` (baseline) | 16,908 |
| `coroutine-legacy` (full) | **3,678** |
| `mixed` (superglobals, no coro-isolation) | 17,688 |
| `coroutine-legacy`, fn-statics-reset OFF | 16,782 |
| `coroutine-legacy`, class-statics-reset OFF | 3,800 |

superglobals(true) costs nothing (`mixed` is full speed). The entire slowdown is
the **function-static reset**, specifically the request-BEGIN refresh
(`zealphp_reset_request_statics_begin`).

## Root cause — not the reset's CPU, the recomputation it forces

The begin-refresh itself is cheap (measured **800 ns/call**, 9 registry entries).
The cost is **downstream**: the registry's static-using functions in this app are
the framework's OWN memoisation caches —

- `ZealPHP\access_logging_enabled` / `debug_logging_enabled` /
  `async_logging_enabled` / `bench_mode_enabled` (`static $enabled = null;`)
- `ZealPHP\resolve_log_dir` (`static $resolved; static $checked;` — **filesystem
  probes** for the log dir)
- `ZealPHP\log_file_for`, `ZealPHP\site_url`, `ZealPHP\App::buildServerVars`,
  `ZealPHP\Session\zeal_session_start`

Begin re-initialised each to its `null`/`false`/`[]` template **every request**,
so every one of those functions recomputed its cached result (env reads, path
resolution, filesystem stats, `$_SERVER` assembly) on **every** request.

The request-END reset already exempts snapshot (boot/framework) symbols, so it
left them alone — but the request-BEGIN refresh applied **no exemption** and
clobbered them.

## Fix

Apply the same snapshot exemption in `zealphp_reset_request_statics_begin` as the
end reset (`zealphp_opa_snapshot_exempt`, matching the by-name check against
`zealphp_snapshot_functions`/`_classes`). Framework memoisation statics are
per-worker state that MUST persist; only user-app request statics — the #28
WordPress-class `static $first_init` guards, which are NOT snapshot symbols —
need the begin-refresh, so the concurrency contract is unchanged.

The end reset was also switched from a full `EG(function_table)`+`class_table`
sweep to the existing `zealphp_fn_static_registry` walk (same set the per-yield
save/restore uses), so it scales with static-using functions, not total symbols.

## Result

| endpoint | coroutine-legacy before | after | plain coroutine |
|---|---|---|---|
| `/ping` | 3,678 | **15,657** | 17,184 |
| `/work` | 3,861 | **15,763** | 15,776 |
| `/io` | 3,834 | **11,617** | 14,401 |

coroutine-legacy now runs within 0–20% of plain coroutine. phpt `037`/`038` (the
reset + snapshot-exemption contract) stay green; #59 constant isolation unaffected
(0 leak, 0 fatal). Env kill-switch `ZEALPHP_FN_STATICS_RESET_DISABLE` unchanged.
