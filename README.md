# ext-zealphp

[![CI](https://github.com/sibidharan/ext-zealphp/actions/workflows/ci.yml/badge.svg)](https://github.com/sibidharan/ext-zealphp/actions/workflows/ci.yml)
[![PHP 8.3 | 8.4 | 8.5](https://img.shields.io/badge/PHP-8.3%20%7C%208.4%20%7C%208.5-777BB4?logo=php&logoColor=white)](https://www.php.net/)
[![Memory safety: ASAN + Valgrind](https://img.shields.io/badge/memory--safety-ASAN%20%2B%20Valgrind-2ea44f)](.github/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](https://opensource.org/licenses/MIT)

Per-request function overrides for long-running PHP servers.

A 250-line C extension that intercepts 53 PHP built-in functions (`header()`, `session_start()`, `setcookie()`, etc.) and routes them to user-supplied callbacks. Purpose-built for [ZealPHP](https://php.zeal.ninja) — each coroutine/request gets its own response/session state while legacy PHP code calls the same built-in functions unchanged.

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

## API

```php
// Override a PHP built-in with your own callback
zealphp_override('header', function(string $h, bool $replace = true, int $code = 0) {
    // route to per-request response object
});

// Restore one function
zealphp_restore('header');

// Restore all overridden functions
zealphp_restore_all();
```

## Design

- **Allowlist-only**: only 53 functions ZealPHP needs can be overridden — no arbitrary function replacement
- **No class manipulation**: unlike uopz, cannot modify classes, properties, or constants
- **Handler-swapping**: modifies the existing `zend_function` handler pointer in-place, preserving arg_info
- **MSHUTDOWN cleanup**: all originals auto-restored when the extension unloads

## Allowed functions

**Response**: `header`, `header_remove`, `headers_list`, `headers_sent`, `setcookie`, `setrawcookie`, `http_response_code`, `header_register_callback`

**Output**: `flush`, `ob_flush`, `ob_end_flush`, `ob_implicit_flush`, `output_add_rewrite_var`, `output_reset_rewrite_vars`

**Process**: `set_time_limit`, `ignore_user_abort`, `connection_status`, `connection_aborted`, `register_shutdown_function`

**Error**: `error_log`, `error_reporting`, `set_error_handler`, `restore_error_handler`, `set_exception_handler`, `restore_exception_handler`

**File upload**: `is_uploaded_file`, `move_uploaded_file`

**Info**: `phpinfo`, `php_sapi_name`

**Input**: `filter_input`, `filter_input_array`

**Session** (18): `session_start`, `session_id`, `session_status`, `session_name`, `session_write_close`, `session_destroy`, `session_unset`, `session_regenerate_id`, `session_get_cookie_params`, `session_set_cookie_params`, `session_cache_limiter`, `session_cache_expire`, `session_commit`, `session_abort`, `session_encode`, `session_decode`, `session_save_path`, `session_module_name`

**Exec**: `shell_exec`, `exec`, `system`, `passthru`

## Requirements

- PHP 8.3+
- A C compiler (gcc/clang) and php-dev headers for building from source

## License

MIT
