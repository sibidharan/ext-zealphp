--TEST--
Stage 3.5: zealphp_define_hook() explicit toggle + constants_clear request-constant removal
--EXTENSIONS--
zealphp
--INI--
error_reporting=E_ALL
display_errors=stderr
--FILE--
<?php
// zealphp_define_hook(bool $enable): bool installs / removes the
// zealphp_define_intercept handler on define() and RETURNS the resulting
// hooked state (zealphp_define_hooked) AFTER applying the toggle — i.e. the
// CURRENT state, not the previous one. Verify the round-trip.
var_dump(zealphp_define_hook(true));   // now hooked -> true
var_dump(zealphp_define_hook(false));  // now unhooked -> false

// With the define hook ON but silent_redeclare OFF (default), a duplicate
// define() must behave EXACTLY like native PHP: the constant keeps its
// first value and define() returns false (+ an E_WARNING routed to stderr,
// not stdout). The intercept only short-circuits when silent_redeclare is on.
zealphp_define_hook(true);
define('ZEALPHP_HOOK_C', 'orig');
$dup = @define('ZEALPHP_HOOK_C', 'changed');
var_dump($dup);                         // native dup -> false
echo constant('ZEALPHP_HOOK_C'), "\n";  // unchanged -> orig

// zealphp_constants_clear() removes constants that were define()'d through
// the intercept this request, so defined() is false afterward.
var_dump(defined('ZEALPHP_HOOK_C'));    // true before clear
zealphp_constants_clear();
var_dump(defined('ZEALPHP_HOOK_C'));    // false after clear

zealphp_define_hook(false);
?>
--EXPECT--
bool(true)
bool(false)
bool(false)
orig
bool(true)
bool(false)
