#ifndef PLATFORM_DETECT_H
#define PLATFORM_DETECT_H

// The one definition site of the GOO_PLATFORM_* names. include/runtime.h and
// src/runtime/platform.h both need them, and each used to carry its own copy
// of this block. Two copies made the MISRA scan unrepeatable: cppcheck's
// addon reports an unused macro at the first definition it reads, and that
// order follows cppcheck's -j completion order, so the finding moved between
// the two files from run to run.
#if defined(__APPLE__) && defined(__MACH__)
    #define GOO_PLATFORM_MACOS 1
    #define GOO_PLATFORM_UNIX 1
#elif defined(__linux__)
    #define GOO_PLATFORM_LINUX 1
    #define GOO_PLATFORM_UNIX 1
#elif defined(_WIN32) || defined(_WIN64)
    #define GOO_PLATFORM_WINDOWS 1
#else
    #define GOO_PLATFORM_UNKNOWN 1
    #define GOO_PLATFORM_UNIX 1  // Assume Unix-like
#endif

#endif // PLATFORM_DETECT_H
