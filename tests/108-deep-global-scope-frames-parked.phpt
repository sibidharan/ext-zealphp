--TEST--
108: request global-scope frames deeper than 64 are parked across a yield like the shallow ones (the 64-frame cap is gone)
--EXTENSIONS--
zealphp
--SKIPIF--
<?php if (!extension_loaded('openswoole')) die('skip needs the OpenSwoole coroutine runtime'); ?>
--FILE--
<?php
// zealphp_collect_request_frame_ranges() stopped at 64 frames: a deeper
// global-scope frame was never parked, so its bucket stayed IS_INDIRECT into
// the frame across the yield and a concurrent request's frames attached to it.
use OpenSwoole\Coroutine as Co;
$dir = sys_get_temp_dir() . '/zp108_' . getmypid();
@mkdir($dir);
$depth = 80;
for ($i = 0; $i < $depth; $i++) {
    $next = $i + 1 < $depth
        ? 'zealphp_require_global(' . var_export("$dir/f" . ($i + 1) . '.php', true) . ');'
        : '$zp108_deep = $GLOBALS["zp108_tag"]; OpenSwoole\Coroutine::usleep(3000);'
          . ' $GLOBALS["zp108_seen"][$GLOBALS["zp108_tag"]] = $zp108_deep . "/" . $zp108_l0;';
    file_put_contents("$dir/f$i.php", "<?php \$zp108_l$i = \$GLOBALS['zp108_tag'];" . $next);
}
zealphp_coroutine_globals(true);
$results = [];
Co::run(function () use ($dir, &$results) {
    foreach (['A', 'B'] as $tag) {
        go(function () use ($dir, $tag, &$results) {
            zealphp_superglobals_owner();
            $GLOBALS['zp108_tag'] = $tag;
            $GLOBALS['zp108_seen'] = [];
            zealphp_require_global("$dir/f0.php");
            $results[$tag] = $GLOBALS['zp108_seen'][$tag] ?? '(lost)';
            zealphp_coroutine_globals_request_end();
        });
    }
});
zealphp_coroutine_globals(false);
ksort($results);
foreach ($results as $t => $v) echo "$t: $v\n";
for ($i = 0; $i < $depth; $i++) @unlink("$dir/f$i.php");
@rmdir($dir);
?>
--EXPECT--
A: A/A
B: B/B
