--TEST--
111: a static property written through ReflectionProperty::setValue() / ReflectionClass::setStaticPropertyValue() is isolated per coroutine like an opcode write
--SKIPIF--
<?php
if (!extension_loaded('zealphp')) die('skip zealphp not loaded');
if (!extension_loaded('openswoole')) die('skip openswoole not loaded');
?>
--FILE--
<?php
use OpenSwoole\Coroutine as Co;
use OpenSwoole\Coroutine\Channel;
Co::run(function () {
    zealphp_coroutine_globals(true);
    zealphp_coroutine_statics(true);
    eval('class Zp111 { public static $a = "tmpl"; public static $b = "tmpl"; public $inst = 0; }');
    $done = new Channel(3);
    go(function () use ($done) {
        zealphp_superglobals_owner();
        (new ReflectionProperty(Zp111::class, 'a'))->setValue(null, 'A');
        (new ReflectionClass(Zp111::class))->setStaticPropertyValue('b', 'A');
        (new ReflectionProperty(Zp111::class, 'inst'))->setValue(new Zp111(), 1);   // instance write: must not matter
        Co::usleep(3000);
        $done->push('A:' . Zp111::$a . '/' . Zp111::$b);
    });
    go(function () use ($done) {
        zealphp_superglobals_owner();
        Co::usleep(500);
        Zp111::$a = 'B';
        Co::usleep(3000);
        $done->push('B:' . Zp111::$a . '/' . Zp111::$b);
    });
    go(function () use ($done) {
        zealphp_superglobals_owner();
        Co::usleep(1500);
        $done->push('peer:' . Zp111::$a . '/' . Zp111::$b);
    });
    $r = [$done->pop(3.0), $done->pop(3.0), $done->pop(3.0)];
    sort($r);
    echo implode("\n", $r), "\n";
    zealphp_coroutine_statics(false);
    zealphp_coroutine_globals(false);
});
?>
--EXPECT--
A:A/A
B:B/tmpl
peer:tmpl/tmpl
