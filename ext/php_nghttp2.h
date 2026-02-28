#ifndef PHP_NGHTTP2_H
#define PHP_NGHTTP2_H

#include <php.h>
#include <nghttp2/nghttp2.h>

extern zend_module_entry nghttp2_module_entry;
#define phpext_nghttp2_ptr &nghttp2_module_entry

extern zend_class_entry *nghttp2_ce_hpack;
extern zend_class_entry *nghttp2_ce_client;
extern zend_class_entry *nghttp2_ce_server;
extern zend_class_entry *nghttp2_ce_session;
extern zend_class_entry *nghttp2_ce_hpack_exception;
extern zend_class_entry *nghttp2_ce_client_exception;
extern zend_class_entry *nghttp2_ce_server_exception;
extern zend_class_entry *nghttp2_ce_session_exception;

void nghttp2_register_exception_class(void);
void nghttp2_register_hpack_class(void);
void nghttp2_register_client_class(void);
void nghttp2_register_server_class(void);
void nghttp2_register_session_class(void);

void nghttp2_throw_hpack_exception(const char *message, int error_code);
void nghttp2_throw_client_exception(const char *message, int error_code);
void nghttp2_throw_server_exception(const char *message, int error_code);
void nghttp2_throw_session_exception(const char *message, int error_code);

#endif
