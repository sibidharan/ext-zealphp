--TEST--
040: class-static reset keeps cached FETCH_STATIC_PROP slots valid (in-place reset, no use-after-free)
--SKIPIF--
<?php if (!extension_loaded('zealphp')) die('skip zealphp not loaded'); ?>
--FILE--
<?php
/*
 * Regression for the dangling-rtcache-slot use-after-free in
 * zealphp_reset_request_class_statics(). A function's run_time_cache caches a
 * raw pointer into a class's static_members_table (ZEND_FETCH_STATIC_PROP).
 *
 * The OLD implementation freed + reallocated the table (zend_cleanup_internal_
 * class_data + lazy re-init at a NEW address), leaving that cached slot dangling
 * into freed memory — so a read via the cached slot returned the STALE value
 * (and, under heap reuse, a wild Z_STRLEN -> multi-TB alloc / SEGV). The
 * in-place reset keeps the table at a STABLE address, so the cached slot reads
 * the correctly RESET value.
 *
 * This test does NOT call zealphp_reset_request_rtcaches(), deliberately
 * modelling the gap the rtcache reset leaves for SNAPSHOT functions (which it
 * skips): the class-static reset must be self-sufficient.
 */
class Cfg {
    public static string $s = 'default';
    public static int $n = 0;
}

// First call compiles + caches the FETCH_STATIC_PROP slot for Cfg::$s / Cfg::$n.
function readS(): string { return Cfg::$s; }
function readN(): int { return Cfg::$n; }

Cfg::$s = 'request1';
Cfg::$n = 42;
var_dump(readS());   // caches the slot -> "request1"
var_dump(readN());   // caches the slot -> 42

$count = zealphp_reset_request_class_statics();
var_dump($count >= 1);

// Read THROUGH the still-cached slots. In-place reset -> the template defaults.
var_dump(readS());   // must be "default", not the stale "request1"
var_dump(readN());   // must be 0, not the stale 42

// Force heap churn: with a freed table the next read would hit reused memory.
$junk = [];
for ($i = 0; $i < 5000; $i++) { $junk[] = str_repeat('x', 64); }
var_dump(strlen(readS()));   // must be 7 ("default"), never a wild length

echo "ok\n";
?>
--EXPECT--
string(8) "request1"
int(42)
bool(true)
string(7) "default"
int(0)
int(7)
ok
