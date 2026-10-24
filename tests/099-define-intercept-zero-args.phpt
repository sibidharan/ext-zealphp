--TEST--
099: the define() intercept never reads an argument slot past the frame when define() is called with no arguments (B5)
--EXTENSIONS--
zealphp
--FILE--
<?php
// With silent redeclare on, the intercept read ZEND_CALL_ARG(1) without
// checking ZEND_NUM_ARGS(): the slot held whatever the previous call at that
// stack depth left there. After define('ZP099_X', ...) at the same depth,
// define() with no arguments could "succeed" instead of throwing.
zealphp_define_hook(true);
zealphp_silent_redeclare(true);
function zp099_define_one() { return define('ZP099_X', 1); }
function zp099_define_none() { return define(); }
zp099_define_one();
try {
    var_dump(zp099_define_none());
} catch (ArgumentCountError $e) {
    echo get_class($e), ': ', $e->getMessage(), "\n";
}
zealphp_silent_redeclare(false);
zealphp_define_hook(false);
?>
--EXPECT--
ArgumentCountError: define() expects at least 2 arguments, 0 given
