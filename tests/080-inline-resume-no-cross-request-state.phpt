--TEST--
080: a request resumed inline by another request's Channel::push neither sees nor inherits the pusher's state, and the pusher keeps its own (no session wipe)
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
// OpenSwoole's Channel::push() resumes a waiting pop()er INLINE, on the
// pusher's C stack: on_resume(B) fires while request A is still the running
// coroutine, and B's yield returns straight into A. The scheduler hooks used
// to assume every resume came from the event loop, so A's live state was
// overwritten by B's restore and never parked/re-applied: B saw A's class
// statics and kept them, and A continued with B's leftovers / empty
// superglobals (its session wiped by write_close). This is the connection
// pool shape: A releases a connection while B waits in pop().
use OpenSwoole\Coroutine as Co;
use OpenSwoole\Coroutine\Channel;
use OpenSwoole\Timer;

function zp080_yield(int $ms): void {
    $c = new Channel(1);
    Timer::after($ms, static fn() => $c->push(1));
    $c->pop(2.0);
}
class Zp080Auth { public static ?string $user = null; }

zealphp_coroutine_superglobals(true);
$out = [];
Co::run(function () use (&$out) {
    $pool = new Channel(1);
    $done = new Channel(2);
    go(function () use (&$out, $pool, $done) {                 // request "bob"
        zealphp_request_input_set(['who' => 'bob'], [], [], [], [], []);
        $pool->pop(2.0);                                        // alice's push resumes bob INLINE
        $out['bob_inline_user'] = Zp080Auth::$user;
        $out['bob_inline_get'] = $_GET['who'] ?? 'MISSING';
        $out['bob_inline_sess'] = $_SESSION['uid'] ?? 'MISSING';
        zp080_yield(10);
        $out['bob_later_user'] = Zp080Auth::$user;
        $out['bob_get'] = $_GET['who'] ?? 'MISSING';
        $done->push(1);
    });
    go(function () use (&$out, $pool, $done) {                 // request "alice"
        zealphp_request_input_set(['who' => 'alice'], [], [], [], [], []);
        zp080_yield(2);
        Zp080Auth::$user = 'alice';
        $_SESSION = ['uid' => 'alice'];
        $pool->push(1);                                         // bob runs inline, then yields back here
        $out['alice_get'] = $_GET['who'] ?? 'MISSING';
        $out['alice_user'] = Zp080Auth::$user;
        $out['alice_sess'] = $_SESSION['uid'] ?? 'MISSING';
        zp080_yield(20);
        $out['alice_sess_later'] = $_SESSION['uid'] ?? 'MISSING';
        $done->push(1);
    });
    $done->pop(5.0);
    $done->pop(5.0);
});
// Nested + never-yielded resumer: carol pushes before her first yield (her
// task is known only from the coroutine-start hook), dave resumed inline
// pushes on to erin inline; each returns to its own resumer intact.
Zp080Auth::$user = null;   // the framework's request-end statics reset
Co::run(function () use (&$out) {
    $c1 = new Channel(1);
    $c2 = new Channel(1);
    $done = new Channel(3);
    go(function () use (&$out, $c2, $done) {                   // request "erin"
        zealphp_request_input_set(['who' => 'erin'], [], [], [], [], []);
        $c2->pop(2.0);
        $out['erin_get'] = $_GET['who'] ?? 'MISSING';
        $out['erin_user'] = Zp080Auth::$user;
        $done->push(1);
    });
    go(function () use (&$out, $c1, $c2, $done) {              // request "dave"
        zealphp_request_input_set(['who' => 'dave'], [], [], [], [], []);
        $c1->pop(2.0);
        Zp080Auth::$user = 'dave';
        $c2->push(1);                                           // erin inline, on top of carol's stack
        $out['dave_get'] = $_GET['who'] ?? 'MISSING';
        $out['dave_user'] = Zp080Auth::$user;
        $done->push(1);
    });
    go(function () use (&$out, $c1, $done) {                   // request "carol"
        zealphp_request_input_set(['who' => 'carol'], [], [], [], [], []);
        Zp080Auth::$user = 'carol';
        $c1->push(1);                                           // dave inline, before carol ever yielded
        $out['carol_get'] = $_GET['who'] ?? 'MISSING';
        $out['carol_user'] = Zp080Auth::$user;
        $done->push(1);
    });
    $done->pop(5.0); $done->pop(5.0); $done->pop(5.0);
});
foreach (['bob_inline_user', 'bob_inline_get', 'bob_inline_sess', 'bob_later_user', 'bob_get',
          'alice_get', 'alice_user', 'alice_sess', 'alice_sess_later',
          'erin_get', 'erin_user', 'dave_get', 'dave_user', 'carol_get', 'carol_user'] as $k) {
    echo $k, ': ', var_export(array_key_exists($k, $out) ? $out[$k] : 'UNSET', true), "\n";
}
?>
--EXPECT--
bob_inline_user: NULL
bob_inline_get: 'bob'
bob_inline_sess: 'MISSING'
bob_later_user: NULL
bob_get: 'bob'
alice_get: 'alice'
alice_user: 'alice'
alice_sess: 'alice'
alice_sess_later: 'alice'
erin_get: 'erin'
erin_user: NULL
dave_get: 'dave'
dave_user: 'dave'
carol_get: 'carol'
carol_user: 'carol'
