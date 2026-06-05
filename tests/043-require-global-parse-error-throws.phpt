--TEST--
043: zealphp_require_global() propagates a parse error instead of returning false with a pending exception (#18)
--EXTENSIONS--
zealphp
--FILE--
<?php
// #18: a syntax error makes zend_compile_file() return a NULL op_array AND set
// a pending exception (ParseError). The `if (!op_array) RETURN_FALSE;` path
// returned a value while EG(exception) was live — a latent ZEND_ASSERT in a
// debug build, and a swallowed error. The fix RETURN_THROWS() when an exception
// is pending so the ParseError surfaces to the caller.
$dir = sys_get_temp_dir();
$pid = getmypid();
$bad = $dir . '/zp043_bad_' . $pid . '.php';
file_put_contents($bad, "<?php this is not valid php @#$ syntax(((");

try {
    zealphp_require_global($bad);
    echo "no-exception\n";
} catch (\ParseError $e) {
    echo "caught: ParseError\n";
} catch (\Throwable $e) {
    echo "caught: ", get_class($e), "\n";
}

@unlink($bad);
echo "done\n";
?>
--EXPECT--
caught: ParseError
done
