--TEST--
089: resume never repoints a by-ref CV of top-level code running in a FUNCTION's scope (a template included from a function) at a same-named global
--EXTENSIONS--
zealphp
--SKIPIF--
<?php
if (!extension_loaded('openswoole')) {
    die('skip needs the OpenSwoole coroutine runtime');
}
?>
--FILE--
<?php
// Test 077 covers function frames. File-scope code included from inside a
// function (a template render) has no function_name but runs in the
// function's local symbol table; the repair walk still treated its by-ref
// `foreach (... as &$row)` CV as a stale `global $row` binding and repointed
// it, so the loop wrote into $GLOBALS['zp_row'].
use OpenSwoole\Coroutine as Co;
use OpenSwoole\Coroutine\Channel;
use OpenSwoole\Timer;

function zp089_yield(int $ms): void {
    $ch = new Channel(1);
    Timer::after($ms, static fn() => $ch->push(1));
    $ch->pop(2.0);
}
$tpl = sys_get_temp_dir() . '/zp089_' . getmypid() . '.php';
file_put_contents($tpl, '<?php foreach ($rows as &$zp_row) { zp089_yield(2); $zp_row["seen"] = 1; } unset($zp_row);');
function zp089_render(string $tpl): array {
    $rows = [['o' => 'l']];
    include $tpl;
    return $rows;
}

zealphp_coroutine_globals(true);
$out = [];
Co::run(function () use (&$out, $tpl) {
    go(function () use (&$out, $tpl) {
        zealphp_superglobals_owner();
        $GLOBALS['zp_row'] = ['o' => 'g'];
        $rows = zp089_render($tpl);
        $out = [
            'template local seen'  => isset($rows[0]['seen']),
            'global row untouched' => !isset($GLOBALS['zp_row']['seen']) && ($GLOBALS['zp_row']['o'] ?? null) === 'g',
        ];
    });
});
@unlink($tpl);
foreach ($out as $k => $v) { echo $k, ": ", $v ? 'yes' : 'NO', "\n"; }
zealphp_coroutine_globals(false);
?>
--EXPECT--
template local seen: yes
global row untouched: yes
