--TEST--
Stage 7: zealphp_include_isolation() returns previous state; disabled path is standard cached require_once
--EXTENSIONS--
zealphp
--FILE--
<?php
// Toggle/gate semantics for Stage 7. zealphp_include_isolation(bool $on)
// returns the PREVIOUS enabled state and only mutates when $on is non-null.
// Initial state is false.

// false initially -> enabling returns the previous value (false).
var_dump(zealphp_include_isolation(true));   // prev = false
// already enabled -> re-enabling returns true.
var_dump(zealphp_include_isolation(true));   // prev = true
// disabling returns the previous value (true) and leaves it off.
var_dump(zealphp_include_isolation(false));  // prev = true
// now disabled -> querying with null does not mutate, returns current (false).
var_dump(zealphp_include_isolation(null));   // prev = false, no mutation
// confirm still off (re-disabling reports the unchanged false).
var_dump(zealphp_include_isolation(false));  // prev = false

// Disabled path: even with a snapshot taken, isolation OFF means the Stage 7
// hook is a no-op, so a fresh (non-snapshot) file is the standard PHP
// require_once: executes ONCE, second require_once is a cached no-op.
zealphp_process_state_snapshot();            // snapshot before file exists
// isolation is OFF here (last toggle left it false).

$GLOBALS['n'] = 0;

$tmp = tempnam(sys_get_temp_dir(), 'zealphp_toggle_');
$file = $tmp . '.php';
file_put_contents($file, '<?php $GLOBALS["n"]++; echo "run\n";');

require_once $file;   // executes -> n == 1
require_once $file;   // standard cached no-op (isolation off)
echo "count=" . $GLOBALS['n'] . "\n";

@unlink($file);
@unlink($tmp);
?>
--EXPECT--
bool(false)
bool(true)
bool(true)
bool(false)
bool(false)
run
count=1
