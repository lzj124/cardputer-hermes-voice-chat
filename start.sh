#!/bin/bash
# ── ClawVoice 一键启动 ──────────────────────────────────────
# 启动 proxy (Flask 8900) + USB bridge (串口 ↔ proxy)
# Ctrl+C 停止全部
# ────────────────────────────────────────────────────────────

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SERVER_DIR="$SCRIPT_DIR/server"

# 加载 .env 环境变量
if [ -f "$SCRIPT_DIR/.env" ]; then
    set -a  # 自动 export 所有变量
    source "$SCRIPT_DIR/.env"
    set +a
    echo -e "  ${GREEN}✓ 已加载 .env${NC}"
fi

# 颜色
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

cleanup() {
    echo ""
    echo -e "${YELLOW}╺ 正在停止...${NC}"
    # 杀 bridge
    [ -n "$BRIDGE_PID" ] && kill "$BRIDGE_PID" 2>/dev/null && echo "  ✓ bridge 已停止" || true
    # 杀 proxy
    [ -n "$PROXY_PID" ] && kill "$PROXY_PID" 2>/dev/null && echo "  ✓ proxy 已停止" || true
    echo -e "${GREEN}╸ ClawVoice 已关闭${NC}"
    exit 0
}
trap cleanup SIGINT SIGTERM

echo -e "${GREEN}━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
echo -e "${GREEN}  ClawVoice 启动中...${NC}"
echo -e "${GREEN}━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"

# 1. 杀旧进程（避免端口冲突）
echo ""
echo "  清理残留进程..."
pkill -f "usb_bridge.py" 2>/dev/null && echo "    ⚡ 已清理旧 bridge" || true
pkill -f "proxy.py" 2>/dev/null && echo "    ⚡ 已清理旧 proxy" || true
sleep 1

# 2. 启动 proxy（后台）
echo ""
echo "  启动 proxy (:8900)..."
cd "$SERVER_DIR"
python3 proxy.py &
PROXY_PID=$!

# 3. 等 proxy 就绪
echo "  等待 proxy 就绪..."
for i in $(seq 1 10); do
    if curl -s http://127.0.0.1:8900/ > /dev/null 2>&1; then
        echo -e "  ${GREEN}✓ proxy 已就绪 (PID $PROXY_PID)${NC}"
        break
    fi
    sleep 1
done

# 4. 启动 bridge（前台，日志可见）
echo ""
echo -e "  启动 USB bridge (自动检测串口)..."
echo -e "  ${YELLOW}━━ Ctrl+C 停止 ━━${NC}"
echo ""

python3 usb_bridge.py &
BRIDGE_PID=$!

# 等待任意子进程退出
wait -n 2>/dev/null || wait
