--TEST--
084: the process-lifetime toggles refuse to arm under a per-request SAPI (built-in server, like mod_php/FPM) so one request cannot leave freed state behind for the next
--EXTENSIONS--
zealphp
--SKIPIF--
<?php
if (!function_exists('proc_open') || !function_exists('fsockopen')) die('skip needs proc_open + sockets');
?>
--FILE--
<?php
// zealphp_process_state_snapshot() + zealphp_include_isolation(true), or
// zealphp_silent_redeclare(true) + zealphp_oparray_cache_full(true), used to
// arm worker-lifetime state from inside one request. A per-request SAPI frees
// that request's heap at shutdown but the flags stay set, so the NEXT request
// in the process — any vhost — walked freed tables / ran freed op_arrays (the
// built-in server process died on the second request). Drive the CLI built-in
// server, which does a full request startup/shutdown per request like mod_php.
$dir = sys_get_temp_dir() . '/zp084_' . getmypid();
@mkdir($dir);
file_put_contents("$dir/inc.php", '<?php function zp084_inc() { return 1; } class Zp084Inc { public static $x = [1, 2]; }');
file_put_contents("$dir/r.php", <<<'PHP'
<?php
if (($_GET['s'] ?? '') === 'arm') {
    set_error_handler(function ($n, $m) {
        echo "refused: ", strstr($m, '(', true), str_contains($m, 'only available under the CLI SAPI') ? '' : " ($m)", "\n";
        return true;
    });
    zealphp_process_state_snapshot();
    var_dump(zealphp_include_isolation(true), zealphp_silent_redeclare(true), zealphp_oparray_cache_full(true));
    var_dump(zealphp_include_isolation(), zealphp_silent_redeclare(), zealphp_oparray_cache_full());
    require_once __DIR__ . '/inc.php';
    return;
}
require_once __DIR__ . '/inc.php';
echo "later request ok\n";
PHP);

$args = [PHP_BINARY];
if (getenv('PHP_INI_SCAN_DIR') === false) {
    $args[] = '-n';
    $args[] = '-d';
    $args[] = 'extension_dir=' . ini_get('extension_dir');
    $args[] = '-d';
    $args[] = 'extension=zealphp';
}
$port = 0;
for ($try = 0; $try < 20 && !$port; $try++) {
    $p = 18150 + random_int(0, 9);
    $s = @fsockopen('127.0.0.1', $p, $e1, $e2, 0.2);
    if ($s) { fclose($s); continue; }
    $port = $p;
}
$cmd = array_merge($args, ['-d', 'opcache.enable=0', '-S', "127.0.0.1:$port", "$dir/r.php"]);
$proc = proc_open($cmd, [0 => ['pipe', 'r'], 1 => ['file', '/dev/null', 'w'], 2 => ['file', '/dev/null', 'w']], $pipes);
$get = function (string $q) use ($port): string {
    for ($i = 0; $i < 50; $i++) {
        $r = @file_get_contents("http://127.0.0.1:$port/?s=$q", false, stream_context_create(['http' => ['timeout' => 3]]));
        if ($r !== false) return $r;
        usleep(100000);
    }
    return "(no response)\n";
};
echo $get('arm');
echo $get('later1');
echo $get('later2');
proc_terminate($proc, 9);
proc_close($proc);
@unlink("$dir/inc.php"); @unlink("$dir/r.php"); @rmdir($dir);
?>
--EXPECTF--
refused: zealphp_process_state_snapshot
refused: zealphp_include_isolation
refused: zealphp_silent_redeclare
refused: zealphp_oparray_cache_full
bool(false)
bool(false)
bool(false)
bool(false)
bool(false)
bool(false)
later request ok
later request ok
