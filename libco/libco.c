#if defined(__clang__)
  #pragma clang diagnostic ignored "-Wparentheses"

  /* placing code in section(text) does not mark it executable with Clang. */
  #undef  LIBCO_MPROTECT
  #define LIBCO_MPROTECT
#endif

#if defined(__clang__) || defined(__GNUC__)
  #if defined(_WIN32) && (defined(__x86_64__) || defined(__amd64__))
    /* Windows fibers own their stacks, so deterministic save states cannot
       serialize them.  The amd64 backend implements the Win64 ABI and keeps
       coroutine stacks in libco-owned memory. */
    #include "amd64.c"
  #elif defined(_WIN32) && defined(__i386__)
    #include "x86.c"
  #elif defined(__i386__)
    #include "x86.c"
  #elif defined(__amd64__)
    #include "amd64.c"
  #elif defined(__arm__)
    #include "arm.c"
  #elif defined(__aarch64__)
    #include "aarch64.c"
  #elif defined(__powerpc64__) && defined(_CALL_ELF) && _CALL_ELF == 2
    #include "ppc64v2.c"
  #elif defined(_ARCH_PPC) && !defined(__LITTLE_ENDIAN__)
    #include "ppc.c"
  #elif defined(_WIN32)
    #include "fiber.c"
  #else
    #include "sjlj.c"
  #endif
#elif defined(_MSC_VER)
  #if defined(_M_IX86)
    #include "x86.c"
  #elif defined(_M_AMD64)
    #include "amd64.c"
  #else
    #include "fiber.c"
  #endif
#else
  #error "libco: unsupported processor, compiler or operating system"
#endif
