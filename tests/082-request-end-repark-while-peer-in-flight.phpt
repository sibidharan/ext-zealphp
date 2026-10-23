--TEST--
082: a request that changes a process setting (last error, cwd) after its last yield does not hand it to the next request while another request is still in flight
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
// The request-end re-park lives in on_close's *_snapshot_delete calls, which
// are lane-or-owner gated. on_close used to drop the closing coroutine from
// the request set BEFORE those calls, so with any other request in flight the
// closing request no longer counted as an owner and nothing was re-parked: a
// warning (path, key, user input) or a chdir() made after its last yield was
// what the next fresh request started with.
use OpenSwoole\Coroutine as Co;
use OpenSwoole\Coroutine\Channel;
use OpenSwoole\Timer;

function zp082_yield(int $ms): void {
    $c = new Channel(1);
    Timer::after($ms, static fn() => $c->push(1));
    $c->pop(2.0);
}

$base = getcwd();
$dir = sys_get_temp_dir() . '/zp082_' . getmypid();
@mkdir($dir);
zealphp_last_error_isolation(true);
zealphp_cwd_isolation(true);
$seen = [];
Co::run(function () use (&$seen, $dir) {
    $d = new Channel(2);
    go(function () use ($d) {                         // keeps the request set non-empty
        zealphp_superglobals_owner();
        zp082_yield(80);
        $d->push(1);
    });
    go(function () use ($dir) {                       // request A: no yield after these
        zealphp_superglobals_owner();
        zp082_yield(5);
        @trigger_error('SECRET-OF-A', E_USER_NOTICE);
        chdir($dir);
    });
    zp082_yield(30);
    go(function () use (&$seen, $d) {                 // a fresh request after A closed
        zealphp_superglobals_owner();
        $seen['last_error'] = error_get_last()['message'] ?? null;
        $seen['cwd'] = getcwd();
        $d->push(1);
    });
    $d->pop(5.0);
    $d->pop(5.0);
});
zealphp_cwd_isolation(false);
zealphp_last_error_isolation(false);
@rmdir($dir);
var_dump($seen['last_error']);
var_dump($seen['cwd'] === $base);
?>
--EXPECT--
NULL
bool(true)
