--TEST--
081: a coroutine resumed inline from inside a compile (an error handler pushing to a log channel) does not steal the compiling coroutine's CG-table swap (no "Cannot redeclare" on re-execution)
--EXTENSIONS--
zealphp
--SKIPIF--
<?php
if (!extension_loaded('openswoole')) {
    die('skip needs the OpenSwoole coroutine runtime');
}
?>
--INI--
opcache.enable_cli=0
--FILE--
<?php
// Stage 3c swaps CG(function_table)/CG(class_table) to stack-local scratch
// tables while a file compiles. A compile-time deprecation runs the user
// error handler mid-compile; when that handler pushes to a channel a log
// consumer is waiting on, the consumer is resumed INLINE. Its yield used to
// stash the compiler's scratch tables under the CONSUMER's key and point CG
// back at the real tables, so the rest of the file declared straight into the
// real tables and the next re-execution fataled "Cannot redeclare".
use OpenSwoole\Coroutine as Co;
use OpenSwoole\Coroutine\Channel;

zealphp_coroutine_superglobals(true);
zealphp_silent_redeclare(true);
$f = sys_get_temp_dir() . '/zp081_' . getmypid() . '.php';
file_put_contents($f, '<?php function zp081_a($x = 1, $y) {} function zp081_b() {}');
$logged = 0;
Co::run(function () use ($f, &$logged) {
    $q = new Channel(1);
    go(function () use ($q, &$logged) {                         // unregistered log consumer
        while ($q->pop() !== false) { $logged++; }
    });
    go(function () use ($f, $q) {
        zealphp_superglobals_owner();
        set_error_handler(function ($n, $m) use ($q) { $q->push($m); return true; });
        include $f;
        include $f;
        restore_error_handler();
        echo "ok\n";
        $q->close();
    });
});
@unlink($f);
echo "logged: ", $logged > 0 ? 'yes' : 'no', "\n";
echo "fns: ", function_exists('zp081_a') && function_exists('zp081_b') ? 'yes' : 'no', "\n";
?>
--EXPECT--
ok
logged: yes
fns: yes
