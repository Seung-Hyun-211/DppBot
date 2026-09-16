#!/bin/bash
# 클라이언트(C++)와 서버(Go)를 한 번에 빌드한다.
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

echo "=== DppBot 전체 빌드 (클라이언트 + 서버) ==="
echo ""

echo "[1/2] 클라이언트 빌드 (./auto-build.sh)"
echo ""
./auto-build.sh
cd "$SCRIPT_DIR"
echo ""

echo "[2/2] 서버 빌드 (Go)"
if ! command -v go &> /dev/null; then
    echo "오류: go가 설치되어 있지 않습니다. https://go.dev/dl 에서 설치 후 다시 실행하세요."
    exit 1
fi
cd server
go build -o Go-Local .
cd "$SCRIPT_DIR"
echo "✓ 서버 빌드 완료: server/Go-Local"
echo ""

echo "=== 전체 빌드 완료 ==="
echo "클라이언트: build-linux/dppTest"
echo "서버      : server/Go-Local"
echo ""
echo "실행하려면: ./run-all.sh"
