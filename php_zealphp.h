#ifndef PHP_ZEALPHP_H
#define PHP_ZEALPHP_H

extern zend_module_entry zealphp_module_entry;
#define phpext_zealphp_ptr &zealphp_module_entry

#define PHP_ZEALPHP_VERSION "0.3.10"

PHP_MINIT_FUNCTION(zealphp);
PHP_MSHUTDOWN_FUNCTION(zealphp);
PHP_RINIT_FUNCTION(zealphp);
PHP_RSHUTDOWN_FUNCTION(zealphp);
PHP_MINFO_FUNCTION(zealphp);

PHP_FUNCTION(zealphp_override);
PHP_FUNCTION(zealphp_restore);
PHP_FUNCTION(zealphp_restore_all);
PHP_FUNCTION(zealphp_superglobals_set);
PHP_FUNCTION(zealphp_superglobals_clear);
PHP_FUNCTION(zealphp_superglobals_save);
PHP_FUNCTION(zealphp_superglobals_restore);
PHP_FUNCTION(zealphp_coroutine_superglobals);
PHP_FUNCTION(zealphp_coroutine_globals);
PHP_FUNCTION(zealphp_constants_clear);
PHP_FUNCTION(zealphp_ini_restore);
PHP_FUNCTION(zealphp_define_hook);
PHP_FUNCTION(zealphp_globals_snapshot);
PHP_FUNCTION(zealphp_globals_clean);
PHP_FUNCTION(zealphp_process_state_snapshot);
PHP_FUNCTION(zealphp_process_state_clean);
PHP_FUNCTION(zealphp_protect_classes);
PHP_FUNCTION(zealphp_silent_redeclare);
PHP_FUNCTION(zealphp_include_isolation);
PHP_FUNCTION(zealphp_coroutine_statics);

#endif
