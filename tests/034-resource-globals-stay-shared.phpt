--TEST--
0.3.23: RESOURCE-valued $GLOBALS stay SHARED across coroutines (deliberate exclusion); scalars isolate
--EXTENSIONS--
zealphp
--SKIPIF--
<?php
if (!extension_loaded('openswoole')) {
    die("skip resource/scalar coroutine contrast needs the OpenSwoole coroutine runtime");
}
?>
--FILE--
<?php
// Object globals isolate per coroutine (033); RESOURCES must NOT — a resource is a
// handle to one external thing (fd/socket), so snapshot/restore would risk double-
// close / UAF of that single handle. The documented boundary: resources stay shared
// (use a per-coroutine pool, not $GLOBALS). This pins the contrast — flipping
// resources to "isolatable" would reintroduce the UAF the v0.3.12 review guarded.
//
// Deterministic via channel handoff (no timing): A sets globals + yields; B then
// overwrites the shared slot; A resumes and reads.
use OpenSwoole\Coroutine as Co;
use OpenSwoole\Coroutine\Channel;
use OpenSwoole\Timer;

zealphp_coroutine_globals(true);

$result = [];
Co::run(function () use (&$result) {
    $aSet  = new Channel(1);
    $bDone = new Channel(1);

    // Coroutine A — sets a scalar + a resource global, then yields until B is done.
    go(function () use (&$result, $aSet, $bDone) {
        global $sc, $res;
        $sc   = 'A';
        $res  = fopen('php://memory', 'r+');
        $ridA = get_resource_id($res);
        $aSet->push(1);        // release B
        $bDone->pop(3.0);      // YIELD — B overwrites the shared slot meanwhile
        $result['scalar_isolated'] = ($sc === 'A');                     // A's scalar restored
        $result['resource_shared'] = (get_resource_id($res) !== $ridA); // sees B's handle
    });

    // Coroutine B — runs during A's yield, overwrites both globals.
    go(function () use ($aSet, $bDone) {
        $aSet->pop(3.0);       // wait until A set its globals and suspended
        global $sc, $res;
        $sc  = 'B';
        $res = fopen('php://memory', 'r+');
        $bDone->push(1);       // wake A
    });

    $w = new Channel(1);
    Timer::after(200, fn() => $w->push(1));
    $w->pop(3.0);
});

echo "scalar isolated: ", $result['scalar_isolated'] ? 'yes' : 'no', "\n";
echo "resource shared: ", $result['resource_shared'] ? 'yes' : 'no', "\n";
?>
--EXPECT--
scalar isolated: yes
resource shared: yes
