AC_DEFUN([PDNS_CHECK_LIBCRYPTO_PQC], [
  AC_REQUIRE([PDNS_CHECK_LIBCRYPTO])
  # Set the environment correctly for a possibly non-default OpenSSL path that was found by/supplied to PDNS_CHECK_LIBCRYPTO
  save_CPPFLAGS="$CPPFLAGS"
  save_LDFLAGS="$LDFLAGS"
  save_LIBS="$LIBS"
  CPPFLAGS="$LIBCRYPTO_INCLUDES $CPPFLAGS"
  LDFLAGS="$LIBCRYPTO_LDFLAGS $LDFLAGS"
  LIBS="$LIBCRYPTO_LIBS $LIBS"

  # Add checks for other NID algorithm here
  libcrypto_falcon=no
  libcrypto_dilithium2=no
  libcrypto_rainbowIclassic=no

  AC_CHECK_DECLS([NID_falcon512], [
    libcrypto_falcon=yes
    AC_DEFINE([HAVE_LIBCRYPTO_FALCON], [1], [define to 1 if OpenSSL Falcon512 support is available.])
  ], [ : ],
  [AC_INCLUDES_DEFAULT
  #include <$ssldir/include/openssl/evp.h>])

  AC_CHECK_DECLS([NID_dilithium2], [
    libcrypto_dilithium2=yes
    AC_DEFINE([HAVE_LIBCRYPTO_DILITHIUM], [1], [define to 1 if OpenSSL Dilithium 2 support is available.])
  ], [ : ],
  [AC_INCLUDES_DEFAULT
  #include <$ssldir/include/openssl/evp.h>])

  AC_CHECK_DECLS([NID_rainbowIclassic], [
    libcrypto_rainbowIclassic=yes
    AC_DEFINE([HAVE_LIBCRYPTO_RAINBOW], [1], [define to 1 if OpenSSL Rainbow I Classic support is available.])
  ], [ : ],
  [AC_INCLUDES_DEFAULT
  #include <$ssldir/include/openssl/evp.h>])

  AS_IF([test "$libcrypto_falcon" = "yes" -o "libcrypto_dilithium2" = "yes" -o "$libcrypto_rainbowIclassic" = "yes"], [
    AC_DEFINE([HAVE_LIBCRYPTO_PQC], [1], [define to 1 if OpenSSL PQC support is available.])
  ], [ : ])
  
  # Restore variables
  CPPFLAGS="$save_CPPFLAGS"
  LDFLAGS="$save_LDFLAGS"
  LIBS="$save_LIBS"
])