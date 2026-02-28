#include "php_nghttp2.h"
#include <ext/standard/info.h>

static int nghttp2_headers_append_pair(zval *normalized, zend_string *name, zend_string *value)
{
    zval pair;
    zval name_zv;
    zval value_zv;

    array_init(&pair);
    ZVAL_STR_COPY(&name_zv, name);
    ZVAL_STR_COPY(&value_zv, value);
    zend_hash_str_update(Z_ARRVAL(pair), "name", sizeof("name") - 1, &name_zv);
    zend_hash_str_update(Z_ARRVAL(pair), "value", sizeof("value") - 1, &value_zv);
    add_next_index_zval(normalized, &pair);

    return SUCCESS;
}

zend_bool nghttp2_headers_is_reserved_response_name(const zend_string *name)
{
    if (zend_string_equals_literal_ci(name, ":status")) {
        return 1;
    }
    if (zend_string_equals_literal_ci(name, "content-length")) {
        return 1;
    }

    return 0;
}

int nghttp2_headers_normalize(zval *headers, zval *normalized, uint32_t flags)
{
    HashTable *ht = Z_ARRVAL_P(headers);
    zval *entry;
    zend_string *key;
    zend_ulong index;

    array_init(normalized);

    ZEND_HASH_FOREACH_KEY_VAL(ht, index, key, entry) {
        zend_string *name;
        zend_string *value;
        zval *name_zv;
        zval *value_zv;

        if (key != NULL) {
            if ((flags & NGHTTP2_HEADERS_NORMALIZE_ALLOW_ASSOC) == 0) {
                zval_ptr_dtor(normalized);
                zend_type_error("each header must be an array with keys 'name' and 'value'");
                return FAILURE;
            }
            if (Z_TYPE_P(entry) != IS_STRING) {
                zval_ptr_dtor(normalized);
                zend_type_error("when using associative headers, each value must be string");
                return FAILURE;
            }

            name = key;
            value = Z_STR_P(entry);
        } else {
            if (Z_TYPE_P(entry) != IS_ARRAY) {
                zval_ptr_dtor(normalized);
                zend_type_error("each header must be string value or array{name, value}");
                return FAILURE;
            }

            name_zv = zend_hash_str_find(Z_ARRVAL_P(entry), "name", sizeof("name") - 1);
            value_zv = zend_hash_str_find(Z_ARRVAL_P(entry), "value", sizeof("value") - 1);
            if (name_zv == NULL || value_zv == NULL) {
                zval_ptr_dtor(normalized);
                zend_type_error("each header array must contain string 'name' and 'value'");
                return FAILURE;
            }
            if (Z_TYPE_P(name_zv) != IS_STRING || Z_TYPE_P(value_zv) != IS_STRING) {
                zval_ptr_dtor(normalized);
                zend_type_error("header 'name' and 'value' must be strings");
                return FAILURE;
            }

            name = Z_STR_P(name_zv);
            value = Z_STR_P(value_zv);
        }

        if ((flags & NGHTTP2_HEADERS_NORMALIZE_FILTER_RESPONSE_RESERVED) != 0 &&
            nghttp2_headers_is_reserved_response_name(name)) {
            continue;
        }

        if (nghttp2_headers_append_pair(normalized, name, value) != SUCCESS) {
            zval_ptr_dtor(normalized);
            return FAILURE;
        }
    } ZEND_HASH_FOREACH_END();

    return SUCCESS;
}

int nghttp2_headers_build_nv_array(zval *headers, nghttp2_nv **nva_out, size_t *nvlen_out)
{
    HashTable *header_ht;
    nghttp2_nv *nva;
    zval *entry;
    size_t nvlen;
    size_t i = 0;

    header_ht = Z_ARRVAL_P(headers);
    nvlen = zend_hash_num_elements(header_ht);
    *nvlen_out = nvlen;
    if (nvlen == 0) {
        *nva_out = NULL;
        return SUCCESS;
    }

    nva = ecalloc(nvlen, sizeof(*nva));

    ZEND_HASH_FOREACH_VAL(header_ht, entry) {
        zval *name;
        zval *value;

        if (Z_TYPE_P(entry) != IS_ARRAY) {
            efree(nva);
            zend_type_error("each header must be an array with keys 'name' and 'value'");
            return FAILURE;
        }

        name = zend_hash_str_find(Z_ARRVAL_P(entry), "name", sizeof("name") - 1);
        value = zend_hash_str_find(Z_ARRVAL_P(entry), "value", sizeof("value") - 1);
        if (name == NULL || value == NULL) {
            efree(nva);
            zend_type_error("each header must contain both 'name' and 'value'");
            return FAILURE;
        }
        if (Z_TYPE_P(name) != IS_STRING || Z_TYPE_P(value) != IS_STRING) {
            efree(nva);
            zend_type_error("header 'name' and 'value' must be strings");
            return FAILURE;
        }

        nva[i].name = (uint8_t *)Z_STRVAL_P(name);
        nva[i].value = (uint8_t *)Z_STRVAL_P(value);
        nva[i].namelen = Z_STRLEN_P(name);
        nva[i].valuelen = Z_STRLEN_P(value);
        nva[i].flags = NGHTTP2_NV_FLAG_NONE;
        i++;
    } ZEND_HASH_FOREACH_END();

    *nva_out = nva;
    return SUCCESS;
}

PHP_MINIT_FUNCTION(nghttp2)
{
    nghttp2_register_exception_class();
    nghttp2_register_hpack_class();
    nghttp2_register_client_class();
    nghttp2_register_server_class();
    nghttp2_register_session_class();
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
