--TEST--
063: ext#50 — class_alias() re-execution is first-wins under silent_redeclare (S7 re-exec parity)
--EXTENSIONS--
zealphp
--FILE--
<?php
// A runtime class_alias() is a function call, not a ZEND_DECLARE_* opcode, so
// S3a/S3c never see it: when S7 re-executes a require_once'd file per request,
// its class_alias('New','Old') line runs again and vanilla PHP fatals with
// "Cannot redeclare class" (DokuWiki inc/legacy.php Doku_Event_Handler).
// Under silent_redeclare the intercept returns true when the ALIAS name
// already exists — first declaration wins, matching define()'s semantics.

zealphp_silent_redeclare(true);

class Alias063Target {}

var_dump(class_alias('Alias063Target', 'Alias063Old'));        // first: real alias
var_dump(class_alias('Alias063Target', 'Alias063Old'));        // re-exec: silent true
var_dump(class_alias('Alias063Target', '\\Alias063Old'));      // leading-backslash form
var_dump(new Alias063Old() instanceof Alias063Target);          // alias is live

// The intercept must NOT swallow a FIRST-TIME alias.
var_dump(class_alias('Alias063Target', 'Alias063Fresh'));
var_dump(new Alias063Fresh() instanceof Alias063Target);

// And aliasing onto an existing CLASS name (not just an existing alias) is
// also first-wins — the class keeps its identity.
class Alias063Existing {}
var_dump(class_alias('Alias063Target', 'Alias063Existing'));
var_dump(new Alias063Existing() instanceof Alias063Target);     // false: original won

zealphp_silent_redeclare(false);
echo "DONE\n";
?>
--EXPECT--
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
bool(false)
DONE
