--TEST--
Stage 3: silent-redeclare gate OFF — handlers are no-ops, native fatal still fires; toggle returns prev state
--EXTENSIONS--
zealphp
--FILE--
<?php
// Gate-OFF correctness. Every Stage 3 opcode handler
// (zealphp_declare_function_handler / _class_handler / _class_delayed_handler)
// begins with `if (!zealphp_silent_redeclare_enabled) return DISPATCH;`, so
// with the gate off (the default) they are pure pass-throughs and a genuine
// conditional re-declare must reproduce native PHP's fatal.
//
// First we pin the toggle contract: zealphp_silent_redeclare(bool) returns
// the PREVIOUS state. Round-trip false -> true -> false:
var_dump(zealphp_silent_redeclare(true));   // was off  -> prev false
var_dump(zealphp_silent_redeclare(false));  // was on   -> prev true
var_dump(zealphp_silent_redeclare(false));  // was off  -> prev false (gate now OFF)

// Gate is OFF. A conditional class-with-parent redeclare reaches
// ZEND_DECLARE_CLASS_DELAYED at runtime; with the handler dispatching
// normally, the engine fatals exactly as stock PHP does.
class ZGateParent { public function tag() { return "p"; } }

function zgate_decl($n) {
    if ($n === 1) {
        class ZGateChild extends ZGateParent { public function who() { return "first"; } }
    } else {
        class ZGateChild extends ZGateParent { public function who() { return "second"; } }
    }
}

zgate_decl(1);
echo (new ZGateChild)->who(), "\n";

zgate_decl(2); // gate off => native fatal, script halts here
echo "should NOT reach here\n";
?>
--EXPECTF--
bool(false)
bool(true)
bool(false)
first

Fatal error: Cannot declare class ZGateChild, because the name is already in use in %s on line %d
