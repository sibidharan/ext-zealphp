--TEST--
zealphp_superglobals: save/restore fully isolates concurrent coroutine state
--EXTENSIONS--
zealphp
--FILE--
<?php
// Coroutine A starts
zealphp_superglobals_set(
    ['user_id' => '42', 'role' => 'admin'],
    ['action' => 'delete'],
    ['token' => 'admin_tok'],
    ['REQUEST_METHOD' => 'DELETE'],
    [],
    [],
    []
);
$snapA = zealphp_superglobals_save();

// Coroutine B starts (A yielded on I/O)
zealphp_superglobals_set(
    ['user_id' => '99', 'role' => 'viewer'],
    ['action' => 'read'],
    ['token' => 'viewer_tok'],
    ['REQUEST_METHOD' => 'GET'],
    [],
    [],
    []
);

// B sees its own data
echo "B user: " . $_GET['user_id'] . ", role: " . $_GET['role'] . "\n";
echo "B method: " . $_SERVER['REQUEST_METHOD'] . "\n";
echo "B token: " . $_COOKIE['token'] . "\n";

$snapB = zealphp_superglobals_save();

// Coroutine A resumes
zealphp_superglobals_restore($snapA);
echo "A user: " . $_GET['user_id'] . ", role: " . $_GET['role'] . "\n";
echo "A method: " . $_SERVER['REQUEST_METHOD'] . "\n";
echo "A token: " . $_COOKIE['token'] . "\n";
echo "A action: " . $_POST['action'] . "\n";

// B resumes again
zealphp_superglobals_restore($snapB);
echo "B still: " . $_GET['user_id'] . "\n";

zealphp_superglobals_clear();
?>
--EXPECT--
B user: 99, role: viewer
B method: GET
B token: viewer_tok
A user: 42, role: admin
A method: DELETE
A token: admin_tok
A action: delete
B still: 99
