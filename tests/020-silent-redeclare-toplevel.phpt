--TEST--
Stage 3: top-level (file-scope) decls fall back to native error (Stage 4 territory)
--EXTENSIONS--
zealphp
--SKIPIF--
<?php
// PINS the Stage 3 limitation: top-level `function foo() {}` / `class Bar {}`
// at file scope are bound at COMPILE time via zend_register_top_func /
// zend_register_top_class, NOT through the runtime ZEND_DECLARE_* opcodes
// that Stage 3 intercepts. A naive `zend_compile_file` wrapper (Stage 4
// first attempt) deadlocks on autoloader-driven compile recursion: every
// nested require walks + mutates the global function/class tables, and
// the cumulative O(N*M) cost — plus interactions with opcache's late-bind
// path — hangs the worker. Stage 4 needs a per-file diff approach (only
// touch what the current compile is about to define) rather than a
// blanket detach/reattach of the whole user-symbol space. Deferred.
echo "skip top-level redeclare is Stage 4 territory (compile_file hook deferred)\n";
?>
--FILE--
<?php
echo "unreachable\n";
?>
--EXPECT--
