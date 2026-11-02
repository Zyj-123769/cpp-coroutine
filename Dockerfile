# 协程库（sylar_coroutine）Linux 构建 / 测试验证镜像
# 用法（需先确保 Docker Desktop 已启动、daemon 可达）：
#   docker build -t sylar-coroutine-verify .
# 构建过程会依次执行：常规编译(make) + UBSan 编译并跑全量测试(make check)。
# 任一编译错误或测试失败都会使 docker build 非零退出，便于 CI 判定。

FROM gcc:bookworm

WORKDIR /src

# 复制源码（.dockerignore 已排除 Windows 产物与 .workbuddy）
COPY . .

# 1) 常规构建：确认在 Linux + g++ 下无编译错误（含本机加的 CLOCK_MONOTONIC 改动）
RUN make clean && make

# 2) UBSan 构建 + 全量测试：
#    make check 会 clean 后以 -fsanitize=undefined -fno-sanitize-recover=all 重建，
#    并运行 tests/run_tests.sh（含 fork 隔离的栈溢出测试、IO 并发 CPU 断言等）。
RUN make check

CMD ["bash"]
