# ext#59 concurrent constant-leak burst harness

Canonical validation for the per-request `define()` constant isolation fix
(phpt can't exercise the OpenSwoole server's `on_resume` cid-unreliability).

```bash
# ext-zealphp 0.3.60+ and openswoole loaded; a ZealPHP checkout for the autoload
ZEALPHP_AUTOLOAD=/path/to/zealphp/vendor/autoload.php PORT=8133 W=4 GSI=1 DI=1 \
  php -d extension=modules/zealphp.so app.php &

python3 burst.py 8133 kconst.php 40 3   # defineIsolation ON  -> OK=120 LEAK=0
python3 burst.py 8133 noconst.php 40 3   # control            -> OK=120 LEAK=0
```

Env: `W` workers, `GSI=1` globalScopeInclude, `DI=1` defineIsolation, `PORT`.
`kconst.php` is the define-path probe; `noconst.php` the putenv/ini/$_SESSION control.
