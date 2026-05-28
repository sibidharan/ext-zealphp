--TEST--
Security: compare-and-restore doesn't clobber downstream handler swaps
--EXTENSIONS--
zealphp
--FILE--
<?php
// HIGH finding from v0.3.8 security review:
// zealphp_restore_all() unconditionally wrote the saved handler back to
// the function table, silently clobbering any extension that hooked the
// same slot AFTER our override. The fix: only restore if the current
// handler is still our dispatch thunk.
//
// We can't simulate a real third-party extension swap in a phpt, but we
// CAN exercise the restore path round-trip and confirm:
//   1. zealphp_override + zealphp_restore round-trips cleanly.
//   2. After restore, the function behaves like the original.
//   3. Calling zealphp_restore on an unregistered function returns false
//      cleanly (no NULL deref of the persistent handler table).

$captured = [];
$ok = zealphp_override('header', function ($h) use (&$captured) {
    $captured[] = $h;
});
var_dump($ok); // true

header("X-Test: alpha");
header("X-Test: beta");
echo count($captured), "\n";

// Restore — compare-and-restore confirms our dispatch IS still installed
$ok = zealphp_restore('header');
var_dump($ok); // true

// After restore, header() is the original — calling it doesn't append.
@header("X-Test: should-not-capture");
echo count($captured), "\n"; // still 2

// Calling restore again on the same name returns false cleanly.
$ok = zealphp_restore('header');
var_dump($ok); // false

// Calling restore on a never-overridden name returns false cleanly.
$ok = zealphp_restore('setcookie');
var_dump($ok); // false
?>
--EXPECTF--
bool(true)
2
bool(true)
2
bool(false)
bool(false)
