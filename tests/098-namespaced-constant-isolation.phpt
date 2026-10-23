--TEST--
098: define() isolation covers namespaced constants whose namespace has capitals: parked at a yield, removed at request end (B4)
--EXTENSIONS--
zealphp
--SKIPIF--
<?php if (!extension_loaded('openswoole')) die('skip needs the OpenSwoole coroutine runtime'); ?>
--FILE--
<?php
// The request lane was keyed by the raw name 'App\DB_PASS' while
// EG(zend_constants) stores 'app\DB_PASS', so the constant was never found to
// park or clear: it leaked to peers and to every later request.
use OpenSwoole\Coroutine as Co;
zealphp_coroutine_superglobals(true);
zealphp_define_hook(true);
Co::run(function () {
    go(function () {
        zealphp_superglobals_owner();
        define('App\DB_PASS', 'secret-a');
        define('Plain_Const', 'plain-a');
        Co::usleep(5000);
        echo 'A after yield: ', \App\DB_PASS, ' ', \Plain_Const, "\n";
        zealphp_constants_clear();
        zealphp_superglobals_clear();
    });
    go(function () {                    // runs while A is suspended
        zealphp_superglobals_owner();
        echo 'peer sees App\DB_PASS: ', defined('App\DB_PASS') ? 'yes' : 'no', "\n";
        echo 'peer sees Plain_Const: ', defined('Plain_Const') ? 'yes' : 'no', "\n";
        zealphp_constants_clear();
        zealphp_superglobals_clear();
    });
    Co::usleep(20000);
    go(function () {                    // a later request
        zealphp_superglobals_owner();
        echo 'next request sees App\DB_PASS: ', defined('App\DB_PASS') ? 'yes' : 'no', "\n";
        zealphp_constants_clear();
        zealphp_superglobals_clear();
    });
});
zealphp_define_hook(false);
?>
--EXPECT--
peer sees App\DB_PASS: no
peer sees Plain_Const: no
A after yield: secret-a plain-a
next request sees App\DB_PASS: no
