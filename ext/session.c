#include "php_nghttp2.h"

#include <Zend/zend_smart_str.h>
#include <inttypes.h>
#include <string.h>

typedef struct _nghttp2_session_stream_state {
    int32_t stream_id;
    zval headers;
    zend_bool collecting_headers;
    uint8_t headers_category;
    zend_string *outgoing_data;
    size_t outgoing_data_offset;
} nghttp2_session_stream_state;

typedef struct _nghttp2_session_object {
    nghttp2_session *session;
    zend_bool is_server;
    zend_bool closed;
    smart_str outbound;
    zval events;
    HashTable stream_states;
    zend_bool stream_states_initialized;
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

static void nghttp2_session_stream_state_reset_headers(nghttp2_session_stream_state *state)
{
    if (Z_TYPE(state->headers) != IS_UNDEF) {
        zval_ptr_dtor(&state->headers);
        ZVAL_UNDEF(&state->headers);
    }

    state->collecting_headers = 0;
    state->headers_category = NGHTTP2_HCAT_HEADERS;
}

static void nghttp2_session_stream_state_reset_outgoing_data(nghttp2_session_stream_state *state)
{
    if (state->outgoing_data != NULL) {
        zend_string_release(state->outgoing_data);
        state->outgoing_data = NULL;
    }

    state->outgoing_data_offset = 0;
}

static void nghttp2_session_stream_state_free(nghttp2_session_stream_state *state)
{
    nghttp2_session_stream_state_reset_headers(state);
    nghttp2_session_stream_state_reset_outgoing_data(state);
    efree(state);
}

static void nghttp2_session_clear_stream_states(nghttp2_session_object *intern)
{
    nghttp2_session_stream_state *state;

    if (!intern->stream_states_initialized) {
        return;
    }

    ZEND_HASH_FOREACH_PTR(&intern->stream_states, state) {
        nghttp2_session_stream_state_free(state);
    } ZEND_HASH_FOREACH_END();

    zend_hash_clean(&intern->stream_states);
}

static void nghttp2_session_destroy_stream_states(nghttp2_session_object *intern)
{
    if (!intern->stream_states_initialized) {
        return;
    }

    nghttp2_session_clear_stream_states(intern);
    zend_hash_destroy(&intern->stream_states);
    intern->stream_states_initialized = 0;
}

static nghttp2_session_stream_state *nghttp2_session_stream_state_get(
    nghttp2_session_object *intern,
    int32_t stream_id,
    zend_bool create
)
{
    nghttp2_session_stream_state *state;

    state = zend_hash_index_find_ptr(&intern->stream_states, (zend_ulong)stream_id);
    if (state != NULL || !create) {
        return state;
    }

    state = ecalloc(1, sizeof(*state));
    state->stream_id = stream_id;
    ZVAL_UNDEF(&state->headers);
    state->collecting_headers = 0;
    state->headers_category = NGHTTP2_HCAT_HEADERS;
    state->outgoing_data = NULL;
    state->outgoing_data_offset = 0;

    zend_hash_index_add_ptr(&intern->stream_states, (zend_ulong)stream_id, state);
    return state;
}

static void nghttp2_session_stream_state_remove(nghttp2_session_object *intern, int32_t stream_id)
{
    nghttp2_session_stream_state *state;

    state = zend_hash_index_find_ptr(&intern->stream_states, (zend_ulong)stream_id);
    if (state == NULL) {
        return;
    }

    zend_hash_index_del(&intern->stream_states, (zend_ulong)stream_id);
    nghttp2_session_stream_state_free(state);
}

static const char *nghttp2_session_headers_category_name(uint8_t category)
{
    switch (category) {
        case NGHTTP2_HCAT_REQUEST:
            return "request";
        case NGHTTP2_HCAT_RESPONSE:
            return "response";
        case NGHTTP2_HCAT_PUSH_RESPONSE:
            return "push_response";
        case NGHTTP2_HCAT_HEADERS:
        default:
            return "headers";
    }
}

static void nghttp2_session_queue_headers_event(
    nghttp2_session_object *intern,
    nghttp2_session_stream_state *state,
    zend_bool end_stream
)
{
    zval event;
    zval headers;

    array_init(&event);
    add_assoc_string(&event, "type", "headers");
    add_assoc_long(&event, "streamId", state->stream_id);
    add_assoc_string(&event, "category", (char *)nghttp2_session_headers_category_name(state->headers_category));
    add_assoc_bool(&event, "endStream", end_stream);

    ZVAL_COPY(&headers, &state->headers);
    add_assoc_zval(&event, "headers", &headers);

    add_next_index_zval(&intern->events, &event);
}

static void nghttp2_session_queue_data_event(
    nghttp2_session_object *intern,
    int32_t stream_id,
    const uint8_t *data,
    size_t len
)
{
    zval event;

    array_init(&event);
    add_assoc_string(&event, "type", "data");
    add_assoc_long(&event, "streamId", stream_id);
    add_assoc_stringl(&event, "data", (const char *)data, len);

    add_next_index_zval(&intern->events, &event);
}

static void nghttp2_session_queue_stream_close_event(
    nghttp2_session_object *intern,
    int32_t stream_id,
    uint32_t error_code
)
{
    zval event;

    array_init(&event);
    add_assoc_string(&event, "type", "stream_close");
    add_assoc_long(&event, "streamId", stream_id);
    add_assoc_long(&event, "errorCode", (zend_long)error_code);

    add_next_index_zval(&intern->events, &event);
}

static void nghttp2_session_queue_settings_event(
    nghttp2_session_object *intern,
    const nghttp2_settings *settings_frame
)
{
    zval event;
    zval settings;
    size_t i;

    array_init(&event);
    add_assoc_string(&event, "type", "settings");
    add_assoc_bool(&event, "ack", (settings_frame->hd.flags & NGHTTP2_FLAG_ACK) != 0);

    array_init(&settings);
    for (i = 0; i < settings_frame->niv; i++) {
        zval entry;

        array_init(&entry);
        add_assoc_long(&entry, "id", settings_frame->iv[i].settings_id);
        add_assoc_long(&entry, "value", settings_frame->iv[i].value);
        add_next_index_zval(&settings, &entry);
    }

    add_assoc_zval(&event, "settings", &settings);
    add_next_index_zval(&intern->events, &event);
}

static void nghttp2_session_queue_ping_event(
    nghttp2_session_object *intern,
    const nghttp2_ping *ping_frame
)
{
    zval event;

    array_init(&event);
    add_assoc_string(&event, "type", "ping");
    add_assoc_bool(&event, "ack", (ping_frame->hd.flags & NGHTTP2_FLAG_ACK) != 0);
    add_assoc_stringl(&event, "opaqueData", (const char *)ping_frame->opaque_data, sizeof(ping_frame->opaque_data));

    add_next_index_zval(&intern->events, &event);
}

static void nghttp2_session_queue_goaway_event(
    nghttp2_session_object *intern,
    const nghttp2_goaway *goaway_frame
)
{
    zval event;

    array_init(&event);
    add_assoc_string(&event, "type", "goaway");
    add_assoc_long(&event, "lastStreamId", goaway_frame->last_stream_id);
    add_assoc_long(&event, "errorCode", (zend_long)goaway_frame->error_code);
    add_assoc_stringl(&event, "debugData", (const char *)goaway_frame->opaque_data, goaway_frame->opaque_data_len);

    add_next_index_zval(&intern->events, &event);
}

static void nghttp2_session_queue_rst_stream_event(
    nghttp2_session_object *intern,
    const nghttp2_rst_stream *rst_stream_frame
)
{
    zval event;

    array_init(&event);
    add_assoc_string(&event, "type", "rst_stream");
    add_assoc_long(&event, "streamId", rst_stream_frame->hd.stream_id);
    add_assoc_long(&event, "errorCode", (zend_long)rst_stream_frame->error_code);

    add_next_index_zval(&intern->events, &event);
}

static void nghttp2_session_release(nghttp2_session_object *intern, zend_bool reinitialize_state)
{
    if (intern->session != NULL) {
        nghttp2_session_del(intern->session);
        intern->session = NULL;
    }

    nghttp2_session_reset_outbound(intern);
    nghttp2_session_clear_stream_states(intern);

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

static int nghttp2_session_on_begin_headers_cb(
    nghttp2_session *session,
    const nghttp2_frame *frame,
    void *user_data
)
{
    nghttp2_session_object *intern = (nghttp2_session_object *)user_data;
    nghttp2_session_stream_state *state;

    (void)session;

    if (frame->hd.type != NGHTTP2_HEADERS || frame->hd.stream_id <= 0) {
        return 0;
    }

    state = nghttp2_session_stream_state_get(intern, frame->hd.stream_id, 1);
    if (state == NULL) {
        return NGHTTP2_ERR_CALLBACK_FAILURE;
    }

    nghttp2_session_stream_state_reset_headers(state);
    array_init(&state->headers);
    state->collecting_headers = 1;
    state->headers_category = frame->headers.cat;

    nghttp2_session_set_stream_user_data(session, frame->hd.stream_id, state);
    return 0;
}

static int nghttp2_session_on_header_cb(
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
    nghttp2_session_object *intern = (nghttp2_session_object *)user_data;
    nghttp2_session_stream_state *state;
    zval header;
    zval name_zv;
    zval value_zv;

    (void)session;
    (void)flags;

    if (frame->hd.type != NGHTTP2_HEADERS || frame->hd.stream_id <= 0) {
        return 0;
    }

    state = nghttp2_session_stream_state_get(intern, frame->hd.stream_id, 1);
    if (state == NULL || !state->collecting_headers) {
        return 0;
    }

    array_init(&header);
    ZVAL_STRINGL(&name_zv, (const char *)name, namelen);
    ZVAL_STRINGL(&value_zv, (const char *)value, valuelen);
    zend_hash_str_update(Z_ARRVAL(header), "name", sizeof("name") - 1, &name_zv);
    zend_hash_str_update(Z_ARRVAL(header), "value", sizeof("value") - 1, &value_zv);
    add_next_index_zval(&state->headers, &header);

    return 0;
}

static int nghttp2_session_on_frame_recv_cb(
    nghttp2_session *session,
    const nghttp2_frame *frame,
    void *user_data
)
{
    nghttp2_session_object *intern = (nghttp2_session_object *)user_data;
    nghttp2_session_stream_state *state;

    (void)session;

    switch (frame->hd.type) {
        case NGHTTP2_HEADERS:
            if (frame->hd.stream_id <= 0) {
                return 0;
            }

            state = nghttp2_session_stream_state_get(intern, frame->hd.stream_id, 0);
            if (state == NULL || !state->collecting_headers) {
                return 0;
            }

            nghttp2_session_queue_headers_event(
                intern,
                state,
                (frame->hd.flags & NGHTTP2_FLAG_END_STREAM) != 0
            );
            nghttp2_session_stream_state_reset_headers(state);
            break;
        case NGHTTP2_SETTINGS:
            nghttp2_session_queue_settings_event(intern, &frame->settings);
            break;
        case NGHTTP2_PING:
            nghttp2_session_queue_ping_event(intern, &frame->ping);
            break;
        case NGHTTP2_GOAWAY:
            nghttp2_session_queue_goaway_event(intern, &frame->goaway);
            break;
        case NGHTTP2_RST_STREAM:
            nghttp2_session_queue_rst_stream_event(intern, &frame->rst_stream);
            break;
        default:
            break;
    }

    return 0;
}

static int nghttp2_session_on_data_chunk_recv_cb(
    nghttp2_session *session,
    uint8_t flags,
    int32_t stream_id,
    const uint8_t *data,
    size_t len,
    void *user_data
)
{
    nghttp2_session_object *intern = (nghttp2_session_object *)user_data;

    (void)session;
    (void)flags;

    nghttp2_session_queue_data_event(intern, stream_id, data, len);
    return 0;
}

static int nghttp2_session_on_stream_close_cb(
    nghttp2_session *session,
    int32_t stream_id,
    uint32_t error_code,
    void *user_data
)
{
    nghttp2_session_object *intern = (nghttp2_session_object *)user_data;

    (void)session;

    nghttp2_session_queue_stream_close_event(intern, stream_id, error_code);
    nghttp2_session_stream_state_remove(intern, stream_id);
    return 0;
}

static ssize_t nghttp2_session_data_read_cb(
    nghttp2_session *session,
    int32_t stream_id,
    uint8_t *buf,
    size_t length,
    uint32_t *data_flags,
    nghttp2_data_source *source,
    void *user_data
)
{
    nghttp2_session_stream_state *state = (nghttp2_session_stream_state *)source->ptr;
    size_t remain;
    size_t ncopy;

    (void)session;
    (void)stream_id;
    (void)user_data;

    if (state == NULL || state->outgoing_data == NULL) {
        *data_flags |= NGHTTP2_DATA_FLAG_EOF;
        return 0;
    }

    remain = ZSTR_LEN(state->outgoing_data) - state->outgoing_data_offset;
    ncopy = remain < length ? remain : length;
    if (ncopy > 0) {
        memcpy(buf, ZSTR_VAL(state->outgoing_data) + state->outgoing_data_offset, ncopy);
        state->outgoing_data_offset += ncopy;
    }

    if (state->outgoing_data_offset >= ZSTR_LEN(state->outgoing_data)) {
        *data_flags |= NGHTTP2_DATA_FLAG_EOF;
        nghttp2_session_stream_state_reset_outgoing_data(state);
    }

    return (ssize_t)ncopy;
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
    nghttp2_session_callbacks_set_on_begin_headers_callback(cbs, nghttp2_session_on_begin_headers_cb);
    nghttp2_session_callbacks_set_on_header_callback(cbs, nghttp2_session_on_header_cb);
    nghttp2_session_callbacks_set_on_frame_recv_callback(cbs, nghttp2_session_on_frame_recv_cb);
    nghttp2_session_callbacks_set_on_data_chunk_recv_callback(cbs, nghttp2_session_on_data_chunk_recv_cb);
    nghttp2_session_callbacks_set_on_stream_close_callback(cbs, nghttp2_session_on_stream_close_cb);

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
    nghttp2_session_clear_stream_states(intern);

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

static int nghttp2_session_build_nv_array(zval *headers, nghttp2_nv **nva_out, size_t *nvlen_out)
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

static int nghttp2_session_build_settings_entries(
    zval *settings,
    nghttp2_settings_entry **iv_out,
    size_t *niv_out
)
{
    HashTable *settings_ht;
    nghttp2_settings_entry *iv;
    zval *entry;
    size_t niv;
    size_t i = 0;

    settings_ht = Z_ARRVAL_P(settings);
    niv = zend_hash_num_elements(settings_ht);
    *niv_out = niv;
    if (niv == 0) {
        *iv_out = NULL;
        return SUCCESS;
    }

    iv = ecalloc(niv, sizeof(*iv));

    ZEND_HASH_FOREACH_VAL(settings_ht, entry) {
        zval *id;
        zval *value;
        zend_long id_long;
        zend_long value_long;

        if (Z_TYPE_P(entry) != IS_ARRAY) {
            efree(iv);
            zend_type_error("each setting must be an array with keys 'id' and 'value'");
            return FAILURE;
        }

        id = zend_hash_str_find(Z_ARRVAL_P(entry), "id", sizeof("id") - 1);
        value = zend_hash_str_find(Z_ARRVAL_P(entry), "value", sizeof("value") - 1);
        if (id == NULL || value == NULL) {
            efree(iv);
            zend_type_error("each setting must contain both 'id' and 'value'");
            return FAILURE;
        }

        id_long = zval_get_long(id);
        value_long = zval_get_long(value);

        if (id_long <= 0 || id_long > INT32_MAX) {
            efree(iv);
            zend_value_error("setting 'id' must be between 1 and %d", INT32_MAX);
            return FAILURE;
        }
        if (value_long < 0) {
            efree(iv);
            zend_value_error("setting 'value' must be greater than or equal to 0");
            return FAILURE;
        }
        if ((uint64_t)value_long > UINT32_MAX) {
            efree(iv);
            zend_value_error("setting 'value' must be less than or equal to %" PRIu32, UINT32_MAX);
            return FAILURE;
        }

        iv[i].settings_id = (int32_t)id_long;
        iv[i].value = (uint32_t)value_long;
        i++;
    } ZEND_HASH_FOREACH_END();

    *iv_out = iv;
    return SUCCESS;
}

static int nghttp2_session_submit_headers_internal(
    nghttp2_session_object *intern,
    zend_long stream_id,
    zval *headers,
    zend_bool end_stream
)
{
    nghttp2_nv *nva = NULL;
    size_t nvlen = 0;
    int rv;
    nghttp2_session_stream_state *state;

    if (stream_id <= 0) {
        nghttp2_throw_session_exception("streamId must be greater than 0", NGHTTP2_ERR_INVALID_ARGUMENT);
        return FAILURE;
    }

    if (nghttp2_session_build_nv_array(headers, &nva, &nvlen) != SUCCESS) {
        return FAILURE;
    }

    rv = nghttp2_submit_headers(
        intern->session,
        end_stream ? NGHTTP2_FLAG_END_STREAM : NGHTTP2_FLAG_NONE,
        (int32_t)stream_id,
        NULL,
        nva,
        nvlen,
        NULL
    );
    if (nva != NULL) {
        efree(nva);
    }

    if (rv != 0) {
        nghttp2_throw_session_exception(nghttp2_strerror(rv), rv);
        return FAILURE;
    }

    state = nghttp2_session_stream_state_get(intern, (int32_t)stream_id, 1);
    if (state != NULL) {
        nghttp2_session_set_stream_user_data(intern->session, (int32_t)stream_id, state);
    }

    if (nghttp2_session_flush_outbound(intern) != SUCCESS) {
        return FAILURE;
    }

    return SUCCESS;
}

static zend_object *nghttp2_session_create_object(zend_class_entry *ce)
{
    nghttp2_session_object *intern = zend_object_alloc(sizeof(nghttp2_session_object), ce);

    intern->session = NULL;
    intern->is_server = 0;
    intern->closed = 1;
    memset(&intern->outbound, 0, sizeof(intern->outbound));
    array_init(&intern->events);
    zend_hash_init(&intern->stream_states, 8, NULL, NULL, 0);
    intern->stream_states_initialized = 1;

    zend_object_std_init(&intern->std, ce);
    object_properties_init(&intern->std, ce);
    intern->std.handlers = &nghttp2_session_object_handlers;

    return &intern->std;
}

static void nghttp2_session_free_object(zend_object *object)
{
    nghttp2_session_object *intern = nghttp2_session_from_obj(object);

    nghttp2_session_release(intern, 0);
    nghttp2_session_destroy_stream_states(intern);
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
    nghttp2_nv *nva = NULL;
    size_t nvlen = 0;
    int32_t stream_id;
    nghttp2_session_stream_state *state;

    ZEND_PARSE_PARAMETERS_START(1, 2)
        Z_PARAM_ARRAY(headers)
        Z_PARAM_OPTIONAL
        Z_PARAM_BOOL(end_stream)
    ZEND_PARSE_PARAMETERS_END();

    if (nghttp2_session_require_open(intern) != SUCCESS) {
        RETURN_THROWS();
    }
    if (!end_stream) {
        nghttp2_throw_session_exception("request bodies are not implemented yet", NGHTTP2_ERR_INVALID_ARGUMENT);
        RETURN_THROWS();
    }

    if (nghttp2_session_build_nv_array(headers, &nva, &nvlen) != SUCCESS) {
        RETURN_THROWS();
    }

    stream_id = nghttp2_submit_request(intern->session, NULL, nva, nvlen, NULL, NULL);
    if (nva != NULL) {
        efree(nva);
    }

    if (stream_id < 0) {
        nghttp2_throw_session_exception(nghttp2_strerror(stream_id), stream_id);
        RETURN_THROWS();
    }

    state = nghttp2_session_stream_state_get(intern, stream_id, 1);
    if (state != NULL) {
        nghttp2_session_set_stream_user_data(intern->session, stream_id, state);
    }

    if (nghttp2_session_flush_outbound(intern) != SUCCESS) {
        RETURN_THROWS();
    }

    RETURN_LONG(stream_id);
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

    if (nghttp2_session_submit_headers_internal(intern, stream_id, headers, end_stream) != SUCCESS) {
        RETURN_THROWS();
    }
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

    if (nghttp2_session_submit_headers_internal(intern, stream_id, headers, end_stream) != SUCCESS) {
        RETURN_THROWS();
    }
}

ZEND_METHOD(Nghttp2_Session, submitData)
{
    zend_long stream_id;
    zend_string *data;
    zend_bool end_stream = 0;
    nghttp2_session_object *intern = Z_NGHTTP2_SESSION_OBJ_P(ZEND_THIS);
    nghttp2_session_stream_state *state;
    nghttp2_data_provider provider;
    int rv;

    ZEND_PARSE_PARAMETERS_START(2, 3)
        Z_PARAM_LONG(stream_id)
        Z_PARAM_STR(data)
        Z_PARAM_OPTIONAL
        Z_PARAM_BOOL(end_stream)
    ZEND_PARSE_PARAMETERS_END();

    if (nghttp2_session_require_open(intern) != SUCCESS) {
        RETURN_THROWS();
    }
    if (stream_id <= 0) {
        nghttp2_throw_session_exception("streamId must be greater than 0", NGHTTP2_ERR_INVALID_ARGUMENT);
        RETURN_THROWS();
    }

    state = nghttp2_session_stream_state_get(intern, (int32_t)stream_id, 0);
    if (state == NULL) {
        nghttp2_throw_session_exception("stream state not found", NGHTTP2_ERR_INVALID_STATE);
        RETURN_THROWS();
    }
    if (state->outgoing_data != NULL) {
        nghttp2_throw_session_exception("stream already has pending outbound data", NGHTTP2_ERR_INVALID_STATE);
        RETURN_THROWS();
    }

    state->outgoing_data = zend_string_copy(data);
    state->outgoing_data_offset = 0;

    memset(&provider, 0, sizeof(provider));
    provider.source.ptr = state;
    provider.read_callback = nghttp2_session_data_read_cb;

    rv = nghttp2_submit_data(
        intern->session,
        end_stream ? NGHTTP2_FLAG_END_STREAM : NGHTTP2_FLAG_NONE,
        (int32_t)stream_id,
        &provider
    );
    if (rv != 0) {
        nghttp2_session_stream_state_reset_outgoing_data(state);
        nghttp2_throw_session_exception(nghttp2_strerror(rv), rv);
        RETURN_THROWS();
    }

    if (nghttp2_session_flush_outbound(intern) != SUCCESS) {
        RETURN_THROWS();
    }
}

ZEND_METHOD(Nghttp2_Session, submitSettings)
{
    zval *settings;
    nghttp2_session_object *intern = Z_NGHTTP2_SESSION_OBJ_P(ZEND_THIS);
    nghttp2_settings_entry *iv = NULL;
    size_t niv = 0;
    int rv;

    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_ARRAY(settings)
    ZEND_PARSE_PARAMETERS_END();

    if (nghttp2_session_require_open(intern) != SUCCESS) {
        RETURN_THROWS();
    }

    if (nghttp2_session_build_settings_entries(settings, &iv, &niv) != SUCCESS) {
        RETURN_THROWS();
    }

    rv = nghttp2_submit_settings(intern->session, NGHTTP2_FLAG_NONE, iv, niv);
    if (iv != NULL) {
        efree(iv);
    }

    if (rv != 0) {
        nghttp2_throw_session_exception(nghttp2_strerror(rv), rv);
        RETURN_THROWS();
    }

    if (nghttp2_session_flush_outbound(intern) != SUCCESS) {
        RETURN_THROWS();
    }
}

ZEND_METHOD(Nghttp2_Session, submitPing)
{
    zend_string *opaque_data = NULL;
    nghttp2_session_object *intern = Z_NGHTTP2_SESSION_OBJ_P(ZEND_THIS);
    const uint8_t *payload = NULL;
    int rv;

    ZEND_PARSE_PARAMETERS_START(0, 1)
        Z_PARAM_OPTIONAL
        Z_PARAM_STR(opaque_data)
    ZEND_PARSE_PARAMETERS_END();

    if (nghttp2_session_require_open(intern) != SUCCESS) {
        RETURN_THROWS();
    }

    if (opaque_data != NULL) {
        if (ZSTR_LEN(opaque_data) != 8) {
            zend_argument_value_error(1, "must be exactly 8 bytes");
            RETURN_THROWS();
        }
        payload = (const uint8_t *)ZSTR_VAL(opaque_data);
    }

    rv = nghttp2_submit_ping(intern->session, NGHTTP2_FLAG_NONE, payload);
    if (rv != 0) {
        nghttp2_throw_session_exception(nghttp2_strerror(rv), rv);
        RETURN_THROWS();
    }

    if (nghttp2_session_flush_outbound(intern) != SUCCESS) {
        RETURN_THROWS();
    }
}

ZEND_METHOD(Nghttp2_Session, submitRstStream)
{
    zend_long stream_id;
    zend_long error_code;
    nghttp2_session_object *intern = Z_NGHTTP2_SESSION_OBJ_P(ZEND_THIS);
    int rv;

    ZEND_PARSE_PARAMETERS_START(2, 2)
        Z_PARAM_LONG(stream_id)
        Z_PARAM_LONG(error_code)
    ZEND_PARSE_PARAMETERS_END();

    if (nghttp2_session_require_open(intern) != SUCCESS) {
        RETURN_THROWS();
    }

    if (stream_id <= 0) {
        zend_argument_value_error(1, "must be greater than 0");
        RETURN_THROWS();
    }
    if (error_code < 0) {
        zend_argument_value_error(2, "must be greater than or equal to 0");
        RETURN_THROWS();
    }

    rv = nghttp2_submit_rst_stream(intern->session, NGHTTP2_FLAG_NONE, (int32_t)stream_id, (uint32_t)error_code);
    if (rv != 0) {
        nghttp2_throw_session_exception(nghttp2_strerror(rv), rv);
        RETURN_THROWS();
    }

    if (nghttp2_session_flush_outbound(intern) != SUCCESS) {
        RETURN_THROWS();
    }
}

ZEND_METHOD(Nghttp2_Session, submitGoaway)
{
    zend_long error_code = 0;
    zend_string *debug_data = NULL;
    nghttp2_session_object *intern = Z_NGHTTP2_SESSION_OBJ_P(ZEND_THIS);
    int32_t last_stream_id;
    int rv;

    ZEND_PARSE_PARAMETERS_START(0, 2)
        Z_PARAM_OPTIONAL
        Z_PARAM_LONG(error_code)
        Z_PARAM_STR(debug_data)
    ZEND_PARSE_PARAMETERS_END();

    if (nghttp2_session_require_open(intern) != SUCCESS) {
        RETURN_THROWS();
    }

    if (error_code < 0) {
        zend_argument_value_error(1, "must be greater than or equal to 0");
        RETURN_THROWS();
    }

    last_stream_id = nghttp2_session_get_last_proc_stream_id(intern->session);
    rv = nghttp2_submit_goaway(
        intern->session,
        NGHTTP2_FLAG_NONE,
        last_stream_id,
        (uint32_t)error_code,
        debug_data != NULL ? (const uint8_t *)ZSTR_VAL(debug_data) : NULL,
        debug_data != NULL ? ZSTR_LEN(debug_data) : 0
    );
    if (rv != 0) {
        nghttp2_throw_session_exception(nghttp2_strerror(rv), rv);
        RETURN_THROWS();
    }

    if (nghttp2_session_flush_outbound(intern) != SUCCESS) {
        RETURN_THROWS();
    }
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
