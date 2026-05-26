--TEST--
PARANOID: rapid coroutine switching — 100 switches, no data corruption
--EXTENSIONS--
zealphp
--FILE--
<?php
// Simulate 10 coroutines with 10 round-robin switches each
$snapshots = [];
$N = 10;

// Initialize each coroutine's session
for ($i = 0; $i < $N; $i++) {
    zealphp_superglobals_set(
        ['coro_id' => (string)$i],
        ['data' => "payload_$i"],
        [],
        ['REMOTE_ADDR' => "10.0.0.$i"],
        [],
        [],
    []
    );
    $_SESSION = ['owner' => $i, 'counter' => 0];
    $snapshots[$i] = zealphp_superglobals_save();
}

// Rapid round-robin switching — each coro increments its counter
for ($round = 0; $round < 10; $round++) {
    for ($i = 0; $i < $N; $i++) {
        zealphp_superglobals_restore($snapshots[$i]);

        // Verify identity
        if ($_GET['coro_id'] !== (string)$i) {
            echo "CORRUPTION: coro $i sees GET coro_id=" . $_GET['coro_id'] . "\n";
            exit(1);
        }
        if ($_SESSION['owner'] !== $i) {
            echo "CORRUPTION: coro $i sees session owner=" . $_SESSION['owner'] . "\n";
            exit(1);
        }
        if ($_SERVER['REMOTE_ADDR'] !== "10.0.0.$i") {
            echo "CORRUPTION: coro $i sees addr=" . $_SERVER['REMOTE_ADDR'] . "\n";
            exit(1);
        }

        $_SESSION['counter']++;
        $snapshots[$i] = zealphp_superglobals_save();
    }
}

// Verify final state — each counter should be 10
for ($i = 0; $i < $N; $i++) {
    zealphp_superglobals_restore($snapshots[$i]);
    if ($_SESSION['counter'] !== 10) {
        echo "CORRUPTION: coro $i counter=" . $_SESSION['counter'] . " (expected 10)\n";
        exit(1);
    }
}

zealphp_superglobals_clear();
echo "100 switches, 10 coroutines, 0 corruption\n";
?>
--EXPECT--
100 switches, 10 coroutines, 0 corruption
