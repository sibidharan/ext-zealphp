#!/bin/bash
# Minimal repro for the Stage-8 object-global store-corruption (FIXED in 0.3.47).
# Pre-fix: 3-8 worker SIGSEGVs per 6-way x20 burst (zend_objects_store_put free-list poison).
# Post-fix: 0 worker deaths. Needs: ZealPHP + ext-zealphp + openswoole + a MySQL at 127.0.0.1
# (user zext/zextpass, db zext_repro with `t(v)` seeded). Boots a ~15-line YOURLS-class app
# (file-scope `$pdo = new PDO(mysql)` under App::globalScopeInclude(true)) and bursts it.
set -e
PORT="${1:-9760}"; D=/repros/burst48
mysql -e "CREATE DATABASE IF NOT EXISTS zext_repro CHARACTER SET utf8mb4;
          CREATE TABLE IF NOT EXISTS zext_repro.t (id INT PRIMARY KEY AUTO_INCREMENT, v VARCHAR(64));
          INSERT IGNORE INTO zext_repro.t (id,v) VALUES (1,'seed');" 2>/dev/null || true
mkdir -p "$D/public"
cat > "$D/config.php" <<'PHP'
<?php
define('YS_SITE', 'http://127.0.0.1');
define('YS_DB_HOST', '127.0.0.1'); define('YS_DB_NAME', 'zext_repro');
define('YS_DB_USER', 'zext'); define('YS_DB_PASS', 'zextpass');
PHP
cat > "$D/public/index.php" <<'PHP'
<?php
require_once __DIR__ . '/../config.php';
$pdo = new PDO('mysql:host='.YS_DB_HOST.';dbname='.YS_DB_NAME, YS_DB_USER, YS_DB_PASS,
               [PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION]);
$row = $pdo->query('SELECT v FROM t LIMIT 1')->fetch(PDO::FETCH_ASSOC);
echo 'OK site=', YS_SITE, ' db=', $row['v'] ?? 'none';
PHP
cat > "$D/app.php" <<PHP
<?php
require "/zeal/vendor/autoload.php";
use ZealPHP\App;
App::mode(App::MODE_COROUTINE_LEGACY);
App::globalScopeInclude(true);
\$app = App::init("127.0.0.1", $PORT);
App::\$cwd = '$D';
App::documentRoot('$D/public');
\$app->setFallback(function () {
    \$g = \ZealPHP\G::instance();
    \$g->server['PHP_SELF']='/index.php'; \$g->server['SCRIPT_NAME']='/index.php';
    \$g->server['SCRIPT_FILENAME']='$D/public/index.php';
    return App::include('/index.php');
});
\$app->run(['worker_num' => 2, 'task_worker_num' => 0]);
PHP
cd "$D"; (php app.php > boot.log 2>&1 &); sleep 6
echo "single: $(curl -s --max-time 8 -w '[%{http_code}]' http://127.0.0.1:$PORT/)"
for r in $(seq 1 8); do
  echo -n "round $r: "
  seq 1 20 | xargs -P6 -I{} curl -s --max-time 10 -o /dev/null -w "%{http_code} " http://127.0.0.1:$PORT/ | tr ' ' '\n' | sort | uniq -c | tr '\n' ' '; echo
done
echo "worker deaths (expect 0 on >=0.3.47): $(grep -cE 'abnormal exit' boot.log)"
