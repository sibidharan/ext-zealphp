--TEST--
104: the full op_array cache notices a file changed on disk (mtime/size/inode) and recompiles it instead of serving the stale code (B11)
--EXTENSIONS--
zealphp
--FILE--
<?php
zealphp_process_state_snapshot();
zealphp_include_isolation(true);
zealphp_silent_redeclare(true);
zealphp_oparray_cache_full(true);
$f = sys_get_temp_dir() . '/zp104_' . getmypid() . '.php';
file_put_contents($f, '<?php return "v1";');
echo include $f, ' ', include $f, "\n";          // compiled once, then cached
file_put_contents($f, '<?php return "v2-deployed";');
clearstatcache();
echo include $f, "\n";
file_put_contents($f, '<?php return "v3";');      // same size as v1
touch($f, time() + 5);
echo include $f, ' ', include $f, "\n";
zealphp_oparray_cache_full(false);
zealphp_silent_redeclare(false);
zealphp_include_isolation(false);
@unlink($f);
?>
--EXPECT--
v1 v1
v2-deployed
v3 v3
