--TEST--
052: per-coroutine CWD isolation — a chdir() in one coroutine never leaks to peers (framework #323)
--EXTENSIONS--
zealphp
--SKIPIF--
<?php
if (!extension_loaded('openswoole')) {
    die('skip per-coroutine CWD isolation needs the OpenSwoole coroutine runtime');
}
?>
--FILE--
<?php
// chdir() is a PROCESS-level syscall: under coroutine concurrency one
// request's chdir('/tmp/...') used to change the working directory of every
// concurrently-running peer (and the framework's own chdir-to-script-dir in
// executeFile() raced itself). With zealphp_cwd_isolation(true):
//   - each coroutine that chdir's gets ITS OWN cwd back after every resume
//   - coroutines that never chdir always see the worker baseline
//   - after all coroutines end, the process is re-parked at the baseline
use OpenSwoole\Coroutine as Co;
use OpenSwoole\Coroutine\Channel;
use OpenSwoole\Timer;

$base = getcwd();
$dirA = sys_get_temp_dir() . '/zp052_a_' . getmypid();
$dirB = sys_get_temp_dir() . '/zp052_b_' . getmypid();
@mkdir($dirA); @mkdir($dirB);
$realA = realpath($dirA);
$realB = realpath($dirB);

zealphp_cwd_isolation(true);

$wrongA = new OpenSwoole\Atomic(0);
$wrongB = new OpenSwoole\Atomic(0);
$leaked = new OpenSwoole\Atomic(0);

$yield = function (): void {
    $ch = new Channel(1);
    Timer::after(5, fn() => $ch->push(1));
    $ch->pop(2.0);
};

Co::run(function () use ($realA, $realB, $base, $wrongA, $wrongB, $leaked, $yield) {
    $wg = new Channel(3);
    go(function () use ($realA, $wrongA, $yield, $wg) {
        chdir($realA);
        for ($i = 0; $i < 3; $i++) {
            $yield();
            if (getcwd() !== $realA) { $wrongA->add(1); }
        }
        $wg->push(1);
    });
    go(function () use ($realB, $wrongB, $yield, $wg) {
        chdir($realB);
        for ($i = 0; $i < 3; $i++) {
            $yield();
            if (getcwd() !== $realB) { $wrongB->add(1); }
        }
        $wg->push(1);
    });
    go(function () use ($base, $leaked, $yield, $wg) {
        // Never chdir's — must see the baseline on every slice, never a
        // peer's directory.
        for ($i = 0; $i < 4; $i++) {
            if (getcwd() !== $base) { $leaked->add(1); }
            $yield();
        }
        $wg->push(1);
    });
    for ($i = 0; $i < 3; $i++) { $wg->pop(5.0); }
});

echo "A wrong-cwd resumes: ", $wrongA->get(), "\n";
echo "B wrong-cwd resumes: ", $wrongB->get(), "\n";
echo "bystander leaks: ", $leaked->get(), "\n";
echo "process re-parked at baseline: ", (getcwd() === $base ? "yes" : "NO: " . getcwd()), "\n";

zealphp_cwd_isolation(false);
@rmdir($dirA); @rmdir($dirB);
?>
--EXPECT--
A wrong-cwd resumes: 0
B wrong-cwd resumes: 0
bystander leaks: 0
process re-parked at baseline: yes
