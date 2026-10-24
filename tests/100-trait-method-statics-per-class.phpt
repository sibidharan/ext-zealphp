--TEST--
100: function-static isolation registers each class's copy of a trait method separately, so every using class's statics are reset and isolated (B7)
--EXTENSIONS--
zealphp
--SKIPIF--
<?php if (!extension_loaded('openswoole')) die('skip needs the OpenSwoole coroutine runtime'); ?>
--FILE--
<?php
// The registry was keyed by op_array->static_variables, the template every
// trait clone shares: only the first using class was ever registered.
use OpenSwoole\Coroutine as Co;
trait Zp100Counter { public static function next() { static $n = 0; return ++$n; } }
class Zp100A { use Zp100Counter; }
class Zp100B { use Zp100Counter; }
class Zp100C { use Zp100Counter; }
Co::run(function () {
    zealphp_coroutine_statics(true);
    go(function () {
        Zp100A::next(); Zp100B::next(); Zp100C::next();
        Zp100A::next(); Zp100B::next(); Zp100C::next();
        echo 'request 1: ', Zp100A::next(), Zp100B::next(), Zp100C::next(), "\n";
        echo 'reset: ', zealphp_reset_request_statics(), "\n";
    });
    go(function () {
        echo 'request 2: ', Zp100A::next(), Zp100B::next(), Zp100C::next(), "\n";
    });
    zealphp_coroutine_statics(false);
});
?>
--EXPECTF--
request 1: 333
reset: %d
request 2: 111
