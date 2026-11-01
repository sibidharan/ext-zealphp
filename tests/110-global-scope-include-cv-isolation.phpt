--TEST--
110: a global created by a file INCLUDED from the request's global frame (a plain CV write, no write opcode) is still isolated per coroutine and parked on yield
--SKIPIF--
<?php
if (!extension_loaded('zealphp')) die('skip zealphp not loaded');
if (!extension_loaded('openswoole')) die('skip openswoole not loaded');
?>
--FILE--
<?php
use OpenSwoole\Coroutine as Co;
use OpenSwoole\Coroutine\Channel;
$dir = sys_get_temp_dir() . '/zp110_' . getmypid();
@mkdir($dir);
file_put_contents("$dir/inc_A.php", "<?php\n\$g110 = 'A'; \$h110 = ['who' => 'A'];\n");
file_put_contents("$dir/inc_B.php", "<?php\n\$g110 = 'B'; \$h110 = ['who' => 'B'];\n");
foreach (['A', 'B'] as $id) {
    file_put_contents("$dir/entry_$id.php", "<?php\n"
        . "include '$dir/inc_$id.php';\n"            // nested global-scope include: CV writes, no hook
        . "OpenSwoole\\Coroutine::usleep(3000);\n"
        . "return (\$GLOBALS['g110'] ?? '-') . '/' . (\$GLOBALS['h110']['who'] ?? '-');\n");
}
Co::run(function () use ($dir) {
    zealphp_coroutine_globals(true);
    $done = new Channel(3);
    foreach (['A', 'B'] as $id) {
        go(function () use ($id, $dir, $done) {
            zealphp_superglobals_owner();
            $done->push("$id:" . zealphp_require_global("$dir/entry_$id.php"));
            zealphp_coroutine_globals_request_end();
        });
    }
    go(function () use ($done) {
        zealphp_superglobals_owner();
        Co::usleep(1500);   // both entries are parked mid-yield now
        $done->push('peer:' . ($GLOBALS['g110'] ?? '-') . '/' . ($GLOBALS['h110']['who'] ?? '-'));
        zealphp_coroutine_globals_request_end();
    });
    $r = [$done->pop(3.0), $done->pop(3.0), $done->pop(3.0)];
    sort($r);
    echo implode("\n", $r), "\n";
    zealphp_coroutine_globals(false);
});
array_map('unlink', glob("$dir/*.php")); @rmdir($dir);
?>
--EXPECT--
A:A/A
B:B/B
peer:-/-
