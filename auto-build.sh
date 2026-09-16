#!/bin/bash

set -e  # 오류 발생 시 스크립트 중단

echo "=== dppTest 완전 자동 빌드 ==="
echo ""

# 프로젝트 루트 디렉토리 확인
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

# 시스템 패키지 자동 설치 함수
install_system_packages() {
    if command -v apt-get &> /dev/null; then
        echo "[1/5] 시스템 패키지 설치 중 (Ubuntu/Debian)..."
        sudo apt-get update -qq
        sudo apt-get install -y \
            build-essential \
            cmake \
            libssl-dev \
            libopus-dev \
            curl \
            git \
            zip \
            unzip \
            tar > /dev/null 2>&1
        echo "✓ 시스템 패키지 설치 완료"
    elif command -v dnf &> /dev/null; then
        echo "[1/5] 시스템 패키지 설치 중 (Fedora/RHEL)..."
        sudo dnf install -y \
            gcc-c++ \
            cmake \
            openssl-devel \
            opus-devel \
            curl \
            git \
            zip \
            unzip \
            tar > /dev/null 2>&1
        echo "✓ 시스템 패키지 설치 완료"
    else
        echo "경고: 패키지 매니저를 찾을 수 없습니다. 수동으로 설치가 필요할 수 있습니다."
    fi
    echo ""
}

# 필수 명령어 확인 및 자동 설치
check_and_install() {
    local cmd=$1
    local pkg=$2
    
    if ! command -v "$cmd" &> /dev/null; then
        echo "필수 도구 '$cmd'가 없습니다. 자동 설치를 시도합니다..."
        install_system_packages
        
        # 다시 확인
        if ! command -v "$cmd" &> /dev/null; then
            echo "오류: $cmd 설치에 실패했습니다. 수동으로 설치해주세요."
            exit 1
        fi
    fi
}

# 필수 도구 확인
echo "[1/5] 필수 도구 확인 중..."
check_and_install "gcc" "build-essential"
check_and_install "cmake" "cmake"
check_and_install "git" "git"
check_and_install "curl" "curl"
check_and_install "zip" "zip"
check_and_install "unzip" "unzip"
check_and_install "tar" "tar"
echo "✓ 모든 필수 도구 확인 완료"
echo ""

# vcpkg 설치
echo "[2/5] vcpkg 설치 중..."
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

# 의존성 설치
echo "[3/5] 의존성 설치 중..."
if [ ! -d "vcpkg_installed/x64-linux" ]; then
    # 메모리가 적은 환경에서 vcpkg가 코어 수만큼 병렬 컴파일하다가 OOM으로 죽는 것을 방지.
    # 컴파일 작업 하나당 대략 1.5GB로 잡고, CPU 코어 수를 넘지 않게 제한한다.
    if [ -z "$VCPKG_MAX_CONCURRENCY" ] && command -v free &> /dev/null; then
        MEM_MB=$(free -m | awk '/^Mem:/{print $2}')
        if [ -n "$MEM_MB" ] && [ "$MEM_MB" -gt 0 ]; then
            JOBS=$(( MEM_MB / 1536 ))
            [ "$JOBS" -lt 1 ] && JOBS=1
            CPU_JOBS=$(nproc)
            [ "$JOBS" -gt "$CPU_JOBS" ] && JOBS=$CPU_JOBS
            export VCPKG_MAX_CONCURRENCY=$JOBS
            echo "메모리(${MEM_MB}MB) 기준으로 빌드 병렬도를 ${JOBS}로 제한합니다 (OOM 방지)."
            if [ "$MEM_MB" -lt 2048 ]; then
                echo "경고: 메모리가 2GB 미만입니다. openssl/dpp 빌드 중 OOM이 날 수 있습니다."
                echo "      스왑을 추가하는 걸 권장합니다: sudo fallocate -l 4G /swapfile && sudo chmod 600 /swapfile && sudo mkswap /swapfile && sudo swapon /swapfile"
            fi
        fi
    fi

    echo "의존성을 설치하는 중... (시간이 오래 걸릴 수 있습니다: 10-30분)"
    ./vcpkg/vcpkg install --triplet x64-linux
    echo "✓ 의존성 설치 완료"
else
    echo "✓ 의존성이 이미 설치되어 있습니다."
fi
echo ""

# 빌드 디렉토리 준비
echo "[4/5] 빌드 디렉토리 준비 중..."
mkdir -p build-linux
cd build-linux

# CMake 구성 (기존 구성이 있으면 재구성)
if [ ! -f "CMakeCache.txt" ]; then
    echo "CMake 구성 중..."
    cmake .. -DCMAKE_TOOLCHAIN_FILE=../vcpkg/scripts/buildsystems/vcpkg.cmake
    echo "✓ CMake 구성 완료"
else
    echo "✓ CMake 구성이 이미 되어 있습니다."
fi
echo ""

# 빌드
echo "[5/5] 빌드 중..."
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
