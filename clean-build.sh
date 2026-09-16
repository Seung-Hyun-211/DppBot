#!/bin/bash

set -e

echo "=== 완전히 깨끗하게 재빌드 ==="

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

read -p "모든 빌드 산출물과 vcpkg_installed를 삭제하시겠습니까? (y/N): " -n 1 -r
echo
if [[ ! $REPLY =~ ^[Yy]$ ]]; then
    echo "취소되었습니다."
    exit 0
fi

echo "삭제 중..."
rm -rf build-linux
rm -rf vcpkg_installed

echo "의존성 재설치 중..."
./vcpkg/vcpkg install --triplet x64-linux

echo "빌드 중..."
mkdir -p build-linux
cd build-linux
cmake .. -DCMAKE_TOOLCHAIN_FILE=../vcpkg/scripts/buildsystems/vcpkg.cmake
cmake --build . --config Release -j$(nproc)

echo "=== 재빌드 완료 ==="
