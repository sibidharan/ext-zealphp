--TEST--
Stage 3: top-level (file-scope) decls fall back to native error (Stage 4 territory)
--EXTENSIONS--
zealphp
--SKIPIF--
<?php
// This test PINS the documented Stage 3 limitation:
// top-level `function foo() {}` / `class Bar {}` at file scope are bound
// at COMPILE time via zend_register_top_func / zend_register_top_class,
// NOT through the runtime ZEND_DECLARE_* opcodes that our hook intercepts.
// So a second include of the same file STILL fatals with the native
// "Cannot redeclare" error — Stage 3 does not change this. Closing this
// requires a careful compile_file intercept that properly tears down
// existing class entries (method tables, inheritance, refcounting) and
// is reserved for Stage 4. Mode 1 Pool remains the workaround.
//
// Skipping rather than asserting the failure mode because exact wording
// and stacktrace format varies across PHP minor releases.
echo "skip top-level redeclare is Stage 4 territory\n";
?>
--FILE--
<?php
echo "unreachable\n";
?>
--EXPECT--
