--TEST--
PARANOID: $_SESSION data doesn't leak between coroutines after yield
--EXTENSIONS--
zealphp
--FILE--
<?php
// Simulate: A sets sensitive session, yields, B runs, A resumes
// A must NOT see B's data, B must NOT see A's data

// Request A: admin user
zealphp_superglobals_set(
    [], [],
    ['PHPSESSID' => 'admin_sess'],
    [], [], [],
    []
);
$_SESSION = ['user_id' => 1, 'role' => 'admin', 'secret_token' => 'sk_admin_xyz'];
$snapA = zealphp_superglobals_save();

// Request B: anonymous user (A yielded)
zealphp_superglobals_set(
    [], [],
    ['PHPSESSID' => 'anon_sess'],
    [], [], [],
    []
);
$_SESSION = ['user_id' => null, 'role' => 'guest'];

// B must NOT see A's secret
echo "B sees admin secret: " . (isset($_SESSION['secret_token']) ? "LEAKED!" : "no") . "\n";
echo "B role: " . $_SESSION['role'] . "\n";

$snapB = zealphp_superglobals_save();

// A resumes
zealphp_superglobals_restore($snapA);
echo "A role: " . $_SESSION['role'] . "\n";
echo "A secret: " . $_SESSION['secret_token'] . "\n";
echo "A sees guest: " . ($_SESSION['role'] === 'guest' ? "LEAKED!" : "no") . "\n";

// B resumes
zealphp_superglobals_restore($snapB);
echo "B still guest: " . $_SESSION['role'] . "\n";
echo "B sees admin secret: " . (isset($_SESSION['secret_token']) ? "LEAKED!" : "no") . "\n";

zealphp_superglobals_clear();
echo "PASS\n";
?>
--EXPECT--
B sees admin secret: no
B role: guest
A role: admin
A secret: sk_admin_xyz
A sees guest: no
B still guest: guest
B sees admin secret: no
PASS
