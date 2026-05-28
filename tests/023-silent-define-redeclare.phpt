--TEST--
Stage 3.5: silent-redeclare auto-engages define() intercept — first-wins, no warning
--EXTENSIONS--
zealphp
--INI--
error_reporting=E_ALL
display_errors=stderr
--FILE--
<?php
// Stage 3.5 silent-define-redeclare. The intercept lives in
// zealphp_define_intercept(): when zealphp_silent_redeclare_enabled is on
// and define(NAME, VAL) targets an already-defined constant name, it
// RETURNs TRUE *without* calling the real define() — suppressing PHP's
// native "Constant FOO already defined" E_WARNING. First-declaration wins
// (matches FPM fresh-process semantics).
//
// Crucially the define() hook is AUTO-ENGAGED by zealphp_silent_redeclare(true)
// itself — the C impl installs zealphp_define_intercept as define's handler
// inside PHP_FUNCTION(zealphp_silent_redeclare) when it flips the gate on.
// So NO separate zealphp_define_hook(true) call is needed here; if that
// auto-engage regressed, the second define() would emit a warning to stderr
// and FOO would change value — both caught below.

define('FOO', 'first');
echo constant('FOO'), "\n";

// Turn on silent-redeclare. This alone must engage the define intercept.
$prev = zealphp_silent_redeclare(true);
var_dump($prev);

// Re-define FOO. Native PHP would emit E_WARNING and keep the old value;
// the intercept short-circuits to TRUE silently, also keeping the old value.
$ret = define('FOO', 'second');
var_dump($ret);

// First declaration wins.
echo constant('FOO'), "\n";

zealphp_silent_redeclare(false);
?>
--EXPECT--
first
bool(false)
bool(true)
first
