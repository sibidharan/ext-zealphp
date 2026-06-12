--TEST--
065: ext#48 — class statics (INCLUDING objects) are isolated per coroutine; a peer reads template defaults, never another request's singleton (S5b v2)
--EXTENSIONS--
zealphp
--SKIPIF--
<?php
// The leak window is a TRUE-concurrency scheduling artefact: request A writes a
// class static and yields, request B reads it before A's request-end reset.
// That needs the OpenSwoole on_yield/on_resume/on_close scheduler hooks, so the
// probe requires the coroutine runtime (same gate as 033/062).
if (!extension_loaded('openswoole')) {
    die("skip ext#48 concurrent-window probe needs the OpenSwoole coroutine runtime");
}
?>
--FILE--
<?php
/*
 * ext#48 — "the OOP-static-cache frontier". CHARACTERIZATION TEST.
 *
 * The isolation stack snapshots/restores class statics per coroutine yield via
 * zealphp_statics_snapshot_save()/_restore() (call sites: zealphp_on_yield ~L2266,
 * zealphp_on_resume ~L2349). But that machinery reuses zealphp_globals_isolatable()
 * (zealphp.c ~L1444), which returns FALSE for IS_OBJECT / IS_RESOURCE. So:
 *
 *   - SCALAR / ARRAY class statics ARE isolated per coroutine — PROVIDED the
 *     reading coroutine has its OWN prior snapshot of that class slot (it wrote
 *     the static, then yielded at least once, so on_resume restores ITS value).
 *
 *   - OBJECT class statics are NEVER snapshotted (the singleton/DI-container
 *     pattern: Kirby App::$instance, CakePHP Router::$_collection, a cached
 *     service object). They stay in the process-shared static_members_table, so
 *     request A's write is visible to a concurrent request B until A's
 *     request-end reset (zealphp_reset_request_class_statics) zeroes it —
 *     possibly while B is still rendering.
 *
 * This probe pins the CURRENT (leaking) behavior so a future per-coroutine
 * object-static isolation stage flips the EXPECT (bidirectional regression value).
 *
 * Determinism: a 2-slot relay Channel forces A-writes -> A-yields -> B-reads,
 * so the observation is the leak, not a scheduling coincidence.
 */

use OpenSwoole\Coroutine as Co;
use OpenSwoole\Coroutine\Channel;
use OpenSwoole\Timer;

zealphp_coroutine_globals(true);    // activates the on_yield/resume/close hooks
zealphp_coroutine_statics(true);    // S5a fn-statics + arms the class-static lane

// Plain functions are process-global (immune to the $GLOBALS isolation enabled
// above) — a yield helper and the class under probe.
function relay_yield_065(int $ms = 3): void {
    $ch = new Channel(1);
    Timer::after($ms, fn () => $ch->push(1));
    $ch->pop(2.0);
}

// The class under test. Two statics:
//   $singleton : OBJECT  -> excluded from S5b -> EXPECTED TO LEAK
//   $cache     : ARRAY   -> isolated by S5b IF the reader has its own snapshot
class Cache065 {
    public static $singleton = null;   // object singleton (Kirby App::$instance shape)
    public static array $cache = [];    // scalar/array request cache
}

final class Holder065 {
    public function __construct(public string $owner) {}
}

$out = new Channel(2);

Co::run(function () use ($out) {
    // Pre-stage: in the parent coroutine, give BOTH request coroutines a baseline
    // so the array path has a per-coroutine snapshot lane to restore from.
    Cache065::$cache['k'] = 'boot';

    // relay[0]: A signals "I have written + am about to yield" -> B proceeds.
    $relay = new Channel(1);

    // Request A: writes its statics, then yields (holding the relay open so B
    // observes the live, A-owned table).
    go(function () use ($out, $relay) {
        zealphp_superglobals_owner();
        // B's own array snapshot first so the array path is genuinely isolated
        // for B (it wrote + yielded once before A clobbers the live slot).
        Cache065::$cache['k'] = 'A';
        Cache065::$singleton  = new Holder065('A');
        $relay->push(1);            // let B run
        relay_yield_065(5);         // A yields -> B is resumed with A's live table
        // After B has read, report what A still sees (A's own values via restore).
        $out->push('A_sees_obj='   . (Cache065::$singleton->owner ?? 'null')
                 . ' A_sees_cache=' . (Cache065::$cache['k'] ?? 'null'));
    });

    // Request B: gets its own array snapshot, then reads the statics A wrote.
    go(function () use ($out, $relay) {
        zealphp_superglobals_owner();
        Cache065::$cache['k'] = 'B';     // B's own array value
        relay_yield_065(2);              // B yields -> A runs (writes 'A' + yields)
        $relay->pop(2.0);                // wait until A has written + is yielding
        // Now read. Object: process-shared -> 'A' (LEAK). Array: B restored 'B'.
        $obs_obj   = (Cache065::$singleton instanceof Holder065)
                   ? Cache065::$singleton->owner : 'null';
        $obs_cache = Cache065::$cache['k'] ?? 'null';
        $out->push("B_sees_obj=$obs_obj B_sees_cache=$obs_cache");
        relay_yield_065(8);              // let A finish its report
    });

    $r = [$out->pop(5.0), $out->pop(5.0)];
    sort($r);                            // stable order: A_… before B_…
    echo implode("\n", $r), "\n";
});

echo "DONE\n";

// BIDIRECTIONAL: on ext <= 0.3.51 this FAILS with B_sees_obj=A — the #48
// object-static leak (S5b excluded objects and never re-parked live slots).
// S5b v2 snapshots objects (ref-hold), re-parks post-boot slots to the boot
// template on yield, and drains object refs at request end in coroutine
// context — so B reads the PHP-FPM template default (null), never A’s
// singleton, while A’s own values survive its resume.
?>
--EXPECT--
A_sees_obj=A A_sees_cache=A
B_sees_obj=null B_sees_cache=B
DONE
