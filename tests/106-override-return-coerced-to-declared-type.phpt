--TEST--
106: an override's return value is coerced to the overridden function's declared return type, with one warning per function (B2)
--EXTENSIONS--
zealphp
--FILE--
<?php
// headers_sent(): bool and session_id(): string|false keep their declared
// types while an override runs; opcache's type inference and the JIT trust
// them, so a mismatched value could corrupt memory under JIT.
zealphp_override('headers_sent', fn () => 'yes');
var_dump(headers_sent());
var_dump(headers_sent());                 // no second warning
zealphp_override('session_id', fn () => 42);
var_dump(session_id());
zealphp_restore('session_id');
zealphp_override('session_id', fn () => null);
var_dump(session_id());
zealphp_restore('session_id');
zealphp_override('session_id', fn () => 'abc');
var_dump(session_id());                   // matching type: untouched, no warning
zealphp_restore_all();
?>
--EXPECTF--
Warning: headers_sent(): ext-zealphp: the override for headers_sent() returned string; coerced to bool, the function's declared return type (warned once per function) in %s on line %d
bool(true)
bool(true)

Warning: session_id(): ext-zealphp: the override for session_id() returned int; coerced to string, the function's declared return type (warned once per function) in %s on line %d
string(2) "42"
bool(false)
string(3) "abc"
