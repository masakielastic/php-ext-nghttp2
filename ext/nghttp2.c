#include "php_nghttp2.h"
#include <ext/standard/info.h>

PHP_MINIT_FUNCTION(nghttp2)
{
    nghttp2_register_exception_class();
    nghttp2_register_hpack_class();
    nghttp2_register_client_class();
    nghttp2_register_server_class();
    return SUCCESS;
}

PHP_MINFO_FUNCTION(nghttp2)
{
    php_info_print_table_start();
    php_info_print_table_header(2, "nghttp2 support", "enabled");
    php_info_print_table_end();
}

zend_module_entry nghttp2_module_entry = {
    STANDARD_MODULE_HEADER,
    "nghttp2",
    NULL,
    PHP_MINIT(nghttp2),
    NULL,
    NULL,
    NULL,
    PHP_MINFO(nghttp2),
    "0.1.0",
    STANDARD_MODULE_PROPERTIES
};

#if defined(COMPILE_DL_NGHTTP2) || defined(COMPILE_DL_EXT) || defined(ZEND_COMPILE_DL_NGHTTP2) || defined(ZEND_COMPILE_DL_EXT)
# ifdef ZTS
ZEND_TSRMLS_CACHE_DEFINE()
# endif
ZEND_GET_MODULE(nghttp2)
#endif
