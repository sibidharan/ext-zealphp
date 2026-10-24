--TEST--
093: an object a go() child stores in a post-boot class static is never destructed inside on_resume when its request's statics are restored (B1, class-static shape)
--EXTENSIONS--
zealphp
--SKIPIF--
<?php if (!extension_loaded('openswoole')) die('skip needs the OpenSwoole coroutine runtime'); ?>
--FILE--
<?php
// zealphp_statics_snapshot_restore() released the live slot with
// zval_ptr_dtor() before writing the request's own value back: the child's
// object lost its last reference inside the scheduler callback.
use OpenSwoole\Coroutine as Co;
class Zp093Obj {
    public function __destruct() {
        $in = Co::getCid() > 0 ? 'coroutine' : 'callback';
        Co::usleep(500);
        echo "dtor in: {$in}\n";
    }
}
zealphp_coroutine_globals(true);
Co::run(function () {
    go(function () {
        zealphp_superglobals_owner();
        eval('class Zp093Post { public static $x = null; }');   // post-boot class
        go(function () {
            Co::usleep(10000);
            Zp093Post::$x = new Zp093Obj();
            Co::usleep(50000);
            echo "child done\n";
        });
        Zp093Post::$x = 'request';
        Co::usleep(20000);
        echo 'request resumed: ', is_object(Zp093Post::$x) ? 'object' : var_export(Zp093Post::$x, true), "\n";
        Zp093Post::$x = null;
        Co::usleep(60000);
        zealphp_coroutine_globals_request_end();
        echo "request end\n";
    });
});
zealphp_coroutine_globals(false);
echo "survived\n";
?>
--EXPECTF--
request resumed: %s
%Adtor in: coroutine
%Arequest end
survived
