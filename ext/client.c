#include "php_nghttp2.h"

#include <Zend/zend_smart_str.h>

#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>

#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>

#include <errno.h>
#include <string.h>

typedef struct _nghttp2_client_object {
    int fd;
    SSL_CTX *ssl_ctx;
    SSL *ssl;
    nghttp2_session *session;

    zend_string *host;
    zend_long port;

    int32_t stream_id;
    zend_bool stream_closed;
    zend_bool request_in_progress;
    zend_bool has_status;
    zend_long status;

    zval response_headers;
    smart_str response_body;

    zend_object std;
} nghttp2_client_object;

zend_class_entry *nghttp2_ce_client;
static zend_object_handlers nghttp2_client_object_handlers;

static inline nghttp2_client_object *nghttp2_client_from_obj(zend_object *obj)
{
    return (nghttp2_client_object *)((char *)(obj) - XtOffsetOf(nghttp2_client_object, std));
}

#define Z_NGHTTP2_CLIENT_OBJ_P(zv) nghttp2_client_from_obj(Z_OBJ_P((zv)))

static void nghttp2_client_close_connection(nghttp2_client_object *intern)
{
    if (intern->session != NULL) {
        nghttp2_session_del(intern->session);
        intern->session = NULL;
    }
    if (intern->ssl != NULL) {
        SSL_shutdown(intern->ssl);
        SSL_free(intern->ssl);
        intern->ssl = NULL;
    }
    if (intern->ssl_ctx != NULL) {
        SSL_CTX_free(intern->ssl_ctx);
        intern->ssl_ctx = NULL;
    }
    if (intern->fd >= 0) {
        close(intern->fd);
        intern->fd = -1;
    }
}

static void nghttp2_client_reset_response(nghttp2_client_object *intern)
{
    if (Z_TYPE(intern->response_headers) != IS_UNDEF) {
        zval_ptr_dtor(&intern->response_headers);
    }
    array_init(&intern->response_headers);
    smart_str_free(&intern->response_body);
    intern->has_status = 0;
    intern->status = 0;
}

static int nghttp2_client_tcp_connect(const char *host, const char *port)
{
    struct addrinfo hints;
    struct addrinfo *res = NULL;
    struct addrinfo *rp;
    int fd = -1;
    int gai;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    gai = getaddrinfo(host, port, &hints, &res);
    if (gai != 0) {
        return -1;
    }

    for (rp = res; rp != NULL; rp = rp->ai_next) {
        fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (fd < 0) {
            continue;
        }
        if (connect(fd, rp->ai_addr, rp->ai_addrlen) == 0) {
            break;
        }
        close(fd);
        fd = -1;
    }

    freeaddrinfo(res);
    return fd;
}

static SSL_CTX *nghttp2_client_sslctx_create(void)
{
    static const unsigned char alpn_protos[] = {2, 'h', '2'};
    SSL_CTX *ctx = SSL_CTX_new(TLS_client_method());

    if (ctx == NULL) {
        return NULL;
    }

    SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, NULL);
    if (SSL_CTX_set_default_verify_paths(ctx) != 1) {
        SSL_CTX_free(ctx);
        return NULL;
    }

    if (SSL_CTX_set_alpn_protos(ctx, alpn_protos, sizeof(alpn_protos)) != 0) {
        SSL_CTX_free(ctx);
        return NULL;
    }

    return ctx;
}

static int nghttp2_client_ssl_handshake(nghttp2_client_object *intern)
{
    X509 *cert;
    long vr;
    const unsigned char *alpn = NULL;
    unsigned int alpn_len = 0;

    intern->ssl = SSL_new(intern->ssl_ctx);
    if (intern->ssl == NULL) {
        return FAILURE;
    }

    SSL_set_tlsext_host_name(intern->ssl, ZSTR_VAL(intern->host));
    SSL_set_fd(intern->ssl, intern->fd);

    if (SSL_connect(intern->ssl) != 1) {
        return FAILURE;
    }

    SSL_get0_alpn_selected(intern->ssl, &alpn, &alpn_len);
    if (!(alpn_len == 2 && memcmp(alpn, "h2", 2) == 0)) {
        return FAILURE;
    }

    cert = SSL_get_peer_certificate(intern->ssl);
    if (cert == NULL) {
        return FAILURE;
    }

    vr = SSL_get_verify_result(intern->ssl);
    if (vr != X509_V_OK) {
        X509_free(cert);
        return FAILURE;
    }

#if OPENSSL_VERSION_NUMBER >= 0x10002000L
    if (X509_check_host(cert, ZSTR_VAL(intern->host), ZSTR_LEN(intern->host), 0, NULL) != 1) {
        X509_free(cert);
        return FAILURE;
    }
#endif

    X509_free(cert);
    return SUCCESS;
}

static ssize_t nghttp2_client_send_cb(
    nghttp2_session *session,
    const uint8_t *data,
    size_t length,
    int flags,
    void *user_data
)
{
    nghttp2_client_object *intern = (nghttp2_client_object *)user_data;
    size_t off = 0;

    (void)session;
    (void)flags;

    while (off < length) {
        int n = SSL_write(intern->ssl, data + off, (int)(length - off));
        if (n <= 0) {
            return NGHTTP2_ERR_CALLBACK_FAILURE;
        }
        off += (size_t)n;
    }
    return (ssize_t)length;
}

static int nghttp2_client_on_header_cb(
    nghttp2_session *session,
    const nghttp2_frame *frame,
    const uint8_t *name,
    size_t namelen,
    const uint8_t *value,
    size_t valuelen,
    uint8_t flags,
    void *user_data
)
{
    nghttp2_client_object *intern = (nghttp2_client_object *)user_data;

    (void)session;
    (void)flags;

    if (frame->hd.type != NGHTTP2_HEADERS ||
        frame->headers.cat != NGHTTP2_HCAT_RESPONSE ||
        frame->hd.stream_id != intern->stream_id) {
        return 0;
    }

    if (namelen == sizeof(":status") - 1 && memcmp(name, ":status", sizeof(":status") - 1) == 0) {
        zend_long parsed = 0;
        if (is_numeric_string((const char *)value, valuelen, &parsed, NULL, 0) == IS_LONG) {
            intern->status = parsed;
            intern->has_status = 1;
        }
        return 0;
    }

    {
        zval header;
        zval name_zv;
        zval value_zv;

        array_init(&header);
        ZVAL_STRINGL(&name_zv, (const char *)name, namelen);
        ZVAL_STRINGL(&value_zv, (const char *)value, valuelen);
        zend_hash_str_update(Z_ARRVAL(header), "name", sizeof("name") - 1, &name_zv);
        zend_hash_str_update(Z_ARRVAL(header), "value", sizeof("value") - 1, &value_zv);
        add_next_index_zval(&intern->response_headers, &header);
    }

    return 0;
}

static int nghttp2_client_on_data_chunk_recv_cb(
    nghttp2_session *session,
    uint8_t flags,
    int32_t stream_id,
    const uint8_t *data,
    size_t len,
    void *user_data
)
{
    nghttp2_client_object *intern = (nghttp2_client_object *)user_data;

    (void)session;
    (void)flags;

    if (stream_id == intern->stream_id) {
        smart_str_appendl(&intern->response_body, (const char *)data, len);
    }
    return 0;
}

static int nghttp2_client_on_stream_close_cb(
    nghttp2_session *session,
    int32_t stream_id,
    uint32_t error_code,
    void *user_data
)
{
    nghttp2_client_object *intern = (nghttp2_client_object *)user_data;

    (void)session;
    (void)error_code;

    if (stream_id == intern->stream_id) {
        intern->stream_closed = 1;
    }
    return 0;
}

static int nghttp2_client_session_init(nghttp2_client_object *intern)
{
    nghttp2_session_callbacks *cbs = NULL;
    nghttp2_settings_entry iv[1];
    int rv;

    rv = nghttp2_session_callbacks_new(&cbs);
    if (rv != 0) {
        return FAILURE;
    }

    nghttp2_session_callbacks_set_send_callback(cbs, nghttp2_client_send_cb);
    nghttp2_session_callbacks_set_on_header_callback(cbs, nghttp2_client_on_header_cb);
    nghttp2_session_callbacks_set_on_data_chunk_recv_callback(cbs, nghttp2_client_on_data_chunk_recv_cb);
    nghttp2_session_callbacks_set_on_stream_close_callback(cbs, nghttp2_client_on_stream_close_cb);

    rv = nghttp2_session_client_new(&intern->session, cbs, intern);
    nghttp2_session_callbacks_del(cbs);
    if (rv != 0) {
        return FAILURE;
    }

    iv[0].settings_id = NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS;
    iv[0].value = 100;

    rv = nghttp2_submit_settings(intern->session, NGHTTP2_FLAG_NONE, iv, 1);
    if (rv != 0) {
        return FAILURE;
    }

    rv = nghttp2_session_send(intern->session);
    if (rv != 0) {
        return FAILURE;
    }

    return SUCCESS;
}

static zend_object *nghttp2_client_create_object(zend_class_entry *ce)
{
    nghttp2_client_object *intern = zend_object_alloc(sizeof(nghttp2_client_object), ce);

    intern->fd = -1;
    intern->ssl_ctx = NULL;
    intern->ssl = NULL;
    intern->session = NULL;
    intern->host = NULL;
    intern->port = 443;
    intern->stream_id = -1;
    intern->stream_closed = 0;
    intern->request_in_progress = 0;
    intern->has_status = 0;
    intern->status = 0;
    ZVAL_UNDEF(&intern->response_headers);
    memset(&intern->response_body, 0, sizeof(intern->response_body));

    zend_object_std_init(&intern->std, ce);
    object_properties_init(&intern->std, ce);
    intern->std.handlers = &nghttp2_client_object_handlers;

    return &intern->std;
}

static void nghttp2_client_free_object(zend_object *object)
{
    nghttp2_client_object *intern = nghttp2_client_from_obj(object);

    nghttp2_client_close_connection(intern);
    if (intern->host != NULL) {
        zend_string_release(intern->host);
        intern->host = NULL;
    }
    if (Z_TYPE(intern->response_headers) != IS_UNDEF) {
        zval_ptr_dtor(&intern->response_headers);
        ZVAL_UNDEF(&intern->response_headers);
    }
    smart_str_free(&intern->response_body);

    zend_object_std_dtor(&intern->std);
}

ZEND_BEGIN_ARG_INFO_EX(arginfo_client_construct, 0, 0, 1)
    ZEND_ARG_TYPE_INFO(0, host, IS_STRING, 0)
    ZEND_ARG_TYPE_INFO(0, port, IS_LONG, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_client_request, 0, 1, IS_ARRAY, 0)
    ZEND_ARG_TYPE_INFO(0, path, IS_STRING, 0)
    ZEND_ARG_ARRAY_INFO(0, headers, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_client_close, 0, 0, IS_VOID, 0)
ZEND_END_ARG_INFO()

ZEND_METHOD(Nghttp2_Client, __construct)
{
    zend_string *host;
    zend_long port = 443;
    nghttp2_client_object *intern = Z_NGHTTP2_CLIENT_OBJ_P(ZEND_THIS);
    zend_string *port_str;

    ZEND_PARSE_PARAMETERS_START(1, 2)
        Z_PARAM_STR(host)
        Z_PARAM_OPTIONAL
        Z_PARAM_LONG(port)
    ZEND_PARSE_PARAMETERS_END();

    if (ZSTR_LEN(host) == 0) {
        zend_argument_value_error(1, "must not be empty");
        RETURN_THROWS();
    }
    if (port <= 0 || port > 65535) {
        zend_argument_value_error(2, "must be between 1 and 65535");
        RETURN_THROWS();
    }

    SSL_library_init();
    SSL_load_error_strings();

    intern->host = zend_string_copy(host);
    intern->port = port;

    port_str = zend_long_to_str(port);
    intern->fd = nghttp2_client_tcp_connect(ZSTR_VAL(intern->host), ZSTR_VAL(port_str));
    zend_string_release(port_str);

    if (intern->fd < 0) {
        nghttp2_throw_client_exception("failed to connect to remote host", NGHTTP2_ERR_CALLBACK_FAILURE);
        RETURN_THROWS();
    }

    intern->ssl_ctx = nghttp2_client_sslctx_create();
    if (intern->ssl_ctx == NULL) {
        nghttp2_client_close_connection(intern);
        nghttp2_throw_client_exception("failed to initialize TLS context", NGHTTP2_ERR_CALLBACK_FAILURE);
        RETURN_THROWS();
    }

    if (nghttp2_client_ssl_handshake(intern) != SUCCESS) {
        nghttp2_client_close_connection(intern);
        nghttp2_throw_client_exception("TLS handshake failed or ALPN h2 not negotiated", NGHTTP2_ERR_CALLBACK_FAILURE);
        RETURN_THROWS();
    }

    if (nghttp2_client_session_init(intern) != SUCCESS) {
        nghttp2_client_close_connection(intern);
        nghttp2_throw_client_exception("failed to initialize HTTP/2 session", NGHTTP2_ERR_PROTO);
        RETURN_THROWS();
    }

    nghttp2_client_reset_response(intern);
}

static int nghttp2_client_append_custom_headers(nghttp2_nv *nva, uint32_t offset, zval *headers)
{
    HashTable *ht = Z_ARRVAL_P(headers);
    zval *entry;
    zend_string *key;
    zend_ulong index;
    uint32_t i = offset;

    ZEND_HASH_FOREACH_KEY_VAL(ht, index, key, entry) {
        zval *name = NULL;
        zval *value = NULL;

        if (key != NULL) {
            if (Z_TYPE_P(entry) != IS_STRING) {
                zend_type_error("when using associative headers, each value must be string");
                return FAILURE;
            }
            nva[i].name = (uint8_t *)ZSTR_VAL(key);
            nva[i].namelen = ZSTR_LEN(key);
            nva[i].value = (uint8_t *)Z_STRVAL_P(entry);
            nva[i].valuelen = Z_STRLEN_P(entry);
            nva[i].flags = NGHTTP2_NV_FLAG_NONE;
            i++;
            continue;
        }

        if (Z_TYPE_P(entry) != IS_ARRAY) {
            zend_type_error("each header must be string value or array{name, value}");
            return FAILURE;
        }
        name = zend_hash_str_find(Z_ARRVAL_P(entry), "name", sizeof("name") - 1);
        value = zend_hash_str_find(Z_ARRVAL_P(entry), "value", sizeof("value") - 1);
        if (name == NULL || value == NULL || Z_TYPE_P(name) != IS_STRING || Z_TYPE_P(value) != IS_STRING) {
            zend_type_error("each header array must contain string 'name' and 'value'");
            return FAILURE;
        }

        nva[i].name = (uint8_t *)Z_STRVAL_P(name);
        nva[i].namelen = Z_STRLEN_P(name);
        nva[i].value = (uint8_t *)Z_STRVAL_P(value);
        nva[i].valuelen = Z_STRLEN_P(value);
        nva[i].flags = NGHTTP2_NV_FLAG_NONE;
        i++;
    } ZEND_HASH_FOREACH_END();

    return SUCCESS;
}

ZEND_METHOD(Nghttp2_Client, request)
{
    zend_string *path;
    zval *headers = NULL;
    nghttp2_client_object *intern = Z_NGHTTP2_CLIENT_OBJ_P(ZEND_THIS);
    uint8_t rbuf[16 * 1024];
    zend_string *authority;
    uint32_t extra_headers = 0;
    uint32_t nvlen;
    nghttp2_nv *nva;
    int rv;

    ZEND_PARSE_PARAMETERS_START(1, 2)
        Z_PARAM_STR(path)
        Z_PARAM_OPTIONAL
        Z_PARAM_ARRAY(headers)
    ZEND_PARSE_PARAMETERS_END();

    if (intern->session == NULL || intern->ssl == NULL) {
        nghttp2_throw_client_exception("connection is closed", NGHTTP2_ERR_INVALID_STATE);
        RETURN_THROWS();
    }
    if (intern->request_in_progress) {
        nghttp2_throw_client_exception("another request is already in progress", NGHTTP2_ERR_INVALID_STATE);
        RETURN_THROWS();
    }

    authority = strpprintf(0, "%s:%ld", ZSTR_VAL(intern->host), intern->port);
    if (headers != NULL) {
        extra_headers = zend_hash_num_elements(Z_ARRVAL_P(headers));
    }

    nvlen = 6 + extra_headers;
    nva = ecalloc(nvlen, sizeof(nghttp2_nv));

    nva[0].name = (uint8_t *)":method";
    nva[0].namelen = sizeof(":method") - 1;
    nva[0].value = (uint8_t *)"GET";
    nva[0].valuelen = sizeof("GET") - 1;
    nva[0].flags = NGHTTP2_NV_FLAG_NONE;

    nva[1].name = (uint8_t *)":scheme";
    nva[1].namelen = sizeof(":scheme") - 1;
    nva[1].value = (uint8_t *)"https";
    nva[1].valuelen = sizeof("https") - 1;
    nva[1].flags = NGHTTP2_NV_FLAG_NONE;

    nva[2].name = (uint8_t *)":authority";
    nva[2].namelen = sizeof(":authority") - 1;
    nva[2].value = (uint8_t *)ZSTR_VAL(authority);
    nva[2].valuelen = ZSTR_LEN(authority);
    nva[2].flags = NGHTTP2_NV_FLAG_NONE;

    nva[3].name = (uint8_t *)":path";
    nva[3].namelen = sizeof(":path") - 1;
    nva[3].value = (uint8_t *)ZSTR_VAL(path);
    nva[3].valuelen = ZSTR_LEN(path);
    nva[3].flags = NGHTTP2_NV_FLAG_NONE;

    nva[4].name = (uint8_t *)"user-agent";
    nva[4].namelen = sizeof("user-agent") - 1;
    nva[4].value = (uint8_t *)"php-nghttp2-client/0.1";
    nva[4].valuelen = sizeof("php-nghttp2-client/0.1") - 1;
    nva[4].flags = NGHTTP2_NV_FLAG_NONE;

    nva[5].name = (uint8_t *)"accept";
    nva[5].namelen = sizeof("accept") - 1;
    nva[5].value = (uint8_t *)"*/*";
    nva[5].valuelen = sizeof("*/*") - 1;
    nva[5].flags = NGHTTP2_NV_FLAG_NONE;

    if (headers != NULL && nghttp2_client_append_custom_headers(nva, 6, headers) != SUCCESS) {
        zend_string_release(authority);
        efree(nva);
        RETURN_THROWS();
    }

    nghttp2_client_reset_response(intern);
    intern->stream_closed = 0;
    intern->request_in_progress = 1;

    intern->stream_id = nghttp2_submit_request(intern->session, NULL, nva, nvlen, NULL, NULL);
    zend_string_release(authority);
    efree(nva);

    if (intern->stream_id < 0) {
        intern->request_in_progress = 0;
        nghttp2_throw_client_exception("failed to submit request", intern->stream_id);
        RETURN_THROWS();
    }

    while (!intern->stream_closed) {
        ssize_t fed;
        int n;

        rv = nghttp2_session_send(intern->session);
        if (rv != 0) {
            intern->request_in_progress = 0;
            nghttp2_throw_client_exception(nghttp2_strerror(rv), rv);
            RETURN_THROWS();
        }

        n = SSL_read(intern->ssl, rbuf, sizeof(rbuf));
        if (n <= 0) {
            intern->request_in_progress = 0;
            nghttp2_throw_client_exception("failed to read response from TLS stream", NGHTTP2_ERR_EOF);
            RETURN_THROWS();
        }

        fed = nghttp2_session_mem_recv(intern->session, rbuf, (size_t)n);
        if (fed < 0) {
            intern->request_in_progress = 0;
            nghttp2_throw_client_exception(nghttp2_strerror((int)fed), (int)fed);
            RETURN_THROWS();
        }
    }

    (void)nghttp2_session_send(intern->session);
    intern->request_in_progress = 0;
    intern->stream_id = -1;

    smart_str_0(&intern->response_body);

    array_init(return_value);
    if (intern->has_status) {
        add_assoc_long(return_value, "status", intern->status);
    } else {
        add_assoc_null(return_value, "status");
    }
    {
        zval headers_out;
        ZVAL_COPY(&headers_out, &intern->response_headers);
        add_assoc_zval(return_value, "headers", &headers_out);
    }
    if (intern->response_body.s != NULL) {
        add_assoc_str(return_value, "body", zend_string_copy(intern->response_body.s));
    } else {
        add_assoc_stringl(return_value, "body", "", 0);
    }
}

ZEND_METHOD(Nghttp2_Client, close)
{
    nghttp2_client_object *intern = Z_NGHTTP2_CLIENT_OBJ_P(ZEND_THIS);

    ZEND_PARSE_PARAMETERS_NONE();

    nghttp2_client_close_connection(intern);
    intern->request_in_progress = 0;
    intern->stream_id = -1;
}

static const zend_function_entry nghttp2_client_methods[] = {
    ZEND_ME(Nghttp2_Client, __construct, arginfo_client_construct, ZEND_ACC_PUBLIC)
    ZEND_ME(Nghttp2_Client, request, arginfo_client_request, ZEND_ACC_PUBLIC)
    ZEND_ME(Nghttp2_Client, close, arginfo_client_close, ZEND_ACC_PUBLIC)
    ZEND_FE_END
};

void nghttp2_register_client_class(void)
{
    zend_class_entry ce;

    INIT_NS_CLASS_ENTRY(ce, "Nghttp2", "Client", nghttp2_client_methods);
    nghttp2_ce_client = zend_register_internal_class(&ce);
    nghttp2_ce_client->create_object = nghttp2_client_create_object;
    nghttp2_ce_client->ce_flags |= ZEND_ACC_FINAL;

    memcpy(&nghttp2_client_object_handlers, &std_object_handlers, sizeof(zend_object_handlers));
    nghttp2_client_object_handlers.offset = XtOffsetOf(nghttp2_client_object, std);
    nghttp2_client_object_handlers.free_obj = nghttp2_client_free_object;
}
