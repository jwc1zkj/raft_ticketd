#ifndef container_of
#include <stddef.h>
#if defined(_WIN32) || defined(__WIN32__) || defined(WIN32)
#define container_of(ptr, type, member) \
    ((type *)((char *)(ptr) - offsetof(type, member)))
#else
#define container_of(ptr, type, member) ({ \
                        const typeof( ((type*)0)->member ) \
                        * __mptr = ((void*)(ptr)); \
                        (type*)( (char*)__mptr - \
                        offsetof(type, member) ); \
                        })
#endif
#endif
