--TEST--
091: an object global whose __destruct adds globals does not run inside the request-end reset's walk of the symbol table (no use-after-free of the resized table)
--EXTENSIONS--
zealphp
--SKIPIF--
<?php
if (!extension_loaded('openswoole')) {
    die('skip needs the OpenSwoole coroutine runtime');
}
?>
--ENV--
USE_ZEND_ALLOC=0
MALLOC_PERTURB_=165
--FILE--
<?php
// zealphp_globals_reset_to_parent() released each displaced value with
// zval_ptr_dtor() INSIDE its ZEND_HASH_FOREACH over EG(symbol_table). Dropping
// the last ref of an object global runs its __destruct right there; a
// destructor that writes new globals grows (reallocates) the table the walk is
// still reading. The displaced values are now released after the walk.
use OpenSwoole\Coroutine as Co;
use OpenSwoole\Coroutine\Channel;
use OpenSwoole\Timer;

class Zp091Grow {
    public function __destruct() {
        for ($i = 0; $i < 200; $i++) { $GLOBALS["zp091_new_$i"] = str_repeat('y', 32) . $i; }
        echo "destructed\n";
    }
}
zealphp_coroutine_globals(true);
Co::run(function () {
    go(function () {
        zealphp_superglobals_owner();
        $GLOBALS['zp091_obj'] = new Zp091Grow();
        for ($i = 0; $i < 6; $i++) { $GLOBALS["zp091_pad_$i"] = $i; }
        $c = new Channel(1);
        Timer::after(2, static fn() => $c->push(1));
        $c->pop(1.0);
        zealphp_coroutine_globals_request_end();
        echo "request end done\n";
    });
});
zealphp_coroutine_globals(false);
echo "survived\n";
?>
--EXPECT--
destructed
request end done
survived
