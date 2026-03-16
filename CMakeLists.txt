cmake_minimum_required(VERSION 3.16)
project(OrderExecutorModule LANGUAGES CXX)

# ---------------------------------------------------------------------------
# Language standard
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)   # avoid -std=gnu++17; use strict -std=c++17

# ---------------------------------------------------------------------------
# Compiler flags
# ---------------------------------------------------------------------------
if(MSVC)
    add_compile_options(
        /O2         # maximize speed
        /W4         # high warning level
        /WX-        # warnings not errors (third-party headers may warn)
        /arch:AVX2  # enable AVX2 SIMD (Haswell+)
        /Oi         # enable intrinsic functions
        /GL         # whole-program optimization
    )
    add_link_options(/LTCG)     # link-time code generation (pairs with /GL)
else()
    add_compile_options(
        -O3
        -Wall
        -Wextra
        -Wpedantic
        -march=native            # CPU-specific tuning (SIMD, branch prediction)
        -funroll-loops
        -fno-rtti                # disable RTTI (not used; saves binary size)
    )
endif()

# ---------------------------------------------------------------------------
# Threading
# ---------------------------------------------------------------------------
find_package(Threads REQUIRED)

# ---------------------------------------------------------------------------
# Main test binary
# ---------------------------------------------------------------------------
add_executable(order_executor_test
    main.cpp
    OrderExecutor.cpp
)

target_include_directories(order_executor_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(order_executor_test PRIVATE
    Threads::Threads
)

# ---------------------------------------------------------------------------
# Optional: AddressSanitizer build  (-DENABLE_ASAN=ON)
# ---------------------------------------------------------------------------
option(ENABLE_ASAN "Build with AddressSanitizer (GCC/Clang only)" OFF)
if(ENABLE_ASAN)
    if(MSVC)
        message(WARNING "ENABLE_ASAN is not supported on MSVC; ignoring.")
    else()
        target_compile_options(order_executor_test PRIVATE -fsanitize=address -g -O1)
        target_link_options(order_executor_test PRIVATE -fsanitize=address)
    endif()
endif()

# ---------------------------------------------------------------------------
# Optional: ThreadSanitizer build  (-DENABLE_TSAN=ON)
# ---------------------------------------------------------------------------
option(ENABLE_TSAN "Build with ThreadSanitizer (GCC/Clang only)" OFF)
if(ENABLE_TSAN)
    if(MSVC)
        message(WARNING "ENABLE_TSAN is not supported on MSVC; ignoring.")
    else()
        target_compile_options(order_executor_test PRIVATE -fsanitize=thread -g -O1)
        target_link_options(order_executor_test PRIVATE -fsanitize=thread)
    endif()
endif()

# ---------------------------------------------------------------------------
# Build summary
# ---------------------------------------------------------------------------
message(STATUS "Project      : ${PROJECT_NAME}")
message(STATUS "Build type   : ${CMAKE_BUILD_TYPE}")
message(STATUS "C++ standard : ${CMAKE_CXX_STANDARD}")
message(STATUS "Compiler     : ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
