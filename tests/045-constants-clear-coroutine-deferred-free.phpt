--TEST--
045: zealphp_constants_clear() defers the constant free to coroutine close in coroutine mode (#9)
--EXTENSIONS--
zealphp
--SKIPIF--
<?php
if (!extension_loaded('openswoole')) {
    die('skip requires the OpenSwoole coroutine runtime');
}
?>
--FILE--
<?php
// #9: in COROUTINE mode (os_get_cid >= 0) constants_clear() must ORPHAN each
// request constant — remove it from EG(zend_constants) WITHOUT freeing — and
// defer the actual free to coroutine close. An immediate free would dangle a
// cached FETCH_CONSTANT run_time_cache slot (process-shared under opcache) that
// still points at the struct → use-after-free.
//
// This exercises the orphan-then-free-at-close path. Under the ext's ASAN /
// valgrind CI it also pins the mechanics: a missing on_close drain LEAKS; a
// non-orphaning zend_hash_del (dtor left active) DOUBLE-FREES at close.
Co::run(function () {
    zealphp_define_hook(true);
    define('ZP045_REQ', 'value-one');
    var_dump(defined('ZP045_REQ'));      // true
    var_dump(constant('ZP045_REQ'));     // the live value before clear
    zealphp_constants_clear();
    // Orphaned out of EG immediately (only the FREE is deferred), so defined()
    // is already false — same observable contract as the sync path.
    var_dump(defined('ZP045_REQ'));      // false
    zealphp_define_hook(false);
});
// The coroutine has closed here → zealphp_on_close drained + freed the orphan.
echo "done\n";
?>
--EXPECT--
bool(true)
string(9) "value-one"
bool(false)
done
