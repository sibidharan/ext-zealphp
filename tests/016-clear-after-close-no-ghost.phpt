--TEST--
PARANOID: after coroutine close + clear, no ghost data from ANY previous coroutine
--EXTENSIONS--
zealphp
--FILE--
<?php
// Simulate: coro A writes sensitive data, ends. New coro C starts.
// C must see NOTHING from A.

zealphp_superglobals_set(
    ['api_key' => 'sk_live_secret123'],
    ['password' => 'hunter2'],
    ['auth_token' => 'bearer_xyz'],
    ['HTTP_AUTHORIZATION' => 'Bearer sk_live_secret123'],
    ['upload' => ['name' => 'secret.pdf']],
    ['api_key' => 'sk_live_secret123']
);
$_SESSION = ['user_id' => 42, 'is_admin' => true];

// A ends — clear everything
zealphp_superglobals_clear();

// C starts fresh — must see NOTHING
zealphp_superglobals_set([], [], [], [], [], [],
    []);
$_SESSION = [];

$ghosts = [];
if (!empty($_GET)) $ghosts[] = '_GET';
if (!empty($_POST)) $ghosts[] = '_POST';
if (!empty($_COOKIE)) $ghosts[] = '_COOKIE';
if (!empty($_SERVER)) $ghosts[] = '_SERVER';
if (!empty($_FILES)) $ghosts[] = '_FILES';
if (!empty($_REQUEST)) $ghosts[] = '_REQUEST';
if (!empty($_SESSION)) $ghosts[] = '_SESSION';

if (empty($ghosts)) {
    echo "No ghost data from previous coroutine\n";
} else {
    echo "GHOST DATA in: " . implode(', ', $ghosts) . "\n";
}

zealphp_superglobals_clear();
echo "PASS\n";
?>
--EXPECT--
No ghost data from previous coroutine
PASS
