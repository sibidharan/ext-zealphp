<?php
// ext#59 define-path probe — verbatim from the issue
$me = $_GET['who'] ?? 'anon';
if (!defined('REQ_CONST')) define('REQ_CONST', $me);
usleep(3000);                       // any HOOK_ALL-coroutinized I/O yield
echo json_encode(['me' => $me, 'const' => defined('REQ_CONST') ? constant('REQ_CONST') : 'UNDEF']);
