--TEST--
Stage 3: silent-redeclare suppresses E_COMPILE_ERROR on second class declaration
--EXTENSIONS--
zealphp
--FILE--
<?php
$tmp = tempnam(sys_get_temp_dir(), 'zealphp_silent_class_');
file_put_contents($tmp . '.php', '<?php class ZealphpTestRedeclareCls { public function who() { return "first"; } }');

require $tmp . '.php';
echo (new ZealphpTestRedeclareCls)->who(), "\n";

zealphp_silent_redeclare(true);
require $tmp . '.php';
echo "second-include ok\n";
echo (new ZealphpTestRedeclareCls)->who(), "\n";

zealphp_silent_redeclare(false);
@unlink($tmp);
@unlink($tmp . '.php');
?>
--EXPECT--
first
second-include ok
first
