--TEST--
044: zealphp_require_global() frees its op_array on a zend_bailout inside the required file (#17)
--EXTENSIONS--
zealphp
--INI--
; PHP 8.4+ deprecates passing E_USER_ERROR to trigger_error() and emits an
; E_DEPRECATED before the fatal — suppress it so the output is identical across
; 8.3 / 8.4 / 8.5 (we only care about the bailout, not the deprecation notice).
error_reporting=E_ALL & ~E_DEPRECATED
--FILE--
<?php
// #17: a zend_bailout (E_ERROR / OOM / E_USER_ERROR) inside the required file used
// to longjmp past zealphp_require_global()'s cleanup, leaking the op_array + call
// frame. The fix wraps the execute in zend_try / zend_catch, frees the op_array on
// bailout, and re-raises (the engine reclaims the VM-stack frame). Under the ext's
// valgrind CI this run asserts no leak; functionally the fatal still propagates
// (the worker would recycle).
$dir  = sys_get_temp_dir();
$pid  = getmypid();
$boom = $dir . '/zp044_boom_' . $pid . '.php';
file_put_contents($boom, '<?php trigger_error("zp044 boom", E_USER_ERROR);');
register_shutdown_function(static function () use ($boom) { @unlink($boom); });

zealphp_require_global($boom);
echo "unreachable\n";
?>
--EXPECTF--
Fatal error: zp044 boom in %s on line %d
