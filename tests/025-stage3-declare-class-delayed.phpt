--TEST--
Stage 3: silent-redeclare skips ZEND_DECLARE_CLASS_DELAYED (class WITH parent, deferred bind)
--EXTENSIONS--
zealphp
--FILE--
<?php
// A class declared WITH a parent compiles to ZEND_DECLARE_CLASS_DELAYED:
// the bind is deferred until the parent is available, and for a CONDITIONAL
// declaration (inside a function / if-branch) that opcode executes at
// runtime. zealphp_declare_class_delayed_handler reads the destination key
// from op2 and, when silent_redeclare is on and the class already exists in
// CG(class_table), advances past the do_bind that would fatal with
// "Cannot declare class ..., because the name is already in use".
//
// Native PHP (without the hook) E_COMPILE_ERRORs on the second branch — see
// 026 for the gate-off proof. Here we pin the first-wins skip.

class ZDCDParent { public function tag() { return "parent"; } }

function zdcd_declare($n) {
    if ($n === 1) {
        class ZDCDChild extends ZDCDParent { public function who() { return "first"; } }
    } else {
        class ZDCDChild extends ZDCDParent { public function who() { return "second"; } }
    }
}

zdcd_declare(1);
echo (new ZDCDChild)->who(), "\n";

zealphp_silent_redeclare(true);

zdcd_declare(2); // delayed-bind would fatal without the hook
echo "second-decl ok\n";

// First declaration wins: method body and parent both reflect branch 1.
echo (new ZDCDChild)->who(), "\n";
echo get_parent_class(new ZDCDChild), "\n";

zealphp_silent_redeclare(false);
?>
--EXPECT--
first
second-decl ok
first
ZDCDParent
