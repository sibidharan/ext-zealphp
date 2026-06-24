--TEST--
066: ext#54 — a CONSTANT-EXPRESSION class-static default survives the per-request reset (no IS_UNDEF on request #2; Slim Response::$messages -> Shaarli)
--EXTENSIONS--
zealphp
--SKIPIF--
<?php
// The bug fires only under the on_yield/reset coroutine machinery (same gate as
// 065): a const-expr static default first resolved inside a request coroutine,
// then reset, reads back as IS_UNDEF before the fix.
if (!extension_loaded('openswoole')) {
    die("skip ext#54 const-expr class-static needs the OpenSwoole coroutine runtime");
}
?>
--FILE--
<?php
/*
 * ext#54 — a class first resolved inside a request coroutine whose static-property
 * default is a CONSTANT EXPRESSION (references constants from ANOTHER, not-yet-
 * loaded class) was reset to IS_UNDEF on request #2+ under coroutine-legacy. Slim 3
 * stores its reason phrases this way (Response::$messages, keyed by
 * StatusCode::HTTP_* class constants), so every Slim/Shaarli request after the
 * first threw "ReasonPhrase must be supplied for this code".
 *
 * Trigger (all three needed): (1) the const-expr references a constant from a class
 * that is NOT loaded when this class compiles -> the default stays an unresolved
 * IS_CONSTANT_AST in default_static_members_table (the engine resolves it only into
 * the LIVE table on first access); (2) the class is first resolved INSIDE a request
 * coroutine; (3) the per-request class-static reset runs. The reset (and the
 * on_yield snapshot-save re-park) copy the template into the live slot — copying
 * the raw AST leaves the static unresolved, reading as "unknown type".
 *
 * Fix: resolve the const-expr template IN PLACE during the reset (safe coroutine
 * PHP context), so every subsequent reset + re-park copies the RESOLVED default.
 *
 * Bidirectional: before the fix this prints "c1=3 c2=NOTARR:unknown type".
 */
use OpenSwoole\Coroutine as Co;
use OpenSwoole\Coroutine\Channel;
use OpenSwoole\Timer;

zealphp_coroutine_globals(true);   // on_yield/resume/close hooks
zealphp_coroutine_statics(true);   // arm the class-static lane

function yld66(int $ms = 3): void {
    $ch = new Channel(1);
    Timer::after($ms, fn () => $ch->push(1));
    $ch->pop(2.0);
}

// Two SEPARATE files, autoloaded independently. CE66f's const-expr references
// SC66f::A, but SC66f is NOT loaded when CE66f compiles, so the default is an
// unresolved AST in the template (the #54 precondition).
$dir = sys_get_temp_dir() . '/zl066_' . getmypid();
@mkdir($dir);
file_put_contents("$dir/SC66f.php", "<?php\nclass SC66f { const A=1; const B=2; const C=3; }\n");
file_put_contents("$dir/CE66f.php", "<?php\nclass CE66f { public static \$m = [SC66f::A=>'x', SC66f::B=>'y', SC66f::C=>'z']; }\n");
spl_autoload_register(function ($c) use ($dir) { $f = "$dir/$c.php"; if (is_file($f)) require $f; });

$out = new Channel(1);
Co::run(function () use ($out) {
    go(function () use ($out) {
        zealphp_superglobals_owner();
        // First access autoloads CE66f, then resolves its const-expr (autoloading
        // SC66f) INTO THE LIVE TABLE; default_static_members_table keeps the AST.
        $c1 = is_array(CE66f::$m) ? count(CE66f::$m) : 'NOTARR';
        // Per-request reset + rtcache reset (what the framework runs each request).
        zealphp_reset_request_class_statics();
        zealphp_reset_request_rtcaches();
        // Yield through the snapshot/re-park cycle, then read back.
        yld66(5);
        $c2 = is_array(CE66f::$m) ? count(CE66f::$m) : ('NOTARR:' . gettype(CE66f::$m));
        $out->push("c1=$c1 c2=$c2");
    });
    echo $out->pop(5.0), "\n";
});
echo "DONE\n";

// cleanup
@unlink("$dir/SC66f.php"); @unlink("$dir/CE66f.php"); @rmdir($dir);
?>
--EXPECT--
c1=3 c2=3
DONE
