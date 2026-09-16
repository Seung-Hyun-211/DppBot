#!/bin/bash
# 서버(Go)를 백그라운드로 띄운 뒤 클라이언트(C++)를 실행한다.
# 클라이언트가 종료되면(또는 Ctrl+C) 서버도 함께 정리한다.
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

CLIENT_BIN="build-linux/dppTest"
SERVER_BIN="server/Go-Local"

echo "=== DppBot 실행 (서버 + 클라이언트) ==="
echo ""

if [ ! -f "$CLIENT_BIN" ] || [ ! -f "$SERVER_BIN" ]; then
    echo "오류: 빌드된 실행 파일이 없습니다. 먼저 ./build-all.sh 를 실행하세요."
    exit 1
fi
if [ ! -f "config.json" ]; then
    echo "오류: config.json 이 없습니다. config.example.json 을 참고해 만들어주세요."
    exit 1
fi
if [ ! -f "server/config.json" ]; then
    echo "오류: server/config.json 이 없습니다. server/config.example.json 을 참고해 만들어주세요."
    exit 1
fi

for cmd in yt-dlp ffmpeg; do
    if ! command -v "$cmd" &> /dev/null; then
        echo "경고: '$cmd' 가 PATH에 없습니다. 다운로드/변환 기능이 실패할 수 있습니다."
    fi
done
echo ""

echo "[1/2] 서버 시작 중..."
"./$SERVER_BIN" > server.log 2>&1 &
SERVER_PID=$!

cleanup() {
    echo ""
    echo "종료 중... 서버(PID $SERVER_PID) 정리"
    kill "$SERVER_PID" 2>/dev/null
    wait "$SERVER_PID" 2>/dev/null
}
trap cleanup EXIT INT TERM

# 서버가 응답할 때까지 최대 15초 대기
READY=0
for i in $(seq 1 15); do
    if ! kill -0 "$SERVER_PID" 2>/dev/null; then
        echo "오류: 서버가 시작되지 못했습니다. server.log 확인:"
        cat server.log
        exit 1
    fi
    if curl -s -o /dev/null "http://127.0.0.1:8080/getrandom?count=1"; then
        READY=1
        break
    fi
    sleep 1
done

if [ "$READY" -ne 1 ]; then
    echo "오류: 서버가 15초 안에 응답하지 않았습니다. server.log 확인:"
    cat server.log
    exit 1
fi
echo "✓ 서버 준비됨 (PID $SERVER_PID, 로그: server.log)"
echo ""

echo "[2/2] 클라이언트 시작"
echo ""
LD_LIBRARY_PATH="$SCRIPT_DIR/vcpkg_installed/x64-linux/lib:$LD_LIBRARY_PATH" "$CLIENT_BIN"
