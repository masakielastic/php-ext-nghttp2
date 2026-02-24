#include "php_nghttp2.h"

#include <sys/types.h>

typedef struct _nghttp2_hpack_object {
    nghttp2_hd_deflater *deflater;
    nghttp2_hd_inflater *inflater;
    size_t max_dynamic_table_size;
    zend_object std;
} nghttp2_hpack_object;

zend_class_entry *nghttp2_ce_hpack;
static zend_object_handlers nghttp2_hpack_object_handlers;

static inline nghttp2_hpack_object *nghttp2_hpack_from_obj(zend_object *obj)
{
    return (nghttp2_hpack_object *)((char *)(obj) - XtOffsetOf(nghttp2_hpack_object, std));
}

#define Z_NGHTTP2_HPACK_OBJ_P(zv) nghttp2_hpack_from_obj(Z_OBJ_P((zv)))

static zend_object *nghttp2_hpack_create_object(zend_class_entry *ce)
{
    nghttp2_hpack_object *intern = zend_object_alloc(sizeof(nghttp2_hpack_object), ce);

    intern->deflater = NULL;
    intern->inflater = NULL;
    intern->max_dynamic_table_size = 4096;

    zend_object_std_init(&intern->std, ce);
    object_properties_init(&intern->std, ce);
    intern->std.handlers = &nghttp2_hpack_object_handlers;

    return &intern->std;
}

static void nghttp2_hpack_free_object(zend_object *object)
{
    nghttp2_hpack_object *intern = nghttp2_hpack_from_obj(object);

    if (intern->deflater != NULL) {
        nghttp2_hd_deflate_del(intern->deflater);
        intern->deflater = NULL;
    }
    if (intern->inflater != NULL) {
        nghttp2_hd_inflate_del(intern->inflater);
        intern->inflater = NULL;
    }

    zend_object_std_dtor(&intern->std);
}

ZEND_BEGIN_ARG_INFO_EX(arginfo_hpack_construct, 0, 0, 0)
    ZEND_ARG_TYPE_INFO(0, maxDynamicTableSize, IS_LONG, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_hpack_set_size, 0, 0, 1)
    ZEND_ARG_TYPE_INFO(0, size, IS_LONG, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_hpack_encode, 0, 0, 1)
    ZEND_ARG_ARRAY_INFO(0, headers, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_hpack_decode, 0, 0, 1)
    ZEND_ARG_TYPE_INFO(0, headerBlock, IS_STRING, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_hpack_get_long, 0, 0, IS_LONG, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_hpack_void, 0, 0, IS_VOID, 0)
ZEND_END_ARG_INFO()

static int nghttp2_hpack_object_reinit(nghttp2_hpack_object *intern)
{
    if (intern->deflater != NULL) {
        nghttp2_hd_deflate_del(intern->deflater);
        intern->deflater = NULL;
    }
    if (intern->inflater != NULL) {
        nghttp2_hd_inflate_del(intern->inflater);
        intern->inflater = NULL;
    }

    if (nghttp2_hd_deflate_new(&intern->deflater, intern->max_dynamic_table_size) != 0) {
        return FAILURE;
    }

    if (nghttp2_hd_inflate_new(&intern->inflater) != 0) {
        nghttp2_hd_deflate_del(intern->deflater);
        intern->deflater = NULL;
        return FAILURE;
    }

    return SUCCESS;
}

ZEND_METHOD(Nghttp2_Hpack, __construct)
{
    zend_long max_dynamic_table_size = 4096;
    nghttp2_hpack_object *intern = Z_NGHTTP2_HPACK_OBJ_P(ZEND_THIS);

    ZEND_PARSE_PARAMETERS_START(0, 1)
        Z_PARAM_OPTIONAL
        Z_PARAM_LONG(max_dynamic_table_size)
    ZEND_PARSE_PARAMETERS_END();

    if (max_dynamic_table_size < 0) {
        zend_argument_value_error(1, "must be greater than or equal to 0");
        RETURN_THROWS();
    }

    intern->max_dynamic_table_size = (size_t)max_dynamic_table_size;
    if (nghttp2_hpack_object_reinit(intern) != SUCCESS) {
        nghttp2_throw_hpack_exception("failed to initialize HPACK context", NGHTTP2_ERR_NOMEM);
        RETURN_THROWS();
    }
}

ZEND_METHOD(Nghttp2_Hpack, setMaxDynamicTableSize)
{
    zend_long size;
    nghttp2_hpack_object *intern = Z_NGHTTP2_HPACK_OBJ_P(ZEND_THIS);

    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_LONG(size)
    ZEND_PARSE_PARAMETERS_END();

    if (size < 0) {
        zend_argument_value_error(1, "must be greater than or equal to 0");
        RETURN_THROWS();
    }

    intern->max_dynamic_table_size = (size_t)size;
    if (nghttp2_hpack_object_reinit(intern) != SUCCESS) {
        nghttp2_throw_hpack_exception("failed to apply dynamic table size", NGHTTP2_ERR_NOMEM);
        RETURN_THROWS();
    }
}

ZEND_METHOD(Nghttp2_Hpack, getMaxDynamicTableSize)
{
    nghttp2_hpack_object *intern = Z_NGHTTP2_HPACK_OBJ_P(ZEND_THIS);

    ZEND_PARSE_PARAMETERS_NONE();
    RETURN_LONG((zend_long)intern->max_dynamic_table_size);
}

ZEND_METHOD(Nghttp2_Hpack, getDynamicTableSize)
{
    ZEND_PARSE_PARAMETERS_NONE();
    RETURN_LONG(0);
}

ZEND_METHOD(Nghttp2_Hpack, clearDynamicTable)
{
    nghttp2_hpack_object *intern = Z_NGHTTP2_HPACK_OBJ_P(ZEND_THIS);

    ZEND_PARSE_PARAMETERS_NONE();

    if (nghttp2_hpack_object_reinit(intern) != SUCCESS) {
        nghttp2_throw_hpack_exception("failed to clear dynamic table", NGHTTP2_ERR_NOMEM);
        RETURN_THROWS();
    }
}

ZEND_METHOD(Nghttp2_Hpack, encode)
{
    zval *headers;
    HashTable *header_ht;
    uint32_t nvlen;
    nghttp2_nv *nva;
    uint8_t *out;
    size_t bound;
    ssize_t olen;
    uint32_t i = 0;
    zval *entry;
    nghttp2_hpack_object *intern = Z_NGHTTP2_HPACK_OBJ_P(ZEND_THIS);

    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_ARRAY(headers)
    ZEND_PARSE_PARAMETERS_END();

    header_ht = Z_ARRVAL_P(headers);
    nvlen = zend_hash_num_elements(header_ht);
    if (nvlen == 0) {
        RETURN_EMPTY_STRING();
    }

    nva = ecalloc(nvlen, sizeof(nghttp2_nv));

    ZEND_HASH_FOREACH_VAL(header_ht, entry) {
        zval *name;
        zval *value;

        if (Z_TYPE_P(entry) != IS_ARRAY) {
            efree(nva);
            zend_type_error("each header must be an array with keys 'name' and 'value'");
            RETURN_THROWS();
        }

        name = zend_hash_str_find(Z_ARRVAL_P(entry), "name", sizeof("name") - 1);
        value = zend_hash_str_find(Z_ARRVAL_P(entry), "value", sizeof("value") - 1);
        if (name == NULL || value == NULL) {
            efree(nva);
            zend_type_error("each header must contain both 'name' and 'value'");
            RETURN_THROWS();
        }
        if (Z_TYPE_P(name) != IS_STRING || Z_TYPE_P(value) != IS_STRING) {
            efree(nva);
            zend_type_error("header 'name' and 'value' must be strings");
            RETURN_THROWS();
        }

        nva[i].name = (uint8_t *)Z_STRVAL_P(name);
        nva[i].value = (uint8_t *)Z_STRVAL_P(value);
        nva[i].namelen = Z_STRLEN_P(name);
        nva[i].valuelen = Z_STRLEN_P(value);
        nva[i].flags = NGHTTP2_NV_FLAG_NONE;

        i++;
    } ZEND_HASH_FOREACH_END();

    bound = nghttp2_hd_deflate_bound(intern->deflater, nva, nvlen);
    out = emalloc(bound);
    olen = nghttp2_hd_deflate_hd(intern->deflater, out, bound, nva, nvlen);

    efree(nva);

    if (olen < 0) {
        efree(out);
        nghttp2_throw_hpack_exception(nghttp2_strerror((int)olen), (int)olen);
        RETURN_THROWS();
    }

    RETVAL_STRINGL((char *)out, (size_t)olen);
    efree(out);
}

ZEND_METHOD(Nghttp2_Hpack, decode)
{
    zend_string *header_block;
    const uint8_t *in;
    size_t inlen;
    nghttp2_hpack_object *intern = Z_NGHTTP2_HPACK_OBJ_P(ZEND_THIS);

    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_STR(header_block)
    ZEND_PARSE_PARAMETERS_END();

    array_init(return_value);

    in = (const uint8_t *)ZSTR_VAL(header_block);
    inlen = ZSTR_LEN(header_block);

    while (1) {
        nghttp2_nv nv;
        int inflate_flags = 0;
        ssize_t rv;

        rv = nghttp2_hd_inflate_hd2(intern->inflater, &nv, &inflate_flags, in, inlen, 1);
        if (rv < 0) {
            zval_ptr_dtor(return_value);
            nghttp2_throw_hpack_exception(nghttp2_strerror((int)rv), (int)rv);
            RETURN_THROWS();
        }

        in += (size_t)rv;
        inlen -= (size_t)rv;

        if (inflate_flags & NGHTTP2_HD_INFLATE_EMIT) {
            zval header;
            zval name;
            zval value;

            array_init(&header);
            ZVAL_STRINGL(&name, (const char *)nv.name, nv.namelen);
            ZVAL_STRINGL(&value, (const char *)nv.value, nv.valuelen);
            zend_hash_str_update(Z_ARRVAL(header), "name", sizeof("name") - 1, &name);
            zend_hash_str_update(Z_ARRVAL(header), "value", sizeof("value") - 1, &value);
            add_next_index_zval(return_value, &header);
        }

        if (inflate_flags & NGHTTP2_HD_INFLATE_FINAL) {
            nghttp2_hd_inflate_end_headers(intern->inflater);
            break;
        }

        if ((inflate_flags & NGHTTP2_HD_INFLATE_EMIT) == 0 && inlen == 0) {
            break;
        }
    }
}

static const zend_function_entry nghttp2_hpack_methods[] = {
    ZEND_ME(Nghttp2_Hpack, __construct, arginfo_hpack_construct, ZEND_ACC_PUBLIC)
    ZEND_ME(Nghttp2_Hpack, setMaxDynamicTableSize, arginfo_hpack_set_size, ZEND_ACC_PUBLIC)
    ZEND_ME(Nghttp2_Hpack, getMaxDynamicTableSize, arginfo_hpack_get_long, ZEND_ACC_PUBLIC)
    ZEND_ME(Nghttp2_Hpack, getDynamicTableSize, arginfo_hpack_get_long, ZEND_ACC_PUBLIC)
    ZEND_ME(Nghttp2_Hpack, clearDynamicTable, arginfo_hpack_void, ZEND_ACC_PUBLIC)
    ZEND_ME(Nghttp2_Hpack, encode, arginfo_hpack_encode, ZEND_ACC_PUBLIC)
    ZEND_ME(Nghttp2_Hpack, decode, arginfo_hpack_decode, ZEND_ACC_PUBLIC)
    ZEND_FE_END
};

void nghttp2_register_hpack_class(void)
{
    zend_class_entry ce;

    INIT_NS_CLASS_ENTRY(ce, "Nghttp2", "Hpack", nghttp2_hpack_methods);
    nghttp2_ce_hpack = zend_register_internal_class(&ce);
    nghttp2_ce_hpack->create_object = nghttp2_hpack_create_object;
    nghttp2_ce_hpack->ce_flags |= ZEND_ACC_FINAL;

    memcpy(&nghttp2_hpack_object_handlers, &std_object_handlers, sizeof(zend_object_handlers));
    nghttp2_hpack_object_handlers.offset = XtOffsetOf(nghttp2_hpack_object, std);
    nghttp2_hpack_object_handlers.free_obj = nghttp2_hpack_free_object;
}
