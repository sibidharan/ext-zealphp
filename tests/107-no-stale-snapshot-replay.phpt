--TEST--
107: a yield with nothing to snapshot drops the coroutine's earlier class-static snapshot and tombstone set, so a resume never replays stale values
--EXTENSIONS--
zealphp
--SKIPIF--
<?php if (!extension_loaded('openswoole')) die('skip needs the OpenSwoole coroutine runtime'); ?>
--FILE--
<?php
// Class statics: a yield where every slot matched its template left the
// previous yield's snapshot in place, and the next resume wrote it back.
// $GLOBALS: a yield with no unset parent keys left the previous tombstone
// set, and the next resume deleted a key the request had set again.
use OpenSwoole\Coroutine as Co;
$GLOBALS['zp107_k'] = 'baseline';
zealphp_coroutine_globals(true);
Co::run(function () {
    go(function () {
        zealphp_superglobals_owner();
        eval('class Zp107Post { public static $v = null; }');
        Zp107Post::$v = 'a';
        unset($GLOBALS['zp107_k']);
        Co::usleep(1000);
        Zp107Post::$v = null;              // back to the template value
        $GLOBALS['zp107_k'] = 'set again';
        Co::usleep(1000);
        Co::usleep(1000);
        echo 'static: ', var_export(Zp107Post::$v, true), "\n";
        echo 'global: ', var_export($GLOBALS['zp107_k'] ?? '(deleted)', true), "\n";
        zealphp_coroutine_globals_request_end();
    });
});
zealphp_coroutine_globals(false);
?>
--EXPECT--
static: NULL
global: 'set again'
