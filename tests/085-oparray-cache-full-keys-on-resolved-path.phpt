--TEST--
085: the full op_array cache keys on the resolved file, so a relative include of the same name from two directories never serves one file's code for the other
--EXTENSIONS--
zealphp
--INI--
opcache.enable_cli=0
--FILE--
<?php
// The cache key was the unresolved include string: `include 'inc.php'` from
// a/ and from b/ shared the key "inc.php", and the second include ran a/'s
// code. In a real app that is /admin/header.php vs /header.php.
zealphp_silent_redeclare(true);
zealphp_oparray_cache_full(true);
$b = sys_get_temp_dir() . '/zp085_' . getmypid();
@mkdir("$b/a", 0777, true);
@mkdir("$b/b", 0777, true);
file_put_contents("$b/a/inc.php", '<?php echo "A\n";');
file_put_contents("$b/b/inc.php", '<?php echo "B\n";');
$cwd = getcwd();
chdir("$b/a"); include 'inc.php';
chdir("$b/b"); include 'inc.php';
chdir("$b/a"); include 'inc.php';
chdir($cwd);
zealphp_oparray_cache_full(false);
zealphp_silent_redeclare(false);
@unlink("$b/a/inc.php"); @unlink("$b/b/inc.php"); @rmdir("$b/a"); @rmdir("$b/b"); @rmdir($b);
?>
--EXPECT--
A
B
A
