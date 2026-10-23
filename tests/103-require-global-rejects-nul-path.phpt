--TEST--
103: zealphp_require_global() rejects a path containing a NUL byte instead of truncating it (B11)
--EXTENSIONS--
zealphp
--FILE--
<?php
$dir = sys_get_temp_dir() . '/zp103_' . getmypid();
@mkdir($dir);
file_put_contents("$dir/a.php", '<?php return "included a.php";');
try {
    var_dump(zealphp_require_global("$dir/a.php\0.png"));
} catch (\ValueError $e) {
    echo get_class($e), ': ', str_contains($e->getMessage(), 'null bytes') ? 'null bytes refused' : $e->getMessage(), "\n";
}
var_dump(zealphp_require_global("$dir/a.php"));
@unlink("$dir/a.php"); @rmdir($dir);
?>
--EXPECT--
ValueError: null bytes refused
string(14) "included a.php"
