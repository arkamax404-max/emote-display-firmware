#ifndef PNGDEC_UINT_COMPAT_H
#define PNGDEC_UINT_COMPAT_H

// Host-only compatibility shim for the pinned PNGdec sources. The vendored
// zlib code inside PNGdec (inflate.c inflateSetDictionary) uses the historic
// BSD `uint` type, which Windows targets do not define. POSIX platforms leave
// this header empty.
#if defined(_WIN32)
typedef unsigned int uint;
#endif

#endif
