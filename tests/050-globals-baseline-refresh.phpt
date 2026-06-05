--TEST--
zealphp_globals_baseline_refresh(): exists and is a safe no-op (returns false) when coroutine $GLOBALS isolation is inactive
--EXTENSIONS--
zealphp
--FILE--
<?php
// Re-captures the parent $GLOBALS baseline from the current symbol table so that
// boot-time writes which land AFTER zealphp_coroutine_globals(true) activation are
// visible to EVERY request coroutine, not just the first one (#26: under
// concurrency only 1/N requests saw an app-bootstrap global, because the
// activation-time baseline missed the post-activation boot write and the first
// per-coroutine reset dropped it).
//
// When per-coroutine $GLOBALS isolation is NOT active this must be a harmless
// no-op returning false, so calling it outside coroutine-legacy mode (or without
// OpenSwoole at all) never errors. Its REAL behaviour — folding post-activation
// boot writes into the baseline so all concurrent requests see them — needs the
// OpenSwoole coroutine runtime + concurrency and is covered by the framework
// integration tests plus the ASAN/Valgrind sweep.
var_dump(function_exists('zealphp_globals_baseline_refresh'));

// Inactive isolation -> early return false, no error, no side effect.
var_dump(zealphp_globals_baseline_refresh());

// Idempotent: callable repeatedly.
var_dump(zealphp_globals_baseline_refresh());
echo "no-op safe\n";
?>
--EXPECT--
bool(true)
bool(false)
bool(false)
no-op safe
