<?php
// ext#59 repro boot — mirrors the issue's config:
// coroutine-legacy + superglobals(true) + globalScopeInclude(GSI=1) + ignorePhpExt(false)
require getenv('ZEALPHP_AUTOLOAD') ?: '/root/zealphp/vendor/autoload.php';

use ZealPHP\App;

App::mode('coroutine-legacy');
if (getenv('GSI')) {
    App::globalScopeInclude(true);
}
if (getenv('DI')) {
    App::defineIsolation(true);   // S10 opt-in matrix cell
}
App::ignorePhpExt(false);

$app = App::init('0.0.0.0', (int) (getenv('PORT') ?: 8133), __DIR__);
$app->run([
    'worker_num' => (int) (getenv('W') ?: 4),
    'task_worker_num' => 0,
]);
