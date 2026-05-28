--TEST--
Stage 3: silent-redeclare suppresses redeclare error on conditional function decls
--EXTENSIONS--
zealphp
--FILE--
<?php
// Stage 3 silent-redeclare hooks the ZEND_DECLARE_FUNCTION opcode, which PHP
// emits for CONDITIONAL function declarations (declarations inside an `if`,
// inside another function, or inside a class method). Top-level function
// declarations in PHP 8.x are bound at COMPILE-TIME via
// zend_register_top_func() — those go through a different code path that
// the opcode hook does not see. The behaviour for top-level redeclares is
// unchanged.
//
// This test covers the conditional path: the same `function foo() {}` body
// reached through two if-branches inside a loop, with silent-redeclare on.

function declare_via_conditional($n) {
    if ($n === 1) {
        function zealphp_test_cond_fn() { return "first"; }
    } else {
        function zealphp_test_cond_fn() { return "second"; }
    }
}

declare_via_conditional(1);
echo zealphp_test_cond_fn(), "\n";

$prev = zealphp_silent_redeclare(true);
var_dump($prev);

declare_via_conditional(2); // would E_COMPILE_ERROR without the hook
echo "second-decl ok\n";
echo zealphp_test_cond_fn(), "\n"; // first wins (silent-redeclare = no-op)

zealphp_silent_redeclare(false);
?>
--EXPECT--
first
bool(false)
second-decl ok
first
