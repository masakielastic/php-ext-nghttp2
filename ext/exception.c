#include "php_nghttp2.h"
#include <Zend/zend_exceptions.h>

zend_class_entry *nghttp2_ce_hpack_exception;

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_nghttp2_hpack_exception_get_nghttp2_error_code, 0, 0, IS_LONG, 0)
ZEND_END_ARG_INFO()

ZEND_METHOD(Nghttp2_HpackException, getNghttp2ErrorCode)
{
    zval *code;

    ZEND_PARSE_PARAMETERS_NONE();

    code = zend_read_property(nghttp2_ce_hpack_exception, Z_OBJ_P(ZEND_THIS), "nghttp2ErrorCode", sizeof("nghttp2ErrorCode") - 1, 1, NULL);
    RETURN_LONG(zval_get_long(code));
}

static const zend_function_entry nghttp2_hpack_exception_methods[] = {
    ZEND_ME(Nghttp2_HpackException, getNghttp2ErrorCode, arginfo_nghttp2_hpack_exception_get_nghttp2_error_code, ZEND_ACC_PUBLIC)
    ZEND_FE_END
};

void nghttp2_register_exception_class(void)
{
    zend_class_entry ce;

    INIT_NS_CLASS_ENTRY(ce, "Nghttp2\\Exception", "HpackException", nghttp2_hpack_exception_methods);
    nghttp2_ce_hpack_exception = zend_register_internal_class_ex(&ce, zend_ce_exception);
    zend_declare_property_long(nghttp2_ce_hpack_exception, "nghttp2ErrorCode", sizeof("nghttp2ErrorCode") - 1, 0, ZEND_ACC_PROTECTED);
}

void nghttp2_throw_hpack_exception(const char *message, int error_code)
{
    zval zv;

    zend_throw_exception(nghttp2_ce_hpack_exception, message, error_code);

    if (EG(exception) != NULL) {
        ZVAL_LONG(&zv, error_code);
#if PHP_VERSION_ID >= 80500
        zend_update_property(nghttp2_ce_hpack_exception, EG(exception), "nghttp2ErrorCode", sizeof("nghttp2ErrorCode") - 1, &zv);
#else
        zend_update_property(nghttp2_ce_hpack_exception, Z_OBJ(EG(exception)), "nghttp2ErrorCode", sizeof("nghttp2ErrorCode") - 1, &zv);
#endif
    }
}
