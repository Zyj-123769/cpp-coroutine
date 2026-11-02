# 协程库（C++）构建脚本
# 用法：
#   make                # 编译所有测试与示例
#   make test_fiber     # 只编译某个目标
#   make clean          # 清理产物
#
# 依赖：Linux + g++，需要 ucontext / epoll / pthread / dlsym（均为 Linux 原生能力）

CXX      ?= g++
# -U_FORTIFY_SOURCE/-D_FORTIFY_SOURCE=0：Ubuntu 在优化构建时默认启用 _FORTIFY_SOURCE，
# 会把 read/write 等定长调用改写为 __read_chk/__write_chk，绕过本库的 dlsym hook，
# 导致协程挂起语义失效。syscall hook 库必须禁用 fortify。
CXXFLAGS ?= -std=c++14 -Wall -D_GNU_SOURCE -Iinclude -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0
LDFLAGS  ?= -pthread -ldl

SRCS := $(wildcard src/*.cpp)
OBJS := $(SRCS:.cpp=.o)

TARGETS := test_fiber test_scheduler test_timer test_ioscheduler test_hook \
	test_io_concurrency test_fiber_exception test_stack_overflow \
	echo_server bench_http_server bench_native_http_server

.PHONY: all clean

all: $(TARGETS)

# 编译库源码
src/%.o: src/%.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

# 各测试 / 示例（统一链接所有库对象）
test_fiber: tests/test_fiber.cpp $(OBJS)
	$(CXX) $(CXXFLAGS) tests/test_fiber.cpp $(OBJS) $(LDFLAGS) -o $@

test_scheduler: tests/test_scheduler.cpp $(OBJS)
	$(CXX) $(CXXFLAGS) tests/test_scheduler.cpp $(OBJS) $(LDFLAGS) -o $@

test_timer: tests/test_timer.cpp $(OBJS)
	$(CXX) $(CXXFLAGS) tests/test_timer.cpp $(OBJS) $(LDFLAGS) -o $@

test_ioscheduler: tests/test_ioscheduler.cpp $(OBJS)
	$(CXX) $(CXXFLAGS) tests/test_ioscheduler.cpp $(OBJS) $(LDFLAGS) -o $@

test_hook: tests/test_hook.cpp $(OBJS)
	$(CXX) $(CXXFLAGS) tests/test_hook.cpp $(OBJS) $(LDFLAGS) -o $@

# 边界测试
test_io_concurrency: tests/test_io_concurrency.cpp $(OBJS)
	$(CXX) $(CXXFLAGS) tests/test_io_concurrency.cpp $(OBJS) $(LDFLAGS) -o $@

test_fiber_exception: tests/test_fiber_exception.cpp $(OBJS)
	$(CXX) $(CXXFLAGS) tests/test_fiber_exception.cpp $(OBJS) $(LDFLAGS) -o $@

test_stack_overflow: tests/test_stack_overflow.cpp $(OBJS)
	$(CXX) $(CXXFLAGS) tests/test_stack_overflow.cpp $(OBJS) $(LDFLAGS) -o $@

echo_server: examples/echo_server.cpp $(OBJS)
	$(CXX) $(CXXFLAGS) examples/echo_server.cpp $(OBJS) $(LDFLAGS) -o $@

# 压测用 HTTP server
bench_http_server: bench/http_server.cpp $(OBJS)
	$(CXX) $(CXXFLAGS) bench/http_server.cpp $(OBJS) $(LDFLAGS) -o $@

bench_native_http_server: bench/native_http_server.cpp
	$(CXX) $(CXXFLAGS) bench/native_http_server.cpp $(LDFLAGS) -o $@

# UBSan 构建 + 全部测试
# 说明：此处未启用 ASan。ASan 的符号插桩会让 hooked read 经 PLT 解析到 ASan 拦截器、
# 绕过本库的 hook（write 不受影响），导致依赖协程挂起语义的 IO 测试失效；
# UBSan 无此问题，可完整覆盖并全部通过。fiber.cpp 已内置协程栈切换的 ASan 标注，
# 如需 ASan 可手动追加 -fsanitize=address 构建。
# -fno-sanitize-recover=all：真 UB 触发即非零退出，保证 check 结果可信。
#
# 编译器对齐：clang 的 -fsanitize=undefined 默认额外启用 unsigned-integer-overflow，
# 而该检查捕获的是 unsigned 类型回绕（C/C++ 标准中的定义良好行为，并非 UB），
# gcc 的 undefined 组默认不含它、也不接受该选项名；为避免 clang 因良性回绕误 abort、
# 并让 g++/clang 的 check 行为一致，仅对 unsigned-integer-overflow 使用 recover。
#
# 运行时依赖：clang 的 -fsanitize=undefined 需要 compiler-rt 运行时
# （libclang_rt.ubsan_standalone*.a）。它在 Debian/Ubuntu 上由 libclang-rt-<ver>-dev 提供，
# 而该包只是 libclang-common-<ver>-dev 的 Recommends —— 若用 --no-install-recommends
# 安装 clang（CI 镜像常见做法），就会缺失，链接期报
# "cannot find libclang_rt.ubsan_standalone-x86_64.a"，check 直接 Error 2。
# 若遇此错，安装对应运行时即可：
#   sudo apt-get install -y libclang-rt-$(clang++ -dumpversion | cut -d. -f1)-dev
UBSAN_CXXFLAGS = -fsanitize=undefined -fno-sanitize-recover=all -g -O1
UBSAN_LDFLAGS  = -fsanitize=undefined -fno-sanitize-recover=all
ifneq (,$(findstring clang,$(CXX)))
  UBSAN_CXXFLAGS += -fsanitize-recover=unsigned-integer-overflow
  UBSAN_LDFLAGS  += -fsanitize-recover=unsigned-integer-overflow
endif

check:
	$(MAKE) clean
	$(MAKE) CXXFLAGS="$(CXXFLAGS) $(UBSAN_CXXFLAGS)" \
	       LDFLAGS="$(LDFLAGS) $(UBSAN_LDFLAGS)"
	bash tests/run_tests.sh

# 实验性 ThreadSanitizer 构建 + 全部测试
# 注意：TSan 会拦截 read/write/recv/send 等 libc 调用，与本库 dlsym hook 存在交互；
# 同时 TSan 默认不理解 swapcontext 用户态协程切换，会把协程间共享内存误报为数据竞争，
# 需在 src/fiber.cpp 接入 __tsan_switch_to_fiber 注解、并厘清 hook 交互后方可可靠使用。
# 当前仅作实验入口（不纳入合并门禁），用于暴露潜在竞态线索。
check-tsan:
	$(MAKE) clean
	$(MAKE) CXXFLAGS="$(CXXFLAGS) -fsanitize=thread -g -O1" \
	       LDFLAGS="$(LDFLAGS) -fsanitize=thread"
	bash tests/run_tests.sh

clean:
	rm -f $(OBJS) $(TARGETS)