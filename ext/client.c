#include "php_nghttp2.h"

#include <Zend/zend_smart_str.h>
#include <Zend/zend_exceptions.h>

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
    zval session_obj;

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
    if (Z_TYPE(intern->session_obj) != IS_UNDEF) {
        nghttp2_session_close_zval(&intern->session_obj);
        zval_ptr_dtor(&intern->session_obj);
        ZVAL_UNDEF(&intern->session_obj);
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

static void nghttp2_client_rethrow_session_exception(const char *fallback_message, int fallback_code)
{
    zend_string *message = NULL;
    int error_code = fallback_code;

    if (EG(exception) != NULL && instanceof_function(EG(exception)->ce, nghttp2_ce_session_exception)) {
        zval *message_zv;
        zval *code_zv;

        message_zv = zend_read_property(zend_ce_exception, EG(exception), "message", sizeof("message") - 1, 1, NULL);
        code_zv = zend_read_property(nghttp2_ce_session_exception, EG(exception), "nghttp2ErrorCode", sizeof("nghttp2ErrorCode") - 1, 1, NULL);

        if (message_zv != NULL && Z_TYPE_P(message_zv) == IS_STRING) {
            message = zend_string_copy(Z_STR_P(message_zv));
        }
        if (code_zv != NULL) {
            error_code = zval_get_long(code_zv);
        }
    }

    if (EG(exception) != NULL) {
        OBJ_RELEASE(EG(exception));
        EG(exception) = NULL;
    }
    if (message != NULL) {
        nghttp2_throw_client_exception(ZSTR_VAL(message), error_code);
        zend_string_release(message);
    } else {
        nghttp2_throw_client_exception(fallback_message, error_code);
    }
}

static int nghttp2_client_ssl_write_all(nghttp2_client_object *intern, zend_string *data)
{
    size_t off = 0;

    while (off < ZSTR_LEN(data)) {
        int n = SSL_write(intern->ssl, ZSTR_VAL(data) + off, (int)(ZSTR_LEN(data) - off));
        if (n <= 0) {
            nghttp2_throw_client_exception("failed to write request to TLS stream", NGHTTP2_ERR_CALLBACK_FAILURE);
            return FAILURE;
        }
        off += (size_t)n;
    }

    return SUCCESS;
}

static int nghttp2_client_flush_session_outbound(nghttp2_client_object *intern)
{
    zend_string *outbound;
    int result = SUCCESS;

    outbound = nghttp2_session_pop_outbound_string(&intern->session_obj);
    if (ZSTR_LEN(outbound) > 0) {
        result = nghttp2_client_ssl_write_all(intern, outbound);
    }
    zend_string_release(outbound);

    return result;
}

static void nghttp2_client_append_header_pair(zval *headers, zval *pair)
{
    zval header;
    zval *name;
    zval *value;

    name = zend_hash_str_find(Z_ARRVAL_P(pair), "name", sizeof("name") - 1);
    value = zend_hash_str_find(Z_ARRVAL_P(pair), "value", sizeof("value") - 1);
    if (name == NULL || value == NULL || Z_TYPE_P(name) != IS_STRING || Z_TYPE_P(value) != IS_STRING) {
        return;
    }

    array_init(&header);
    add_assoc_str(&header, "name", zend_string_copy(Z_STR_P(name)));
    add_assoc_str(&header, "value", zend_string_copy(Z_STR_P(value)));
    add_next_index_zval(headers, &header);
}

static void nghttp2_client_process_session_events(nghttp2_client_object *intern)
{
    zval events;
    zval *event;

    nghttp2_session_pop_events_array(&intern->session_obj, &events);

    ZEND_HASH_FOREACH_VAL(Z_ARRVAL(events), event) {
        zval *type;
        zval *stream_id;

        if (Z_TYPE_P(event) != IS_ARRAY) {
            continue;
        }

        type = zend_hash_str_find(Z_ARRVAL_P(event), "type", sizeof("type") - 1);
        stream_id = zend_hash_str_find(Z_ARRVAL_P(event), "streamId", sizeof("streamId") - 1);
        if (type == NULL || Z_TYPE_P(type) != IS_STRING || stream_id == NULL || zval_get_long(stream_id) != intern->stream_id) {
            continue;
        }

        if (zend_string_equals_literal(Z_STR_P(type), "headers")) {
            zval *headers = zend_hash_str_find(Z_ARRVAL_P(event), "headers", sizeof("headers") - 1);
            zval *header;

            if (headers == NULL || Z_TYPE_P(headers) != IS_ARRAY) {
                continue;
            }

            ZEND_HASH_FOREACH_VAL(Z_ARRVAL_P(headers), header) {
                zval *name;
                zval *value;
                zend_long parsed = 0;

                if (Z_TYPE_P(header) != IS_ARRAY) {
                    continue;
                }

                name = zend_hash_str_find(Z_ARRVAL_P(header), "name", sizeof("name") - 1);
                value = zend_hash_str_find(Z_ARRVAL_P(header), "value", sizeof("value") - 1);
                if (name == NULL || value == NULL || Z_TYPE_P(name) != IS_STRING || Z_TYPE_P(value) != IS_STRING) {
                    continue;
                }

                if (zend_string_equals_literal(Z_STR_P(name), ":status")) {
                    if (is_numeric_string(Z_STRVAL_P(value), Z_STRLEN_P(value), &parsed, NULL, 0) == IS_LONG) {
                        intern->status = parsed;
                        intern->has_status = 1;
                    }
                    continue;
                }

                nghttp2_client_append_header_pair(&intern->response_headers, header);
            } ZEND_HASH_FOREACH_END();
        } else if (zend_string_equals_literal(Z_STR_P(type), "data")) {
            zval *data = zend_hash_str_find(Z_ARRVAL_P(event), "data", sizeof("data") - 1);

            if (data != NULL && Z_TYPE_P(data) == IS_STRING) {
                smart_str_appendl(&intern->response_body, Z_STRVAL_P(data), Z_STRLEN_P(data));
            }
        } else if (zend_string_equals_literal(Z_STR_P(type), "stream_close")) {
            intern->stream_closed = 1;
        }
    } ZEND_HASH_FOREACH_END();

    zval_ptr_dtor(&events);
}

static void nghttp2_client_request_headers_append(zval *headers, const char *name, zend_string *value)
{
    zval pair;

    array_init(&pair);
    add_assoc_string(&pair, "name", (char *)name);
    add_assoc_str(&pair, "value", zend_string_copy(value));
    add_next_index_zval(headers, &pair);
}

static zend_object *nghttp2_client_create_object(zend_class_entry *ce)
{
    nghttp2_client_object *intern = zend_object_alloc(sizeof(nghttp2_client_object), ce);

    intern->fd = -1;
    intern->ssl_ctx = NULL;
    intern->ssl = NULL;
    ZVAL_UNDEF(&intern->session_obj);
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

    if (nghttp2_session_create(&intern->session_obj, 0) != SUCCESS) {
        nghttp2_client_close_connection(intern);
        nghttp2_client_rethrow_session_exception("failed to initialize HTTP/2 session", NGHTTP2_ERR_PROTO);
        RETURN_THROWS();
    }

    if (nghttp2_client_flush_session_outbound(intern) != SUCCESS) {
        nghttp2_client_close_connection(intern);
        RETURN_THROWS();
    }

    nghttp2_client_reset_response(intern);
}

ZEND_METHOD(Nghttp2_Client, request)
{
    zend_string *path;
    zval *headers = NULL;
    zval normalized_headers;
    zval request_headers;
    nghttp2_client_object *intern = Z_NGHTTP2_CLIENT_OBJ_P(ZEND_THIS);
    uint8_t rbuf[16 * 1024];
    zend_string *authority;
    int32_t submitted_stream_id;

    ZEND_PARSE_PARAMETERS_START(1, 2)
        Z_PARAM_STR(path)
        Z_PARAM_OPTIONAL
        Z_PARAM_ARRAY(headers)
    ZEND_PARSE_PARAMETERS_END();

    ZVAL_UNDEF(&normalized_headers);
    array_init(&request_headers);

    if (Z_TYPE(intern->session_obj) == IS_UNDEF || intern->ssl == NULL) {
        zval_ptr_dtor(&request_headers);
        nghttp2_throw_client_exception("connection is closed", NGHTTP2_ERR_INVALID_STATE);
        RETURN_THROWS();
    }
    if (intern->request_in_progress) {
        zval_ptr_dtor(&request_headers);
        nghttp2_throw_client_exception("another request is already in progress", NGHTTP2_ERR_INVALID_STATE);
        RETURN_THROWS();
    }

    authority = strpprintf(0, "%s:%ld", ZSTR_VAL(intern->host), intern->port);
    if (headers != NULL) {
        if (nghttp2_headers_normalize(headers, &normalized_headers, NGHTTP2_HEADERS_NORMALIZE_ALLOW_ASSOC) != SUCCESS) {
            zend_string_release(authority);
            zval_ptr_dtor(&request_headers);
            RETURN_THROWS();
        }
    }

    nghttp2_client_request_headers_append(&request_headers, ":method", zend_string_init("GET", sizeof("GET") - 1, 0));
    nghttp2_client_request_headers_append(&request_headers, ":scheme", zend_string_init("https", sizeof("https") - 1, 0));
    nghttp2_client_request_headers_append(&request_headers, ":authority", authority);
    nghttp2_client_request_headers_append(&request_headers, ":path", path);
    nghttp2_client_request_headers_append(&request_headers, "user-agent", zend_string_init("php-nghttp2-client/0.1", sizeof("php-nghttp2-client/0.1") - 1, 0));
    nghttp2_client_request_headers_append(&request_headers, "accept", zend_string_init("*/*", sizeof("*/*") - 1, 0));

    if (Z_TYPE(normalized_headers) != IS_UNDEF) {
        zval *header;

        ZEND_HASH_FOREACH_VAL(Z_ARRVAL(normalized_headers), header) {
            if (Z_TYPE_P(header) != IS_ARRAY) {
                continue;
            }
            Z_TRY_ADDREF_P(header);
            add_next_index_zval(&request_headers, header);
        } ZEND_HASH_FOREACH_END();
        zval_ptr_dtor(&normalized_headers);
    }

    nghttp2_client_reset_response(intern);
    intern->stream_closed = 0;
    intern->request_in_progress = 1;

    if (nghttp2_session_submit_request_headers(&intern->session_obj, &request_headers, 1, &submitted_stream_id) != SUCCESS) {
        intern->request_in_progress = 0;
        zend_string_release(authority);
        zval_ptr_dtor(&request_headers);
        nghttp2_client_rethrow_session_exception("failed to submit request", NGHTTP2_ERR_CALLBACK_FAILURE);
        RETURN_THROWS();
    }

    intern->stream_id = submitted_stream_id;
    zend_string_release(authority);
    zval_ptr_dtor(&request_headers);

    if (nghttp2_client_flush_session_outbound(intern) != SUCCESS) {
        intern->request_in_progress = 0;
        RETURN_THROWS();
    }

    while (!intern->stream_closed) {
        int n;

        n = SSL_read(intern->ssl, rbuf, sizeof(rbuf));
        if (n <= 0) {
            intern->request_in_progress = 0;
            nghttp2_throw_client_exception("failed to read response from TLS stream", NGHTTP2_ERR_EOF);
            RETURN_THROWS();
        }

        if (nghttp2_session_receive_bytes(&intern->session_obj, rbuf, (size_t)n, NULL) != SUCCESS) {
            intern->request_in_progress = 0;
            nghttp2_client_rethrow_session_exception("failed to process response bytes", NGHTTP2_ERR_PROTO);
            RETURN_THROWS();
        }

        nghttp2_client_process_session_events(intern);

        if (nghttp2_client_flush_session_outbound(intern) != SUCCESS) {
            intern->request_in_progress = 0;
            RETURN_THROWS();
        }
    }

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
