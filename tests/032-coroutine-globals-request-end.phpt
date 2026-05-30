--TEST--
zealphp_coroutine_globals_request_end(): exists and is a safe no-op when coroutine $GLOBALS isolation is inactive
--EXTENSIONS--
zealphp
--FILE--
<?php
// The request-end drain (the coroutine-context destruction point for isolated
// object globals) must exist and be a harmless no-op when per-coroutine $GLOBALS
// isolation is NOT active — so calling it outside coroutine-legacy mode (or
// without OpenSwoole at all) never errors. Its REAL behaviour — destructing
// object globals in coroutine context so an I/O __destruct can yield — needs the
// OpenSwoole coroutine runtime + concurrency, and is covered by the framework's
// tests/Integration/CoroutineLegacyBehaviorTest.php plus the ASAN/Valgrind sweep.
var_dump(function_exists('zealphp_coroutine_globals_request_end'));

// Inactive isolation -> early return, no error, no side effect.
zealphp_coroutine_globals_request_end();

// Idempotent: callable repeatedly.
zealphp_coroutine_globals_request_end();
echo "no-op safe\n";
?>
--EXPECT--
bool(true)
no-op safe
