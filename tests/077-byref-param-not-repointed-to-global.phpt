--TEST--
077: resume never repoints a function's by-ref parameter / foreach-by-ref local at a same-named global (only `global $x` bindings are repaired)
--EXTENSIONS--
zealphp
--SKIPIF--
<?php
if (!extension_loaded('openswoole')) {
    die('skip needs the OpenSwoole coroutine runtime');
}
if (!function_exists('zealphp_coroutine_globals')) {
    die('skip needs $GLOBALS isolation');
}
?>
--FILE--
<?php
// The #29 repair walk repointed ANY reference-typed CV named like a live
// array/object global, so `array &$zp_cfg` silently wrote into $GLOBALS.
use OpenSwoole\Coroutine as Co;
use OpenSwoole\Coroutine\Channel;
use OpenSwoole\Timer;

function zp077_yield(int $ms): void {
    $ch = new Channel(1);
    Timer::after($ms, static fn() => $ch->push(1));
    $ch->pop(2.0);
}
function zp077_mut(array &$zp_cfg): void { zp077_yield(5); $zp_cfg['touched'] = 1; }
function zp077_each(array $rows): array {
    foreach ($rows as &$zp_row) { zp077_yield(2); $zp_row['seen'] = 1; }
    unset($zp_row);
    return $rows;
}
function zp077_bound(): void { global $zp_obj; zp077_yield(3); $zp_obj->n++; }

zealphp_coroutine_globals(true);
$out = [];
Co::run(function () use (&$out) {
    go(function () use (&$out) {
        zealphp_superglobals_owner();
        $GLOBALS['zp_cfg'] = ['o' => 'g'];
        $GLOBALS['zp_row'] = ['o' => 'g'];
        $GLOBALS['zp_obj'] = (object) ['n' => 0];
        $local = ['o' => 'l'];
        zp077_mut($local);
        $rows = zp077_each([['o' => 'l']]);
        zp077_bound();
        $out = [
            'param local touched'    => isset($local['touched']),
            'param global untouched' => !isset($GLOBALS['zp_cfg']['touched']),
            'foreach local seen'     => isset($rows[0]['seen']),
            'foreach global clean'   => !isset($GLOBALS['zp_row']['seen']),
            'global $x still bound'  => $GLOBALS['zp_obj']->n === 1,
        ];
    });
});
foreach ($out as $k => $v) { echo $k, ": ", $v ? 'yes' : 'NO', "\n"; }
zealphp_coroutine_globals(false);
?>
--EXPECT--
param local touched: yes
param global untouched: yes
foreach local seen: yes
foreach global clean: yes
global $x still bound: yes
