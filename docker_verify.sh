#!/usr/bin/env bash
# 协程库容器化构建 + 测试验证
# 前置：用户已在沙箱外启动 Docker Desktop（让 docker daemon 可达）。
# 本脚本在容器内执行：常规 make 编译 + UBSan make check 全量测试。
set -euo pipefail
cd "$(dirname "$0")"

IMAGE="sylar-coroutine-verify"

echo ">>> 构建验证镜像并运行 make + make check ..."
docker build -t "$IMAGE" .

echo ""
echo ">>> 全部通过：常规编译无错误，UBSan 构建下所有测试 PASS。"
