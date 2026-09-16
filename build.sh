#!/bin/bash

set -e  # 오류 발생 시 스크립트 중단

echo "=== dppTest Linux 빌드 스크립트 ==="
echo ""

# 프로젝트 루트 디렉토리 확인
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

echo "[1/6] 시스템 패키지 확인 중..."

# 필수 명령어 확인
check_command() {
    if ! command -v "$1" &> /dev/null; then
        echo "오류: $1이 설치되어 있지 않습니다."
        echo "다음 명령어로 설치하세요:"
        if command -v apt-get &> /dev/null; then
            echo "  sudo apt-get install -y $2"
        elif command -v dnf &> /dev/null; then
            echo "  sudo dnf install -y $2"
        fi
        exit 1
    fi
}

check_command "gcc" "build-essential"
check_command "cmake" "cmake"
check_command "git" "git"
check_command "curl" "curl"
check_command "zip" "zip"
check_command "unzip" "unzip"
check_command "tar" "tar"

echo "✓ 모든 필수 패키지가 설치되어 있습니다."
echo ""

# vcpkg 설치
echo "[2/6] vcpkg 설치 중..."
if [ ! -d "vcpkg" ]; then
    echo "vcpkg를 클론하는 중..."
    git clone https://github.com/Microsoft/vcpkg.git
    cd vcpkg
    ./bootstrap-vcpkg.sh
    cd ..
    echo "✓ vcpkg 설치 완료"
else
    echo "✓ vcpkg가 이미 설치되어 있습니다."
fi
echo ""

# vcpkg 업데이트 (선택사항)
read -p "vcpkg를 업데이트하시겠습니까? (y/N): " -n 1 -r
echo
if [[ $REPLY =~ ^[Yy]$ ]]; then
    echo "vcpkg 업데이트 중..."
    cd vcpkg
    git pull
    ./bootstrap-vcpkg.sh
    cd ..
    echo "✓ vcpkg 업데이트 완료"
fi
echo ""

# 의존성 설치
echo "[3/6] 의존성 설치 중..."
if [ ! -d "vcpkg_installed/x64-linux" ]; then
    echo "의존성을 설치하는 중... (시간이 오래 걸릴 수 있습니다)"
    ./vcpkg/vcpkg install --triplet x64-linux
    echo "✓ 의존성 설치 완료"
else
    echo "의존성이 이미 설치되어 있습니다."
    read -p "의존성을 재설치하시겠습니까? (y/N): " -n 1 -r
    echo
    if [[ $REPLY =~ ^[Yy]$ ]]; then
        ./vcpkg/vcpkg install --triplet x64-linux
        echo "✓ 의존성 재설치 완료"
    fi
fi
echo ""

# 빌드 디렉토리 정리
echo "[4/6] 빌드 디렉토리 준비 중..."
read -p "기존 빌드 디렉토리를 삭제하고 새로 빌드하시겠습니까? (y/N): " -n 1 -r
echo
if [[ $REPLY =~ ^[Yy]$ ]]; then
    rm -rf build-linux
    echo "✓ 기존 빌드 디렉토리 삭제 완료"
fi

mkdir -p build-linux
cd build-linux
echo ""

# CMake 구성
echo "[5/6] CMake 구성 중..."
cmake .. -DCMAKE_TOOLCHAIN_FILE=../vcpkg/scripts/buildsystems/vcpkg.cmake
echo "✓ CMake 구성 완료"
echo ""

# 빌드
echo "[6/6] 빌드 중..."
CPU_COUNT=$(nproc)
echo "병렬 빌드 사용: $CPU_COUNT 개 작업"
cmake --build . --config Release -j$CPU_COUNT
echo ""

# 빌드 결과 확인
if [ -f "dppTest" ]; then
    echo "=== 빌드 성공! ==="
    echo "실행 파일 위치: $(pwd)/dppTest"
    echo ""
    echo "실행 방법:"
    echo "  cd build-linux"
    echo "  ./dppTest"
    echo ""
    echo "또는 라이브러리 경로를 지정하여 실행:"
    echo "  LD_LIBRARY_PATH=../vcpkg_installed/x64-linux/lib ./dppTest"
else
    echo "=== 빌드 실패 ==="
    echo "빌드 로그를 확인하세요."
    exit 1
fi
