--TEST--
083: a request that ini_set()s memory_limit still gets the OOM-safe switch: the ini stage does not re-apply the limit inside the scheduler callback
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
// Test 078's scenario, plus an ini_set('memory_limit') in the request. The ini
// stage re-applied each modified directive through zend_alter_ini_entry_ex on
// yield; for memory_limit that runs OnChangeMemoryLimit -> zend_set_memory_limit,
// cancelling the callback's limit lift for every stage after it (statics,
// $GLOBALS), so the $GLOBALS copy hit the limit inside on_yield again.
use OpenSwoole\Coroutine as Co;
use OpenSwoole\Coroutine\Channel;
use OpenSwoole\Timer;

function zp083_yield(int $ms): void {
    $ch = new Channel(1);
    Timer::after($ms, static fn() => $ch->push(1));
    $ch->pop(2.0);
}

zealphp_coroutine_globals(true);
$out = [];
Co::run(function () use (&$out) {
    go(function () use (&$out) {
        zealphp_superglobals_owner();
        ini_set('memory_limit', '48M');
        $GLOBALS['zp083_big'] = range(1, 1500000);   // ~34 MB: fits 48M, the switch copy does not
        zp083_yield(5);
        $out['survived_yield'] = count($GLOBALS['zp083_big']) === 1500000;
        $out['limit_after_resume'] = ini_get('memory_limit');
        unset($GLOBALS['zp083_big']);
    });
});
echo "survived yield: ", ($out['survived_yield'] ?? false) ? 'yes' : 'NO', "\n";
echo "limit after resume: ", $out['limit_after_resume'] ?? 'UNSET', "\n";
zealphp_coroutine_globals(false);

$x = str_repeat('a', 200 * 1024 * 1024);
echo "limit enforced: NO\n";
?>
--EXPECTF--
survived yield: yes
limit after resume: 48M

Fatal error: Allowed memory size of 50331648 bytes exhausted%s
