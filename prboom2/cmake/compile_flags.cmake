if (NOT DEFINED RUNTIME_SANITIZE)
    set(RUNTIME_SANITIZE OFF)
endif()
if (NOT DEFINED STRICT_WARN_MODE)
    set(STRICT_WARN_MODE OFF)
endif()

# Enforce C/C++ standard level
#set(CMAKE_C_STANDARD_REQUIRED YES)
#set(CMAKE_CXX_STANDARD_REQUIRED YES)

# Enforce to disable any compiler-specific extensions
#set(CMAKE_C_EXTENSIONS NO)
#set(CMAKE_CXX_EXTENSIONS NO)

# Flags that enable sanitization
if (NOT DEFINED SANITIZERS)
    set(SANITIZERS "leak,undefined")
endif (NOT DEFINED SANITIZERS)
set(INSTRUMENTATION_FLAGS -fsanitize=${SANITIZERS} -fstack-check -fstack-protector-all -fno-omit-frame-pointer)

# Common flags for GNU-compatible compilers
set(COMMON_WARN_FLAGS
        ${COMMON_WARN_FLAGS}
        -pedantic
        -Wpedantic
        -Wall
        -Wextra

        # Flags to disable due to legacy codebase
        -Wno-reserved-macro-identifier
        -Wno-padded)

if ("${CMAKE_C_COMPILER_ID}" STREQUAL "GNU" OR "${CMAKE_CXX_COMPILER_ID}" STREQUAL "GNU")
    message(STATUS "Detected compiler: GNU")

    # Flags that apply to both C and C++
    set(COMMON_WARN_FLAGS
            ${COMMON_WARN_FLAGS}
            -fstrict-aliasing
            -Wcast-align
            -Wcast-qual
            -Wconversion
            -Wdisabled-optimization
            -Wdouble-promotion
            -Wduplicated-cond
            -Wduplicated-branches
            -Wfloat-equal
            -Wformat=2
            -Winit-self
            -Winline
            -Winvalid-pch
            -Wlogical-op
            -Wmissing-declarations
            -Wmissing-format-attribute
            -Wmissing-include-dirs
            -Wno-unused
            -Wnull-dereference
            -Wodr
            -Wpointer-arith
            -Wredundant-decls
            -Wrestrict
            -Wshadow
            -Wsign-conversion
            -Wstrict-overflow=5
            -Wswitch-default
            -Wswitch-enum
            -Wwrite-strings
            -Wundef
            -Wuninitialized
            -Wunreachable-code)

    # Flags that apply only to C OR C++
    set(C_WARN_FLAGS
            ${COMMON_WARN_FLAGS}
            -Wbad-function-cast
            -Wmissing-prototypes
            -Wnested-externs
            -Wold-style-definition
            -Wstrict-prototypes)
    set(CXX_WARN_FLAGS
            ${COMMON_WARN_FLAGS}
            -Wctor-dtor-privacy
            -Wnoexcept
            -Wold-style-cast
            -Woverloaded-virtual
            -Wsign-promo
            -Wstrict-null-sentinel
            -Wuseless-cast
            -Wzero-as-null-pointer-constant)

elseif ("${CMAKE_C_COMPILER_ID}" STREQUAL "Clang" OR "${CMAKE_CXX_COMPILER_ID}" STREQUAL "Clang")
    message(STATUS "Detected compiler: Clang")

    # Flags that apply to both C and C++
    set(COMMON_WARN_FLAGS ${COMMON_WARN_FLAGS} -Weverything)

    # Flags that apply only to C OR C++
    set(C_WARN_FLAGS ${COMMON_WARN_FLAGS})
    set(CXX_WARN_FLAGS ${COMMON_WARN_FLAGS} -Wno-c++98-compat -Wno-c++98-compat-pedantic)

    if (NOT ${STRICT_WARN_MODE})
        set(CXX_WARN_FLAGS
                ${CXX_WARN_FLAGS}
                -Wno-exit-time-destructors
                -Wno-global-constructors
                -Wno-padded)
    endif (NOT ${STRICT_WARN_MODE})

elseif ("${CMAKE_C_COMPILER_ID}" STREQUAL "MSVC" OR "${CMAKE_CXX_COMPILER_ID}" STREQUAL "MSVC")
    message(STATUS "Detected compiler: MSVC")

    # Flags that apply to both C and C++
    set(COMMON_WARN_FLAGS ${COMMON_WARN_FLAGS} /permissive-)
    if (${STRICT_WARN_MODE})
        set(COMMON_WARN_FLAGS ${COMMON_WARN_FLAGS} /Wall)
    else (${STRICT_WARN_MODE})
        set(COMMON_WARN_FLAGS ${COMMON_WARN_FLAGS} /W4)
    endif (${STRICT_WARN_MODE})

    # Flags that apply only to C OR C++
    set(C_WARN_FLAGS ${COMMON_WARN_FLAGS})
    set(CXX_WARN_FLAGS ${COMMON_WARN_FLAGS})

else ()
    message(FATAL_ERROR "Unknown compiler combination")
endif ()

if (NOT WIN32)
    set(SANITIZE_FLAGS ${INSTRUMENTATION_FLAGS})
endif ()

unset(INSTRUMENTATION_FLAGS)
unset(COMMON_WARN_FLAGS)
