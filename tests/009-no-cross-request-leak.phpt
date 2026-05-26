--TEST--
zealphp_superglobals: no cross-request leakage after clear
--EXTENSIONS--
zealphp
--FILE--
<?php
// Simulate request 1
zealphp_superglobals_set(
    ['secret' => 'user1_token'],
    ['password' => 'hunter2'],
    ['PHPSESSID' => 'sess_user1'],
    ['REMOTE_ADDR' => '10.0.0.1'],
    [],
    [],
    []
);
echo "Req1 GET: " . $_GET['secret'] . "\n";
echo "Req1 POST: " . $_POST['password'] . "\n";
echo "Req1 COOKIE: " . $_COOKIE['PHPSESSID'] . "\n";

// End request 1
zealphp_superglobals_clear();

// Simulate request 2 — MUST NOT see request 1's data
zealphp_superglobals_set(
    ['page' => '2'],
    [],
    ['PHPSESSID' => 'sess_user2'],
    ['REMOTE_ADDR' => '10.0.0.2'],
    [],
    [],
    []
);

echo "Req2 sees secret: " . (isset($_GET['secret']) ? "LEAKED!" : "no") . "\n";
echo "Req2 sees password: " . (isset($_POST['password']) ? "LEAKED!" : "no") . "\n";
echo "Req2 cookie: " . $_COOKIE['PHPSESSID'] . "\n";
echo "Req2 addr: " . $_SERVER['REMOTE_ADDR'] . "\n";

zealphp_superglobals_clear();
?>
--EXPECT--
Req1 GET: user1_token
Req1 POST: hunter2
Req1 COOKIE: sess_user1
Req2 sees secret: no
Req2 sees password: no
Req2 cookie: sess_user2
Req2 addr: 10.0.0.2
