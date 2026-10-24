--TEST--
092: an object a go() child stores in $GLOBALS while its request is suspended is never destructed inside the scheduler callback (B1: no "API must be called in the coroutine" fatal)
--EXTENSIONS--
zealphp
--SKIPIF--
<?php if (!extension_loaded('openswoole')) die('skip needs the OpenSwoole coroutine runtime'); ?>
--FILE--
<?php
// The child is not a request owner, so its yields never parked what it
// wrote. When the request resumed, the reset to the baseline dropped the
// child's object - its last reference - inside on_resume, and a coroutine
// call in __destruct raised E_ERROR from the scheduler callback.
use OpenSwoole\Coroutine as Co;
class Zp092Obj {
    public function __destruct() {
        echo 'dtor in a coroutine: ', Co::getCid() > 0 ? 'yes' : 'no', "\n";
        Co::usleep(1000);   // a coroutine call: fatal outside a coroutine
        echo "dtor yielded\n";
    }
}
zealphp_coroutine_globals(true);
Co::run(function () {
    go(function () {
        zealphp_superglobals_owner();
        go(function () {
            Co::usleep(10000);
            $GLOBALS['zp092'] = new Zp092Obj();
            Co::usleep(50000);
        });
        Co::usleep(20000);
        echo "request resumed\n";
        Co::usleep(60000);
        zealphp_coroutine_globals_request_end();
        echo "request end\n";
    });
});
zealphp_coroutine_globals(false);
echo "survived\n";
?>
--EXPECT--
request resumed
dtor in a coroutine: yes
dtor yielded
request end
survived
