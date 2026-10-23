--TEST--
078: a $GLOBALS snapshot that would exceed memory_limit inside the yield callback does not bailout from the scheduler (no SIGSEGV / deadlock); the limit is still enforced afterwards
--EXTENSIONS--
zealphp
--SKIPIF--
<?php
if (!extension_loaded('openswoole')) {
    die('skip needs the OpenSwoole coroutine runtime');
}
?>
--INI--
memory_limit=48M
--FILE--
<?php
// coroutine-legacy deep-copies changed $GLOBALS on every yield. A request whose
// own data fits the limit used to have the COPY trip memory_limit inside
// OpenSwoole's on_yield callback; the OOM bailout then longjmp'd onto another
// coroutine's stack (SIGSEGV in zend_mm_gc, or a scheduler deadlock).
use OpenSwoole\Coroutine as Co;
use OpenSwoole\Coroutine\Channel;
use OpenSwoole\Timer;

function zp078_yield(int $ms): void {
    $ch = new Channel(1);
    Timer::after($ms, static fn() => $ch->push(1));
    $ch->pop(2.0);
}

zealphp_coroutine_globals(true);
$out = [];
Co::run(function () use (&$out) {
    go(function () use (&$out) {
        zealphp_superglobals_owner();
        $GLOBALS['zp078_big'] = range(1, 1500000);   // ~34 MB: fits 48M, the switch copy does not
        zp078_yield(5);
        $out['survived_yield'] = count($GLOBALS['zp078_big']) === 1500000;
        unset($GLOBALS['zp078_big']);
    });
});
echo "survived yield: ", ($out['survived_yield'] ?? false) ? 'yes' : 'NO', "\n";
zealphp_coroutine_globals(false);

// The limit itself must still hold once the switch is over.
$x = str_repeat('a', 200 * 1024 * 1024);
echo "limit enforced: NO\n";
?>
--EXPECTF--
survived yield: yes

Fatal error: Allowed memory size of 50331648 bytes exhausted%s
