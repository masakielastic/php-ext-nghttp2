#include "php_nghttp2.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <openssl/err.h>
#include <openssl/ssl.h>

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

typedef struct _nghttp2_server_object {
    int listen_fd;
    SSL_CTX *ssl_ctx;

    zend_string *ip;
    zend_long port;
    zend_string *cert_file;
    zend_string *key_file;

    zend_long response_status;
    zval response_headers;
    zend_string *response_body;

    zend_object std;
} nghttp2_server_object;

typedef struct _nghttp2_server_conn_ctx {
    int fd;
    SSL *ssl;
    nghttp2_server_object *server;
} nghttp2_server_conn_ctx;

typedef struct _nghttp2_server_stream_body {
    uint8_t *data;
    size_t len;
    size_t off;
} nghttp2_server_stream_body;

zend_class_entry *nghttp2_ce_server;
static zend_object_handlers nghttp2_server_object_handlers;

static inline nghttp2_server_object *nghttp2_server_from_obj(zend_object *obj)
{
    return (nghttp2_server_object *)((char *)(obj) - XtOffsetOf(nghttp2_server_object, std));
}

#define Z_NGHTTP2_SERVER_OBJ_P(zv) nghttp2_server_from_obj(Z_OBJ_P((zv)))

static void nghttp2_server_close_listener(nghttp2_server_object *intern)
{
    if (intern->listen_fd >= 0) {
        close(intern->listen_fd);
        intern->listen_fd = -1;
    }
    if (intern->ssl_ctx != NULL) {
        SSL_CTX_free(intern->ssl_ctx);
        intern->ssl_ctx = NULL;
    }
}

static void nghttp2_server_set_default_response(nghttp2_server_object *intern)
{
    if (Z_TYPE(intern->response_headers) != IS_UNDEF) {
        zval_ptr_dtor(&intern->response_headers);
    }
    if (intern->response_body != NULL) {
        zend_string_release(intern->response_body);
    }

    intern->response_status = 200;
    array_init(&intern->response_headers);
    add_assoc_string(&intern->response_headers, "content-type", "text/plain; charset=utf-8");
    intern->response_body = zend_string_init("Hello HTTP/2 over TLS\n", sizeof("Hello HTTP/2 over TLS\n") - 1, 0);
}

static int nghttp2_server_alpn_select_cb(
    SSL *ssl,
    const unsigned char **out,
    unsigned char *outlen,
    const unsigned char *in,
    unsigned int inlen,
    void *arg
)
{
    static const unsigned char h2[] = {0x02, 'h', '2'};

    (void)ssl;
    (void)arg;

    if (SSL_select_next_proto((unsigned char **)out, outlen, h2, sizeof(h2), in, inlen) == OPENSSL_NPN_NEGOTIATED) {
        return SSL_TLSEXT_ERR_OK;
    }

    return SSL_TLSEXT_ERR_NOACK;
}

static SSL_CTX *nghttp2_server_sslctx_create(const char *cert_file, const char *key_file)
{
    SSL_CTX *ctx = SSL_CTX_new(TLS_server_method());
    if (ctx == NULL) {
        return NULL;
    }

    if (!SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION)) {
        SSL_CTX_free(ctx);
        return NULL;
    }

    if (SSL_CTX_use_certificate_file(ctx, cert_file, SSL_FILETYPE_PEM) != 1) {
        SSL_CTX_free(ctx);
        return NULL;
    }
    if (SSL_CTX_use_PrivateKey_file(ctx, key_file, SSL_FILETYPE_PEM) != 1) {
        SSL_CTX_free(ctx);
        return NULL;
    }
    if (SSL_CTX_check_private_key(ctx) != 1) {
        SSL_CTX_free(ctx);
        return NULL;
    }

    SSL_CTX_set_alpn_select_cb(ctx, nghttp2_server_alpn_select_cb, NULL);

#ifdef SSL_OP_NO_COMPRESSION
    SSL_CTX_set_options(ctx, SSL_OP_NO_COMPRESSION);
#endif

    return ctx;
}

static int nghttp2_server_create_listen_socket(const char *ip, zend_long port)
{
    int fd;
    int yes = 1;
    struct sockaddr_in addr;

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }

    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) < 0) {
        close(fd);
        return -1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, ip, &addr.sin_addr) != 1) {
        close(fd);
        return -1;
    }

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }
    if (listen(fd, 16) < 0) {
        close(fd);
        return -1;
    }

    return fd;
}

static ssize_t nghttp2_server_send_cb(
    nghttp2_session *session,
    const uint8_t *data,
    size_t length,
    int flags,
    void *user_data
)
{
    nghttp2_server_conn_ctx *conn = (nghttp2_server_conn_ctx *)user_data;
    size_t off = 0;

    (void)session;
    (void)flags;

    while (off < length) {
        int n = SSL_write(conn->ssl, data + off, (int)(length - off));
        if (n <= 0) {
            int err = SSL_get_error(conn->ssl, n);
            if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
                return NGHTTP2_ERR_WOULDBLOCK;
            }
            return NGHTTP2_ERR_CALLBACK_FAILURE;
        }
        off += (size_t)n;
    }

    return (ssize_t)length;
}

static ssize_t nghttp2_server_data_read_cb(
    nghttp2_session *session,
    int32_t stream_id,
    uint8_t *buf,
    size_t length,
    uint32_t *data_flags,
    nghttp2_data_source *source,
    void *user_data
)
{
    nghttp2_server_stream_body *body = (nghttp2_server_stream_body *)source->ptr;
    size_t remain;
    size_t ncopy;

    (void)session;
    (void)stream_id;
    (void)user_data;

    remain = body->len - body->off;
    ncopy = remain < length ? remain : length;
    if (ncopy > 0) {
        memcpy(buf, body->data + body->off, ncopy);
        body->off += ncopy;
    }
    if (body->off >= body->len) {
        *data_flags |= NGHTTP2_DATA_FLAG_EOF;
    }

    return (ssize_t)ncopy;
}

static int nghttp2_server_submit_response(
    nghttp2_server_object *intern,
    nghttp2_session *session,
    int32_t stream_id
)
{
    char content_length[32];
    char status[4];
    nghttp2_nv *hdrs;
    nghttp2_nv *user_hdrs = NULL;
    nghttp2_data_provider dp;
    nghttp2_server_stream_body *body;
    size_t user_header_count = 0;
    uint32_t hdr_count = 2;
    int rv;

    if (nghttp2_headers_build_nv_array(&intern->response_headers, &user_hdrs, &user_header_count) != SUCCESS) {
        return NGHTTP2_ERR_CALLBACK_FAILURE;
    }

    hdrs = ecalloc(user_header_count + 2, sizeof(*hdrs));

    body = ecalloc(1, sizeof(*body));
    body->len = ZSTR_LEN(intern->response_body);
    body->data = emalloc(body->len > 0 ? body->len : 1);
    if (body->len > 0) {
        memcpy(body->data, ZSTR_VAL(intern->response_body), body->len);
    }
    body->off = 0;

    nghttp2_session_set_stream_user_data(session, stream_id, body);

    snprintf(content_length, sizeof(content_length), "%zu", body->len);
    snprintf(status, sizeof(status), "%03ld", intern->response_status);

    hdrs[0].name = (uint8_t *)":status";
    hdrs[0].value = (uint8_t *)status;
    hdrs[0].namelen = sizeof(":status") - 1;
    hdrs[0].valuelen = sizeof(status) - 1;
    hdrs[0].flags = NGHTTP2_NV_FLAG_NONE;

    hdrs[1].name = (uint8_t *)"content-length";
    hdrs[1].value = (uint8_t *)content_length;
    hdrs[1].namelen = sizeof("content-length") - 1;
    hdrs[1].valuelen = (uint16_t)strlen(content_length);
    hdrs[1].flags = NGHTTP2_NV_FLAG_NONE;

    if (user_hdrs != NULL) {
        memcpy(&hdrs[2], user_hdrs, user_header_count * sizeof(*user_hdrs));
        hdr_count += (uint32_t)user_header_count;
        efree(user_hdrs);
        user_hdrs = NULL;
    }

    memset(&dp, 0, sizeof(dp));
    dp.source.ptr = body;
    dp.read_callback = nghttp2_server_data_read_cb;

    rv = nghttp2_submit_response(session, stream_id, hdrs, hdr_count, &dp);
    efree(hdrs);
    if (rv != 0) {
        nghttp2_session_set_stream_user_data(session, stream_id, NULL);
        efree(body->data);
        efree(body);
        if (user_hdrs != NULL) {
            efree(user_hdrs);
        }
        return rv;
    }

    return 0;
}

static int nghttp2_server_on_frame_recv_cb(
    nghttp2_session *session,
    const nghttp2_frame *frame,
    void *user_data
)
{
    nghttp2_server_conn_ctx *conn = (nghttp2_server_conn_ctx *)user_data;
    int rv;

    if (frame->hd.stream_id <= 0) {
        return 0;
    }

    if (frame->hd.type == NGHTTP2_HEADERS &&
        frame->headers.cat == NGHTTP2_HCAT_REQUEST &&
        (frame->hd.flags & NGHTTP2_FLAG_END_STREAM)) {
        rv = nghttp2_server_submit_response(conn->server, session, frame->hd.stream_id);
        if (rv != 0) {
            return NGHTTP2_ERR_CALLBACK_FAILURE;
        }
    }

    if (frame->hd.type == NGHTTP2_DATA &&
        (frame->hd.flags & NGHTTP2_FLAG_END_STREAM) &&
        nghttp2_session_get_stream_user_data(session, frame->hd.stream_id) == NULL) {
        rv = nghttp2_server_submit_response(conn->server, session, frame->hd.stream_id);
        if (rv != 0) {
            return NGHTTP2_ERR_CALLBACK_FAILURE;
        }
    }

    return 0;
}

static int nghttp2_server_on_stream_close_cb(
    nghttp2_session *session,
    int32_t stream_id,
    uint32_t error_code,
    void *user_data
)
{
    nghttp2_server_stream_body *body;

    (void)error_code;
    (void)user_data;

    body = (nghttp2_server_stream_body *)nghttp2_session_get_stream_user_data(session, stream_id);
    if (body != NULL) {
        efree(body->data);
        efree(body);
        nghttp2_session_set_stream_user_data(session, stream_id, NULL);
    }

    return 0;
}

static nghttp2_session *nghttp2_server_setup_session(nghttp2_server_conn_ctx *conn)
{
    nghttp2_session_callbacks *cbs = NULL;
    nghttp2_session *session = NULL;
    nghttp2_settings_entry iv[1];
    int rv;

    rv = nghttp2_session_callbacks_new(&cbs);
    if (rv != 0) {
        return NULL;
    }

    nghttp2_session_callbacks_set_send_callback(cbs, nghttp2_server_send_cb);
    nghttp2_session_callbacks_set_on_frame_recv_callback(cbs, nghttp2_server_on_frame_recv_cb);
    nghttp2_session_callbacks_set_on_stream_close_callback(cbs, nghttp2_server_on_stream_close_cb);

    rv = nghttp2_session_server_new(&session, cbs, conn);
    nghttp2_session_callbacks_del(cbs);
    if (rv != 0) {
        return NULL;
    }

    iv[0].settings_id = NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS;
    iv[0].value = 100;
    rv = nghttp2_submit_settings(session, NGHTTP2_FLAG_NONE, iv, 1);
    if (rv != 0) {
        nghttp2_session_del(session);
        return NULL;
    }

    return session;
}

static void nghttp2_server_handle_connection(nghttp2_server_object *intern, int client_fd)
{
    nghttp2_server_conn_ctx conn;
    nghttp2_session *session;
    uint8_t buf[16 * 1024];
    const unsigned char *alpn = NULL;
    unsigned int alpn_len = 0;
    int rv;

    memset(&conn, 0, sizeof(conn));
    conn.fd = client_fd;
    conn.server = intern;
    conn.ssl = SSL_new(intern->ssl_ctx);
    if (conn.ssl == NULL) {
        close(client_fd);
        return;
    }
    SSL_set_fd(conn.ssl, client_fd);

    rv = SSL_accept(conn.ssl);
    if (rv != 1) {
        SSL_free(conn.ssl);
        close(client_fd);
        return;
    }

    SSL_get0_alpn_selected(conn.ssl, &alpn, &alpn_len);
    if (!(alpn_len == 2 && memcmp(alpn, "h2", 2) == 0)) {
        SSL_shutdown(conn.ssl);
        SSL_free(conn.ssl);
        close(client_fd);
        return;
    }

    session = nghttp2_server_setup_session(&conn);
    if (session == NULL) {
        SSL_shutdown(conn.ssl);
        SSL_free(conn.ssl);
        close(client_fd);
        return;
    }

    if (nghttp2_session_send(session) != 0) {
        nghttp2_session_del(session);
        SSL_shutdown(conn.ssl);
        SSL_free(conn.ssl);
        close(client_fd);
        return;
    }

    for (;;) {
        int n = SSL_read(conn.ssl, buf, (int)sizeof(buf));
        if (n == 0) {
            break;
        }
        if (n < 0) {
            int ssl_err = SSL_get_error(conn.ssl, n);
            if (ssl_err == SSL_ERROR_WANT_READ || ssl_err == SSL_ERROR_WANT_WRITE) {
                continue;
            }
            break;
        }

        if (nghttp2_session_mem_recv(session, buf, (size_t)n) < 0) {
            break;
        }
        if (nghttp2_session_send(session) != 0) {
            break;
        }
        if (nghttp2_session_want_read(session) == 0 &&
            nghttp2_session_want_write(session) == 0) {
            break;
        }
    }

    nghttp2_session_del(session);
    SSL_shutdown(conn.ssl);
    SSL_free(conn.ssl);
    close(client_fd);
}

static zend_object *nghttp2_server_create_object(zend_class_entry *ce)
{
    nghttp2_server_object *intern = zend_object_alloc(sizeof(nghttp2_server_object), ce);

    intern->listen_fd = -1;
    intern->ssl_ctx = NULL;
    intern->ip = NULL;
    intern->port = 8443;
    intern->cert_file = NULL;
    intern->key_file = NULL;
    intern->response_status = 200;
    ZVAL_UNDEF(&intern->response_headers);
    intern->response_body = NULL;

    zend_object_std_init(&intern->std, ce);
    object_properties_init(&intern->std, ce);
    intern->std.handlers = &nghttp2_server_object_handlers;

    return &intern->std;
}

static void nghttp2_server_free_object(zend_object *object)
{
    nghttp2_server_object *intern = nghttp2_server_from_obj(object);

    nghttp2_server_close_listener(intern);
    if (intern->ip != NULL) {
        zend_string_release(intern->ip);
    }
    if (intern->cert_file != NULL) {
        zend_string_release(intern->cert_file);
    }
    if (intern->key_file != NULL) {
        zend_string_release(intern->key_file);
    }
    if (Z_TYPE(intern->response_headers) != IS_UNDEF) {
        zval_ptr_dtor(&intern->response_headers);
    }
    if (intern->response_body != NULL) {
        zend_string_release(intern->response_body);
    }

    zend_object_std_dtor(&intern->std);
}

ZEND_BEGIN_ARG_INFO_EX(arginfo_server_construct, 0, 0, 2)
    ZEND_ARG_TYPE_INFO(0, certFile, IS_STRING, 0)
    ZEND_ARG_TYPE_INFO(0, keyFile, IS_STRING, 0)
    ZEND_ARG_TYPE_INFO(0, ip, IS_STRING, 0)
    ZEND_ARG_TYPE_INFO(0, port, IS_LONG, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_server_serve, 0, 0, IS_VOID, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_server_set_response, 0, 3, IS_VOID, 0)
    ZEND_ARG_TYPE_INFO(0, status, IS_LONG, 0)
    ZEND_ARG_ARRAY_INFO(0, headers, 0)
    ZEND_ARG_TYPE_INFO(0, body, IS_STRING, 0)
ZEND_END_ARG_INFO()

ZEND_METHOD(Nghttp2_Server, __construct)
{
    zend_string *cert_file;
    zend_string *key_file;
    zend_string *ip = NULL;
    zend_long port = 8443;
    nghttp2_server_object *intern = Z_NGHTTP2_SERVER_OBJ_P(ZEND_THIS);

    ZEND_PARSE_PARAMETERS_START(2, 4)
        Z_PARAM_STR(cert_file)
        Z_PARAM_STR(key_file)
        Z_PARAM_OPTIONAL
        Z_PARAM_STR(ip)
        Z_PARAM_LONG(port)
    ZEND_PARSE_PARAMETERS_END();

    if (port <= 0 || port > 65535) {
        zend_argument_value_error(4, "must be between 1 and 65535");
        RETURN_THROWS();
    }

    if (access(ZSTR_VAL(cert_file), R_OK) != 0) {
        nghttp2_throw_server_exception("certificate file is not readable", NGHTTP2_ERR_INVALID_ARGUMENT);
        RETURN_THROWS();
    }
    if (access(ZSTR_VAL(key_file), R_OK) != 0) {
        nghttp2_throw_server_exception("private key file is not readable", NGHTTP2_ERR_INVALID_ARGUMENT);
        RETURN_THROWS();
    }

    if (ip == NULL) {
        ip = zend_string_init("127.0.0.1", sizeof("127.0.0.1") - 1, 0);
    } else {
        ip = zend_string_copy(ip);
    }

    SSL_library_init();
    SSL_load_error_strings();
    OpenSSL_add_ssl_algorithms();
    signal(SIGPIPE, SIG_IGN);

    intern->ssl_ctx = nghttp2_server_sslctx_create(ZSTR_VAL(cert_file), ZSTR_VAL(key_file));
    if (intern->ssl_ctx == NULL) {
        zend_string_release(ip);
        nghttp2_throw_server_exception("failed to initialize TLS server context", NGHTTP2_ERR_CALLBACK_FAILURE);
        RETURN_THROWS();
    }

    intern->listen_fd = nghttp2_server_create_listen_socket(ZSTR_VAL(ip), port);
    if (intern->listen_fd < 0) {
        zend_string_release(ip);
        nghttp2_server_close_listener(intern);
        nghttp2_throw_server_exception("failed to create listen socket", NGHTTP2_ERR_CALLBACK_FAILURE);
        RETURN_THROWS();
    }

    intern->ip = ip;
    intern->port = port;
    intern->cert_file = zend_string_copy(cert_file);
    intern->key_file = zend_string_copy(key_file);
    nghttp2_server_set_default_response(intern);
}

ZEND_METHOD(Nghttp2_Server, setResponse)
{
    zend_long status;
    zval *headers;
    zend_string *body;
    nghttp2_server_object *intern = Z_NGHTTP2_SERVER_OBJ_P(ZEND_THIS);
    zval normalized_headers;

    ZEND_PARSE_PARAMETERS_START(3, 3)
        Z_PARAM_LONG(status)
        Z_PARAM_ARRAY(headers)
        Z_PARAM_STR(body)
    ZEND_PARSE_PARAMETERS_END();

    if (status < 100 || status > 599) {
        zend_argument_value_error(1, "must be between 100 and 599");
        RETURN_THROWS();
    }

    if (nghttp2_headers_normalize(
            headers,
            &normalized_headers,
            NGHTTP2_HEADERS_NORMALIZE_ALLOW_ASSOC | NGHTTP2_HEADERS_NORMALIZE_FILTER_RESPONSE_RESERVED
        ) != SUCCESS) {
        RETURN_THROWS();
    }

    intern->response_status = status;
    if (Z_TYPE(intern->response_headers) != IS_UNDEF) {
        zval_ptr_dtor(&intern->response_headers);
    }
    ZVAL_COPY_VALUE(&intern->response_headers, &normalized_headers);

    if (intern->response_body != NULL) {
        zend_string_release(intern->response_body);
    }
    intern->response_body = zend_string_copy(body);
}

ZEND_METHOD(Nghttp2_Server, serveOnce)
{
    nghttp2_server_object *intern = Z_NGHTTP2_SERVER_OBJ_P(ZEND_THIS);
    struct sockaddr_in caddr;
    socklen_t clen = sizeof(caddr);
    int cfd;

    ZEND_PARSE_PARAMETERS_NONE();

    if (intern->listen_fd < 0 || intern->ssl_ctx == NULL) {
        nghttp2_throw_server_exception("server is closed", NGHTTP2_ERR_INVALID_STATE);
        RETURN_THROWS();
    }

    for (;;) {
        cfd = accept(intern->listen_fd, (struct sockaddr *)&caddr, &clen);
        if (cfd >= 0) {
            break;
        }
        if (errno == EINTR) {
            continue;
        }
        nghttp2_throw_server_exception("accept failed", NGHTTP2_ERR_CALLBACK_FAILURE);
        RETURN_THROWS();
    }

    nghttp2_server_handle_connection(intern, cfd);
}

ZEND_METHOD(Nghttp2_Server, serve)
{
    nghttp2_server_object *intern = Z_NGHTTP2_SERVER_OBJ_P(ZEND_THIS);
    struct sockaddr_in caddr;
    socklen_t clen;
    int cfd;

    ZEND_PARSE_PARAMETERS_NONE();

    if (intern->listen_fd < 0 || intern->ssl_ctx == NULL) {
        nghttp2_throw_server_exception("server is closed", NGHTTP2_ERR_INVALID_STATE);
        RETURN_THROWS();
    }

    for (;;) {
        clen = sizeof(caddr);
        cfd = accept(intern->listen_fd, (struct sockaddr *)&caddr, &clen);
        if (cfd < 0) {
            if (errno == EINTR) {
                continue;
            }
            nghttp2_throw_server_exception("accept failed", NGHTTP2_ERR_CALLBACK_FAILURE);
            RETURN_THROWS();
        }
        nghttp2_server_handle_connection(intern, cfd);
    }
}

ZEND_METHOD(Nghttp2_Server, close)
{
    nghttp2_server_object *intern = Z_NGHTTP2_SERVER_OBJ_P(ZEND_THIS);

    ZEND_PARSE_PARAMETERS_NONE();

    nghttp2_server_close_listener(intern);
}

static const zend_function_entry nghttp2_server_methods[] = {
    ZEND_ME(Nghttp2_Server, __construct, arginfo_server_construct, ZEND_ACC_PUBLIC)
    ZEND_ME(Nghttp2_Server, setResponse, arginfo_server_set_response, ZEND_ACC_PUBLIC)
    ZEND_ME(Nghttp2_Server, serveOnce, arginfo_server_serve, ZEND_ACC_PUBLIC)
    ZEND_ME(Nghttp2_Server, serve, arginfo_server_serve, ZEND_ACC_PUBLIC)
    ZEND_ME(Nghttp2_Server, close, arginfo_server_serve, ZEND_ACC_PUBLIC)
    ZEND_FE_END
};

void nghttp2_register_server_class(void)
{
    zend_class_entry ce;

    INIT_NS_CLASS_ENTRY(ce, "Nghttp2", "Server", nghttp2_server_methods);
    nghttp2_ce_server = zend_register_internal_class(&ce);
    nghttp2_ce_server->create_object = nghttp2_server_create_object;
    nghttp2_ce_server->ce_flags |= ZEND_ACC_FINAL;

    memcpy(&nghttp2_server_object_handlers, &std_object_handlers, sizeof(zend_object_handlers));
    nghttp2_server_object_handlers.offset = XtOffsetOf(nghttp2_server_object, std);
    nghttp2_server_object_handlers.free_obj = nghttp2_server_free_object;
}
