#!/bin/bash

set -e

echo "=== dppTest 빠른 빌드 ==="

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

# vcpkg 설치
if [ ! -d "vcpkg" ]; then
    echo "vcpkg 설치 중..."
    git clone https://github.com/Microsoft/vcpkg.git
    cd vcpkg
    ./bootstrap-vcpkg.sh
    cd ..
fi

# 의존성 설치
if [ ! -d "vcpkg_installed/x64-linux" ]; then
    echo "의존성 설치 중..."
    ./vcpkg/vcpkg install --triplet x64-linux
fi

# 빌드
mkdir -p build-linux
cd build-linux
cmake .. -DCMAKE_TOOLCHAIN_FILE=../vcpkg/scripts/buildsystems/vcpkg.cmake
cmake --build . --config Release -j$(nproc)

echo "=== 빌드 완료 ==="
echo "실행: cd build-linux && ./dppTest"
