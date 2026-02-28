#include "php_nghttp2.h"

#include <Zend/zend_smart_str.h>
#include <string.h>

typedef struct _nghttp2_session_object {
    nghttp2_session *session;
    zend_bool is_server;
    zend_bool closed;
    smart_str outbound;
    zval events;
    zend_object std;
} nghttp2_session_object;

zend_class_entry *nghttp2_ce_session;
static zend_object_handlers nghttp2_session_object_handlers;

static inline nghttp2_session_object *nghttp2_session_from_obj(zend_object *obj)
{
    return (nghttp2_session_object *)((char *)(obj) - XtOffsetOf(nghttp2_session_object, std));
}

#define Z_NGHTTP2_SESSION_OBJ_P(zv) nghttp2_session_from_obj(Z_OBJ_P((zv)))

static void nghttp2_session_reset_events(nghttp2_session_object *intern)
{
    if (Z_TYPE(intern->events) != IS_UNDEF) {
        zval_ptr_dtor(&intern->events);
    }

    array_init(&intern->events);
}

static void nghttp2_session_reset_outbound(nghttp2_session_object *intern)
{
    smart_str_free(&intern->outbound);
}

static void nghttp2_session_release(nghttp2_session_object *intern, zend_bool reinitialize_state)
{
    if (intern->session != NULL) {
        nghttp2_session_del(intern->session);
        intern->session = NULL;
    }

    nghttp2_session_reset_outbound(intern);

    if (reinitialize_state) {
        nghttp2_session_reset_events(intern);
    } else if (Z_TYPE(intern->events) != IS_UNDEF) {
        zval_ptr_dtor(&intern->events);
        ZVAL_UNDEF(&intern->events);
    }

    intern->closed = 1;
}

static ssize_t nghttp2_session_send_cb(
    nghttp2_session *session,
    const uint8_t *data,
    size_t length,
    int flags,
    void *user_data
)
{
    nghttp2_session_object *intern = (nghttp2_session_object *)user_data;

    (void)session;
    (void)flags;

    smart_str_appendl(&intern->outbound, (const char *)data, length);
    return (ssize_t)length;
}

static int nghttp2_session_flush_outbound(nghttp2_session_object *intern)
{
    int rv;

    rv = nghttp2_session_send(intern->session);
    if (rv != 0) {
        nghttp2_throw_session_exception(nghttp2_strerror(rv), rv);
        return FAILURE;
    }

    return SUCCESS;
}

static int nghttp2_session_initialize(nghttp2_session_object *intern, zend_bool is_server)
{
    nghttp2_session_callbacks *cbs = NULL;
    int rv;

    rv = nghttp2_session_callbacks_new(&cbs);
    if (rv != 0) {
        nghttp2_throw_session_exception(nghttp2_strerror(rv), rv);
        return FAILURE;
    }

    nghttp2_session_callbacks_set_send_callback(cbs, nghttp2_session_send_cb);

    if (is_server) {
        rv = nghttp2_session_server_new(&intern->session, cbs, intern);
    } else {
        rv = nghttp2_session_client_new(&intern->session, cbs, intern);
    }

    nghttp2_session_callbacks_del(cbs);
    if (rv != 0) {
        intern->session = NULL;
        nghttp2_throw_session_exception(nghttp2_strerror(rv), rv);
        return FAILURE;
    }

    intern->is_server = is_server;
    intern->closed = 0;
    nghttp2_session_reset_events(intern);
    nghttp2_session_reset_outbound(intern);

    rv = nghttp2_submit_settings(intern->session, NGHTTP2_FLAG_NONE, NULL, 0);
    if (rv != 0) {
        nghttp2_session_release(intern, 1);
        nghttp2_throw_session_exception(nghttp2_strerror(rv), rv);
        return FAILURE;
    }

    if (nghttp2_session_flush_outbound(intern) != SUCCESS) {
        nghttp2_session_release(intern, 1);
        return FAILURE;
    }

    return SUCCESS;
}

static int nghttp2_session_require_open(nghttp2_session_object *intern)
{
    if (intern->closed || intern->session == NULL) {
        nghttp2_throw_session_exception("session is closed", NGHTTP2_ERR_INVALID_STATE);
        return FAILURE;
    }

    return SUCCESS;
}

static void nghttp2_session_throw_not_implemented(void)
{
    nghttp2_throw_session_exception("method is not implemented yet", NGHTTP2_ERR_INVALID_STATE);
}

static zend_object *nghttp2_session_create_object(zend_class_entry *ce)
{
    nghttp2_session_object *intern = zend_object_alloc(sizeof(nghttp2_session_object), ce);

    intern->session = NULL;
    intern->is_server = 0;
    intern->closed = 1;
    memset(&intern->outbound, 0, sizeof(intern->outbound));
    array_init(&intern->events);

    zend_object_std_init(&intern->std, ce);
    object_properties_init(&intern->std, ce);
    intern->std.handlers = &nghttp2_session_object_handlers;

    return &intern->std;
}

static void nghttp2_session_free_object(zend_object *object)
{
    nghttp2_session_object *intern = nghttp2_session_from_obj(object);

    nghttp2_session_release(intern, 0);
    zend_object_std_dtor(&intern->std);
}

ZEND_BEGIN_ARG_INFO_EX(arginfo_session_construct, 0, 0, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_session_factory, 0, 0, IS_OBJECT, 0)
    ZEND_ARG_ARRAY_INFO(0, options, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_session_receive, 0, 1, IS_LONG, 0)
    ZEND_ARG_TYPE_INFO(0, bytes, IS_STRING, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_session_pop_outbound, 0, 0, IS_STRING, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_session_pop_events, 0, 0, IS_ARRAY, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_session_bool, 0, 0, _IS_BOOL, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_session_close, 0, 0, IS_VOID, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_session_submit_request, 0, 1, IS_LONG, 0)
    ZEND_ARG_ARRAY_INFO(0, headers, 0)
    ZEND_ARG_TYPE_INFO(0, endStream, _IS_BOOL, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_session_submit_headers, 0, 2, IS_VOID, 0)
    ZEND_ARG_TYPE_INFO(0, streamId, IS_LONG, 0)
    ZEND_ARG_ARRAY_INFO(0, headers, 0)
    ZEND_ARG_TYPE_INFO(0, endStream, _IS_BOOL, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_session_submit_data, 0, 2, IS_VOID, 0)
    ZEND_ARG_TYPE_INFO(0, streamId, IS_LONG, 0)
    ZEND_ARG_TYPE_INFO(0, data, IS_STRING, 0)
    ZEND_ARG_TYPE_INFO(0, endStream, _IS_BOOL, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_session_submit_settings, 0, 1, IS_VOID, 0)
    ZEND_ARG_ARRAY_INFO(0, settings, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_session_submit_ping, 0, 0, IS_VOID, 0)
    ZEND_ARG_TYPE_INFO(0, opaqueData, IS_STRING, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_session_submit_rst_stream, 0, 2, IS_VOID, 0)
    ZEND_ARG_TYPE_INFO(0, streamId, IS_LONG, 0)
    ZEND_ARG_TYPE_INFO(0, errorCode, IS_LONG, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_session_submit_goaway, 0, 0, IS_VOID, 0)
    ZEND_ARG_TYPE_INFO(0, errorCode, IS_LONG, 0)
    ZEND_ARG_TYPE_INFO(0, debugData, IS_STRING, 0)
ZEND_END_ARG_INFO()

ZEND_METHOD(Nghttp2_Session, __construct)
{
    ZEND_PARSE_PARAMETERS_NONE();
}

ZEND_METHOD(Nghttp2_Session, client)
{
    zval *options = NULL;
    nghttp2_session_object *intern;

    ZEND_PARSE_PARAMETERS_START(0, 1)
        Z_PARAM_OPTIONAL
        Z_PARAM_ARRAY(options)
    ZEND_PARSE_PARAMETERS_END();

    (void)options;

    object_init_ex(return_value, nghttp2_ce_session);
    intern = Z_NGHTTP2_SESSION_OBJ_P(return_value);

    if (nghttp2_session_initialize(intern, 0) != SUCCESS) {
        zval_ptr_dtor(return_value);
        RETURN_THROWS();
    }
}

ZEND_METHOD(Nghttp2_Session, server)
{
    zval *options = NULL;
    nghttp2_session_object *intern;

    ZEND_PARSE_PARAMETERS_START(0, 1)
        Z_PARAM_OPTIONAL
        Z_PARAM_ARRAY(options)
    ZEND_PARSE_PARAMETERS_END();

    (void)options;

    object_init_ex(return_value, nghttp2_ce_session);
    intern = Z_NGHTTP2_SESSION_OBJ_P(return_value);

    if (nghttp2_session_initialize(intern, 1) != SUCCESS) {
        zval_ptr_dtor(return_value);
        RETURN_THROWS();
    }
}

ZEND_METHOD(Nghttp2_Session, receive)
{
    zend_string *bytes;
    ssize_t rv;
    nghttp2_session_object *intern = Z_NGHTTP2_SESSION_OBJ_P(ZEND_THIS);

    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_STR(bytes)
    ZEND_PARSE_PARAMETERS_END();

    if (nghttp2_session_require_open(intern) != SUCCESS) {
        RETURN_THROWS();
    }

    if (ZSTR_LEN(bytes) == 0) {
        RETURN_LONG(0);
    }

    rv = nghttp2_session_mem_recv(intern->session, (const uint8_t *)ZSTR_VAL(bytes), ZSTR_LEN(bytes));
    if (rv < 0) {
        nghttp2_throw_session_exception(nghttp2_strerror((int)rv), (int)rv);
        RETURN_THROWS();
    }

    if (nghttp2_session_flush_outbound(intern) != SUCCESS) {
        RETURN_THROWS();
    }

    RETURN_LONG((zend_long)rv);
}

ZEND_METHOD(Nghttp2_Session, popOutbound)
{
    nghttp2_session_object *intern = Z_NGHTTP2_SESSION_OBJ_P(ZEND_THIS);

    ZEND_PARSE_PARAMETERS_NONE();

    if (intern->outbound.s == NULL) {
        RETURN_EMPTY_STRING();
    }

    RETVAL_STR_COPY(intern->outbound.s);
    nghttp2_session_reset_outbound(intern);
}

ZEND_METHOD(Nghttp2_Session, popEvents)
{
    nghttp2_session_object *intern = Z_NGHTTP2_SESSION_OBJ_P(ZEND_THIS);

    ZEND_PARSE_PARAMETERS_NONE();

    ZVAL_COPY(return_value, &intern->events);
    nghttp2_session_reset_events(intern);
}

ZEND_METHOD(Nghttp2_Session, wantsRead)
{
    nghttp2_session_object *intern = Z_NGHTTP2_SESSION_OBJ_P(ZEND_THIS);

    ZEND_PARSE_PARAMETERS_NONE();

    if (intern->closed || intern->session == NULL) {
        RETURN_FALSE;
    }

    RETURN_BOOL(nghttp2_session_want_read(intern->session) != 0);
}

ZEND_METHOD(Nghttp2_Session, wantsWrite)
{
    nghttp2_session_object *intern = Z_NGHTTP2_SESSION_OBJ_P(ZEND_THIS);

    ZEND_PARSE_PARAMETERS_NONE();

    if (intern->closed || intern->session == NULL) {
        RETURN_FALSE;
    }

    RETURN_BOOL(nghttp2_session_want_write(intern->session) != 0);
}

ZEND_METHOD(Nghttp2_Session, submitRequest)
{
    zval *headers;
    zend_bool end_stream = 1;
    nghttp2_session_object *intern = Z_NGHTTP2_SESSION_OBJ_P(ZEND_THIS);

    ZEND_PARSE_PARAMETERS_START(1, 2)
        Z_PARAM_ARRAY(headers)
        Z_PARAM_OPTIONAL
        Z_PARAM_BOOL(end_stream)
    ZEND_PARSE_PARAMETERS_END();

    if (nghttp2_session_require_open(intern) != SUCCESS) {
        RETURN_THROWS();
    }

    (void)headers;
    (void)end_stream;
    nghttp2_session_throw_not_implemented();
    RETURN_THROWS();
}

ZEND_METHOD(Nghttp2_Session, submitResponse)
{
    zend_long stream_id;
    zval *headers;
    zend_bool end_stream = 0;
    nghttp2_session_object *intern = Z_NGHTTP2_SESSION_OBJ_P(ZEND_THIS);

    ZEND_PARSE_PARAMETERS_START(2, 3)
        Z_PARAM_LONG(stream_id)
        Z_PARAM_ARRAY(headers)
        Z_PARAM_OPTIONAL
        Z_PARAM_BOOL(end_stream)
    ZEND_PARSE_PARAMETERS_END();

    if (nghttp2_session_require_open(intern) != SUCCESS) {
        RETURN_THROWS();
    }

    (void)stream_id;
    (void)headers;
    (void)end_stream;
    nghttp2_session_throw_not_implemented();
    RETURN_THROWS();
}

ZEND_METHOD(Nghttp2_Session, submitHeaders)
{
    zend_long stream_id;
    zval *headers;
    zend_bool end_stream = 0;
    nghttp2_session_object *intern = Z_NGHTTP2_SESSION_OBJ_P(ZEND_THIS);

    ZEND_PARSE_PARAMETERS_START(2, 3)
        Z_PARAM_LONG(stream_id)
        Z_PARAM_ARRAY(headers)
        Z_PARAM_OPTIONAL
        Z_PARAM_BOOL(end_stream)
    ZEND_PARSE_PARAMETERS_END();

    if (nghttp2_session_require_open(intern) != SUCCESS) {
        RETURN_THROWS();
    }

    (void)stream_id;
    (void)headers;
    (void)end_stream;
    nghttp2_session_throw_not_implemented();
    RETURN_THROWS();
}

ZEND_METHOD(Nghttp2_Session, submitData)
{
    zend_long stream_id;
    zend_string *data;
    zend_bool end_stream = 0;
    nghttp2_session_object *intern = Z_NGHTTP2_SESSION_OBJ_P(ZEND_THIS);

    ZEND_PARSE_PARAMETERS_START(2, 3)
        Z_PARAM_LONG(stream_id)
        Z_PARAM_STR(data)
        Z_PARAM_OPTIONAL
        Z_PARAM_BOOL(end_stream)
    ZEND_PARSE_PARAMETERS_END();

    if (nghttp2_session_require_open(intern) != SUCCESS) {
        RETURN_THROWS();
    }

    (void)stream_id;
    (void)data;
    (void)end_stream;
    nghttp2_session_throw_not_implemented();
    RETURN_THROWS();
}

ZEND_METHOD(Nghttp2_Session, submitSettings)
{
    zval *settings;
    nghttp2_session_object *intern = Z_NGHTTP2_SESSION_OBJ_P(ZEND_THIS);

    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_ARRAY(settings)
    ZEND_PARSE_PARAMETERS_END();

    if (nghttp2_session_require_open(intern) != SUCCESS) {
        RETURN_THROWS();
    }

    (void)settings;
    nghttp2_session_throw_not_implemented();
    RETURN_THROWS();
}

ZEND_METHOD(Nghttp2_Session, submitPing)
{
    zend_string *opaque_data = NULL;
    nghttp2_session_object *intern = Z_NGHTTP2_SESSION_OBJ_P(ZEND_THIS);

    ZEND_PARSE_PARAMETERS_START(0, 1)
        Z_PARAM_OPTIONAL
        Z_PARAM_STR(opaque_data)
    ZEND_PARSE_PARAMETERS_END();

    if (nghttp2_session_require_open(intern) != SUCCESS) {
        RETURN_THROWS();
    }

    (void)opaque_data;
    nghttp2_session_throw_not_implemented();
    RETURN_THROWS();
}

ZEND_METHOD(Nghttp2_Session, submitRstStream)
{
    zend_long stream_id;
    zend_long error_code;
    nghttp2_session_object *intern = Z_NGHTTP2_SESSION_OBJ_P(ZEND_THIS);

    ZEND_PARSE_PARAMETERS_START(2, 2)
        Z_PARAM_LONG(stream_id)
        Z_PARAM_LONG(error_code)
    ZEND_PARSE_PARAMETERS_END();

    if (nghttp2_session_require_open(intern) != SUCCESS) {
        RETURN_THROWS();
    }

    (void)stream_id;
    (void)error_code;
    nghttp2_session_throw_not_implemented();
    RETURN_THROWS();
}

ZEND_METHOD(Nghttp2_Session, submitGoaway)
{
    zend_long error_code = 0;
    zend_string *debug_data = NULL;
    nghttp2_session_object *intern = Z_NGHTTP2_SESSION_OBJ_P(ZEND_THIS);

    ZEND_PARSE_PARAMETERS_START(0, 2)
        Z_PARAM_OPTIONAL
        Z_PARAM_LONG(error_code)
        Z_PARAM_STR(debug_data)
    ZEND_PARSE_PARAMETERS_END();

    if (nghttp2_session_require_open(intern) != SUCCESS) {
        RETURN_THROWS();
    }

    (void)error_code;
    (void)debug_data;
    nghttp2_session_throw_not_implemented();
    RETURN_THROWS();
}

ZEND_METHOD(Nghttp2_Session, close)
{
    nghttp2_session_object *intern = Z_NGHTTP2_SESSION_OBJ_P(ZEND_THIS);

    ZEND_PARSE_PARAMETERS_NONE();

    nghttp2_session_release(intern, 1);
}

static const zend_function_entry nghttp2_session_methods[] = {
    ZEND_ME(Nghttp2_Session, __construct, arginfo_session_construct, ZEND_ACC_PRIVATE)
    ZEND_ME(Nghttp2_Session, client, arginfo_session_factory, ZEND_ACC_PUBLIC | ZEND_ACC_STATIC)
    ZEND_ME(Nghttp2_Session, server, arginfo_session_factory, ZEND_ACC_PUBLIC | ZEND_ACC_STATIC)
    ZEND_ME(Nghttp2_Session, receive, arginfo_session_receive, ZEND_ACC_PUBLIC)
    ZEND_ME(Nghttp2_Session, popOutbound, arginfo_session_pop_outbound, ZEND_ACC_PUBLIC)
    ZEND_ME(Nghttp2_Session, popEvents, arginfo_session_pop_events, ZEND_ACC_PUBLIC)
    ZEND_ME(Nghttp2_Session, wantsRead, arginfo_session_bool, ZEND_ACC_PUBLIC)
    ZEND_ME(Nghttp2_Session, wantsWrite, arginfo_session_bool, ZEND_ACC_PUBLIC)
    ZEND_ME(Nghttp2_Session, submitRequest, arginfo_session_submit_request, ZEND_ACC_PUBLIC)
    ZEND_ME(Nghttp2_Session, submitResponse, arginfo_session_submit_headers, ZEND_ACC_PUBLIC)
    ZEND_ME(Nghttp2_Session, submitHeaders, arginfo_session_submit_headers, ZEND_ACC_PUBLIC)
    ZEND_ME(Nghttp2_Session, submitData, arginfo_session_submit_data, ZEND_ACC_PUBLIC)
    ZEND_ME(Nghttp2_Session, submitSettings, arginfo_session_submit_settings, ZEND_ACC_PUBLIC)
    ZEND_ME(Nghttp2_Session, submitPing, arginfo_session_submit_ping, ZEND_ACC_PUBLIC)
    ZEND_ME(Nghttp2_Session, submitRstStream, arginfo_session_submit_rst_stream, ZEND_ACC_PUBLIC)
    ZEND_ME(Nghttp2_Session, submitGoaway, arginfo_session_submit_goaway, ZEND_ACC_PUBLIC)
    ZEND_ME(Nghttp2_Session, close, arginfo_session_close, ZEND_ACC_PUBLIC)
    ZEND_FE_END
};

void nghttp2_register_session_class(void)
{
    zend_class_entry ce;

    INIT_NS_CLASS_ENTRY(ce, "Nghttp2", "Session", nghttp2_session_methods);
    nghttp2_ce_session = zend_register_internal_class(&ce);
    nghttp2_ce_session->create_object = nghttp2_session_create_object;
    nghttp2_ce_session->ce_flags |= ZEND_ACC_FINAL;

    memcpy(&nghttp2_session_object_handlers, &std_object_handlers, sizeof(zend_object_handlers));
    nghttp2_session_object_handlers.offset = XtOffsetOf(nghttp2_session_object, std);
    nghttp2_session_object_handlers.free_obj = nghttp2_session_free_object;
}
