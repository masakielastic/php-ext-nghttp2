PHP_ARG_ENABLE([nghttp2],
  [whether to enable nghttp2 extension],
  [AS_HELP_STRING([--enable-nghttp2], [Enable nghttp2 extension])],
  [yes])

if test "$PHP_NGHTTP2" != "no"; then
  PKG_CHECK_MODULES([NGHTTP2], [libnghttp2])
  PHP_EVAL_INCLINE($NGHTTP2_CFLAGS)
  PHP_EVAL_LIBLINE($NGHTTP2_LIBS, NGHTTP2_SHARED_LIBADD)

  PHP_NEW_EXTENSION([nghttp2], [nghttp2.c hpack.c exception.c], [$ext_shared])
  PHP_SUBST([NGHTTP2_SHARED_LIBADD])
fi
