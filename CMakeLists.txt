cmake_minimum_required(VERSION 3.10)
project(sylar_coroutine LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ucontext/pthread_setname_np/syscall 等需要 _GNU_SOURCE
# 禁用 _FORTIFY_SOURCE：否则优化构建会把 read/write 定长调用改写为 __read_chk/__write_chk，
# 绕过本库的 dlsym hook，导致协程挂起语义失效（syscall hook 库必须禁用）。
add_compile_options(-Wall -D_GNU_SOURCE -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0)

# 协程库静态库
file(GLOB SRC_FILES ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp)
add_library(sylar STATIC ${SRC_FILES})
target_include_directories(sylar PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(sylar PUBLIC pthread dl)

# 各阶段测试
foreach(t test_fiber test_scheduler test_timer test_ioscheduler test_hook
         test_io_concurrency test_fiber_exception test_stack_overflow)
    add_executable(${t} tests/${t}.cpp)
    target_link_libraries(${t} sylar)
endforeach()

# 协程 echo server 示例
add_executable(echo_server examples/echo_server.cpp)
target_link_libraries(echo_server sylar)

# 压测用 HTTP server（协程版链接库；原生线程版不依赖库）
add_executable(bench_http_server bench/http_server.cpp)
target_link_libraries(bench_http_server sylar)

add_executable(bench_native_http_server bench/native_http_server.cpp)
target_link_libraries(bench_native_http_server pthread)