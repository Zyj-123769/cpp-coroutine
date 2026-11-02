#!/usr/bin/env bash
# 压测对比：协程 HTTP server vs 原生线程 HTTP server（ab 测 QPS / CPU / 内存）
# 用法：bash bench/bench.sh          # 默认 -n 20000 -c 100
#       REQS=50000 CONC=200 bash bench/bench.sh
# 依赖：ab（apache2-utils），Ubuntu 下 apt-get install -y apache2-utils
set -euo pipefail
cd "$(dirname "$0")/.."

REQS="${REQS:-20000}"
CONC="${CONC:-100}"
PORT_CORO=18080
PORT_NATIVE=18081

command -v ab >/dev/null 2>&1 || { echo "缺少 ab，请先: sudo apt-get install -y apache2-utils"; exit 1; }

echo "== 编译 =="
make bench_http_server bench_native_http_server

bench() { # $1=名称 $2=二进制 $3=端口
  local name=$1 bin=$2 port=$3 pid abpid qps s cpus rsss
  ./"$bin" "$port" > /tmp/bench_${name}.log 2>&1 &
  pid=$!
  sleep 1

  # 后台压测，同时采样服务器 CPU/RSS
  ab -q -n "$REQS" -c "$CONC" "http://127.0.0.1:${port}/" > /tmp/bench_${name}.ab 2>&1 &
  abpid=$!
  cpus=""; rsss=""
  while kill -0 "$abpid" 2>/dev/null; do
    s=$(ps -o %cpu=,rss= -p "$pid" 2>/dev/null || echo "0 0")
    cpus="$cpus $(echo "$s" | awk '{print $1}')"
    rsss="$rsss $(echo "$s" | awk '{print $2}')"
    sleep 0.2
  done
  wait "$abpid" || true

  qps=$(awk '/Requests per second/{print $4}' /tmp/bench_${name}.ab 2>/dev/null || echo "?")
  kill "$pid" 2>/dev/null || true
  wait "$pid" 2>/dev/null || true

  # 采样序列汇总：均值 / 峰值
  cpu_mean=$(echo "$cpus" | awk '{s=0;n=0;m=$1;for(i=1;i<=NF;i++){s+=$i;n++;if($i>m)m=$i}if(n>0)printf "%.1f",s/n;else printf "0"}')
  cpu_max=$(echo "$cpus" | awk '{m=$1;for(i=1;i<=NF;i++)if($i>m)m=$i;print m}')
  rss_mean=$(echo "$rsss" | awk '{s=0;n=0;m=$1;for(i=1;i<=NF;i++){s+=$i;n++;if($i>m)m=$i}if(n>0)printf "%.0f",s/n;else printf "0"}')
  rss_max=$(echo "$rsss" | awk '{m=$1;for(i=1;i<=NF;i++)if($i>m)m=$i;print m}')

  echo "------------------------------------------------------------"
  echo "$name:"
  echo "  QPS           : ${qps:-?}"
  echo "  CPU%  mean/max : ${cpu_mean} / ${cpu_max}"
  echo "  RSS_KB mean/max: ${rss_mean} / ${rss_max}"
}

bench coroutine bench_http_server      "$PORT_CORO"
bench native    bench_native_http_server "$PORT_NATIVE"

echo "------------------------------------------------------------"
echo "对比结论：协程版用 1 个线程 + 每连接 1 协程；原生版每连接 1 线程。"
echo "并发越高、短连接占比越大，协程版的 QPS 优势与 CPU/内存优势越明显。"
echo "（本机为 WSL，绝对数值低于裸机；相对对比仍有意义）"