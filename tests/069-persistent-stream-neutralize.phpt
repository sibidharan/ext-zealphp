--TEST--
069: ext#45 — persistent userland sockets (pfsockopen / STREAM_CLIENT_PERSISTENT) are neutralized to non-persistent under the coroutine hooks (no orig_path heap corruption, no cross-coroutine fd reuse)
--SKIPIF--
<?php
if (!extension_loaded('zealphp')) die('skip zealphp not loaded');
if (!extension_loaded('openswoole')) die('skip openswoole not loaded');
?>
--FILE--
<?php
// ext#45: a persistent-keyed userland socket created under HOOK_ALL heap-corrupts
// at teardown — the generic xport layer pestrdup()s stream->orig_path PERSISTENT,
// but the coroutine-hooked tcp/unix factory returns an emalloc'd NON-persistent
// stream, so php_stream_free() pefree()s a malloc'd orig_path → _efree() on a
// malloc pointer → "zend_mm_heap corrupted". It is ALSO unsafe across coroutines
// (the persistent fd in EG(persistent_list) is reused, interleaving wire frames).
//
// When the coroutine hooks install, the ext neutralizes persistence:
//   pfsockopen           → routed to fsockopen's handler (non-persistent)
//   stream_socket_client → STREAM_CLIENT_PERSISTENT stripped from the flags arg
// So 3 pfsockopen + 3 persistent stream_socket_client calls open 6 DISTINCT
// connections. The server holds every accepted fd open, so a *persistent* client
// would reuse one fd per kind (=> 2 accepts); a neutralized non-persistent client
// makes 6 fresh connections (=> 6 accepts) and the script tears down cleanly.
// A build WITHOUT the fix crashes here ("zend_mm_heap corrupted") instead.

OpenSwoole\Runtime::enableCoroutine(OpenSwoole\Runtime::HOOK_ALL);
Co::run(function () {
    zealphp_coroutine_superglobals(true);   // installs the coroutine hooks + persist-neutralize

    $port     = 9789;
    $accepts  = 0;
    $held     = [];
    $srvReady = new OpenSwoole\Coroutine\Channel(1);
    $done     = new OpenSwoole\Coroutine\Channel(1);

    go(function () use ($port, &$accepts, &$held, $srvReady, $done) {
        $srv = @stream_socket_server("tcp://127.0.0.1:$port", $e, $s);
        if (!$srv) { $srvReady->push(false); return; }
        $srvReady->push(true);
        // Neutralized (non-persistent) → exactly 6 distinct connections arrive, so
        // accept exactly 6 and exit cleanly (no accept-timeout warning). A buggy
        // (still-persistent) build reuses one fd per kind → only 2 connects → the
        // 3rd accept times out → accepts=2 ≠ 6 (and/or a teardown crash) → FAIL.
        for ($i = 0; $i < 6; $i++) {
            $c = stream_socket_accept($srv, 5.0);
            if ($c === false) break;
            $accepts++;
            $held[] = $c;                     // hold open so a persistent client could reuse one fd
        }
        @fclose($srv);
        foreach ($held as $h) { @fclose($h); }
        $done->push($accepts);
    });

    if ($srvReady->pop(2.0) !== true) { echo "no-server\n"; return; }

    for ($i = 0; $i < 3; $i++) {
        $f = @pfsockopen('127.0.0.1', $port, $errno, $errstr, 2);
        if ($f) { fclose($f); }
        usleep(100000);                       // 100ms; coroutine-yields under HOOK_ALL
    }
    for ($i = 0; $i < 3; $i++) {
        $f = @stream_socket_client("tcp://127.0.0.1:$port", $errno, $errstr, 2,
            STREAM_CLIENT_CONNECT | STREAM_CLIENT_PERSISTENT);
        if ($f) { fclose($f); }
        usleep(100000);
    }
    usleep(200000);

    echo "accepts=" . $done->pop(3.0) . "\n";
    echo "ALIVE\n";
});
echo "DONE\n";
?>
--EXPECT--
accepts=6
ALIVE
DONE
