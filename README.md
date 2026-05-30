# ext-zealphp

[![CI](https://github.com/sibidharan/ext-zealphp/actions/workflows/ci.yml/badge.svg)](https://github.com/sibidharan/ext-zealphp/actions/workflows/ci.yml)
[![PHP 8.3 | 8.4 | 8.5](https://img.shields.io/badge/PHP-8.3%20%7C%208.4%20%7C%208.5-777BB4?logo=php&logoColor=white)](https://www.php.net/)
[![Memory safety: ASAN + Valgrind](https://img.shields.io/badge/memory--safety-ASAN%20%2B%20Valgrind-2ea44f)](.github/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](https://opensource.org/licenses/MIT)

**A per-coroutine request-state isolation runtime for long-running PHP servers.**

Purpose-built for [ZealPHP](https://php.zeal.ninja)'s `coroutine-legacy` mode: it lets
traditional request-style PHP (the PHP-FPM "fresh state per request" mental model) run
under OpenSwoole **coroutine concurrency** with every request-state primitive isolated
**per coroutine** — so unmodified `$_GET['x']`, `global $wpdb`, `define()`, `static $x`,
and `require_once` code behaves as if each request had its own process, while many
requests are actually multiplexed onto a worker's coroutine scheduler.

It does this in two layers:

1. **Function overrides** — intercepts 53 PHP built-ins (`header()`, `session_start()`,
   `setcookie()`, the exec family, …) and routes them to user-supplied callbacks, so the
   framework can point them at per-request response/session objects.
2. **Per-coroutine state isolation** — hooks OpenSwoole's `on_yield` / `on_resume` /
   `on_close` scheduler callbacks (via `dlsym`) and snapshots/restores each coroutine's
   request state across every yield.

> Most users never call these functions directly — they enable the whole stack with
> `App::mode('coroutine-legacy')` in ZealPHP. The functions below are the low-level
> primitives the framework drives.

## The isolation contract

Under `coroutine-legacy`, ZealPHP fires concurrent interleaved requests and asserts
**zero** cross-coroutine leakage of every primitive below (raw OpenSwoole leaks ~39/40):

| Primitive | Isolated per coroutine | Since |
|-----------|------------------------|-------|
| 7 superglobals — `$_GET $_POST $_REQUEST $_COOKIE $_FILES $_SERVER $_SESSION` | ✅ | 0.3.x |
| `$GLOBALS` / `global $x` — scalars & arrays | ✅ (COW delta vs parent baseline) | 0.3.7 |
| `$GLOBALS` / `global $x` — **objects** (`global $wpdb; $wpdb = new wpdb()`) | ✅ (drained in-coroutine at request-end) | **0.3.23** |
| class statics (`Foo::$bar`) | ✅ | 0.3.x |
| function-local `static $x` | ✅ (touched-set registry, ~µs/yield) | 0.3.10 |
| `define()` constants | ✅ (removed at request end) | 0.3.x |
| `ini_set()` values | ✅ (snapshot/restore) | 0.3.x |
| `putenv()` / `getenv()` | ✅ | 0.3.x |
| `header()` / `setcookie()` / `http_response_code()` response state | ✅ (via overrides) | 0.1.x |
| `require_once` / `include_once` re-execution across requests | ✅ (Stage 7) | 0.3.x |
| conditional `function`/`class` re-declaration (no `E_COMPILE_ERROR`) | ✅ (silent-redeclare) | 0.3.x |

**Not isolated (by design):** **resources** in globals (a handle's lifecycle can't be
snapshot/restored — use a per-coroutine pool), closure `static $x`, and process-global
handlers (`set_error_handler`, raw `ob_*`, `pcntl_fork`). Object request-state belongs in
`$GLOBALS` (now isolated) or ZealPHP's `$g` — not class/function statics.

### Object globals (0.3.23)

`global $wpdb; $wpdb = new wpdb()` — the canonical WordPress pattern — now gives each
coroutine its **own** `$wpdb`. Objects were previously excluded (a `__destruct`-in-
scheduler-callback UAF risk): the fix holds an isolated object's refcount in the
per-coroutine delta during the request (so the per-yield reset never drops it to zero
mid-switch), and releases its final reference at request-end via
`zealphp_coroutine_globals_request_end()` — called by the framework **in coroutine
context**, so an I/O `__destruct` (e.g. `$wpdb` closing MySQL under `HOOK_ALL`) can yield.

## Install

### Via PIE (recommended)

```bash
pie install sibidharan/ext-zealphp
```

### From source

```bash
git clone https://github.com/sibidharan/ext-zealphp.git
cd ext-zealphp
phpize
./configure --enable-zealphp
make
sudo make install
echo "extension=zealphp.so" | sudo tee $(php -i | awk -F'=> ' '/Scan this dir/ {print $2}' | head -1 | tr -d ' ')/50-zealphp.ini
```

### Verify

```bash
php -m | grep zealphp
```

The isolation features additionally require **OpenSwoole** loaded (they ride its
coroutine scheduler). The function-override layer works without it.

## API

### Function overrides

```php
// Override a PHP built-in with your own callback (allowlisted functions only)
zealphp_override('header', function (string $h, bool $replace = true, int $code = 0) {
    // route to the per-request response object
});

zealphp_restore('header');   // restore one
zealphp_restore_all();       // restore all
```

### Per-coroutine isolation (framework-driven)

These activate the scheduler-hook isolation. ZealPHP calls them from
`App::mode('coroutine-legacy')`; listed for reference.

```php
zealphp_coroutine_superglobals(true);  // isolate the 7 superglobals per coroutine
zealphp_coroutine_globals(true);       // isolate $GLOBALS / global $x (scalars, arrays, objects)
zealphp_coroutine_statics(true);       // isolate function-local static $x
zealphp_silent_redeclare(true);        // first-wins on conditional function/class re-declare
zealphp_define_hook(true);             // track + clear per-request define() constants
zealphp_include_isolation(true);       // re-execute require_once/include_once per request

// Boot baseline (onWorkerStart) + per-request lifecycle (the framework wires these):
zealphp_process_state_snapshot();              // capture the boot baseline
zealphp_request_input_set($get, $post, …);     // pin request-input superglobals to this coroutine
zealphp_coroutine_globals_request_end();       // drain object globals in-coroutine at request end
zealphp_process_state_clean($flags);           // reset per-request state
```

## Design

- **Scheduler-hook isolation**: `dlsym`s OpenSwoole's `PHPCoroutine::on_yield/on_resume/
  on_close` and chains in front of them. On yield it snapshots this coroutine's state into
  per-coroutine delta tables (keyed by the `Coroutine*` pointer) and resets the shared
  engine tables to a parent baseline; on resume it re-applies the delta. Cooperative
  scheduling means only one coroutine touches the shared tables at a time.
- **COW deltas**: `$GLOBALS` isolation stores only keys that differ from the boot baseline
  (delta + tombstone), not a full copy — memory scales with per-request writes.
- **Allowlist-only overrides**: only the 53 functions ZealPHP needs can be overridden — no
  arbitrary function replacement. **No class manipulation** (unlike uopz). Handler-swapping
  preserves `arg_info`. All originals auto-restored at `MSHUTDOWN`.
- **Memory-safe**: the snapshot/restore paths are validated under **AddressSanitizer** (PHP
  8.3/8.4/8.5, source-built) and **Valgrind memcheck** in CI, plus a 33-test `.phpt` suite
  (PARANOID cross-coroutine-leak tests, silent-redeclare, include-isolation, object-global
  isolation under real coroutine concurrency).

## Allowed functions (override allowlist)

**Response**: `header`, `header_remove`, `headers_list`, `headers_sent`, `setcookie`, `setrawcookie`, `http_response_code`, `header_register_callback`

**Output**: `flush`, `ob_flush`, `ob_end_flush`, `ob_implicit_flush`, `output_add_rewrite_var`, `output_reset_rewrite_vars`

**Process**: `set_time_limit`, `ignore_user_abort`, `connection_status`, `connection_aborted`, `register_shutdown_function`

**Error**: `error_log`, `error_reporting`, `set_error_handler`, `restore_error_handler`, `set_exception_handler`, `restore_exception_handler`

**File upload**: `is_uploaded_file`, `move_uploaded_file`

**Info**: `phpinfo`, `php_sapi_name`

**Input**: `filter_input`, `filter_input_array`

**Session** (18): `session_start`, `session_id`, `session_status`, `session_name`, `session_write_close`, `session_destroy`, `session_unset`, `session_regenerate_id`, `session_get_cookie_params`, `session_set_cookie_params`, `session_cache_limiter`, `session_cache_expire`, `session_commit`, `session_abort`, `session_encode`, `session_decode`, `session_save_path`, `session_module_name`

**Exec** (coroutine-safe shelling-out): `shell_exec`, `exec`, `system`, `passthru`

## Requirements

- **PHP 8.3, 8.4, or 8.5** (CI builds + sanitizes all three)
- **OpenSwoole** for the per-coroutine isolation features (the override layer works without it)
- A C compiler (gcc/clang) and php-dev headers for building from source

## License

MIT
