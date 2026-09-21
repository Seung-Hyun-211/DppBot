#!/bin/bash
# crash-backtrace.log (glibc backtrace_symbols_fd 형식)를 사람이 읽기 좋게 바꿔서 출력한다.
#   ./symbolize-crash.sh [로그파일]      (기본: crash-backtrace.log)
#
# - 심볼 이름이 있는 프레임: c++filt로 demangle
# - 이름 없이 오프셋만 있는 프레임(예: libdpp.so 내부 함수): addr2line으로 함수 이름 복원
LOG="${1:-crash-backtrace.log}"

if [ ! -f "$LOG" ]; then
    echo "$LOG 파일이 없습니다."
    exit 1
fi

have_addr2line=0
have_cxxfilt=0
command -v addr2line > /dev/null 2>&1 && have_addr2line=1
command -v c++filt > /dev/null 2>&1 && have_cxxfilt=1

demangle() {
    if [ "$have_cxxfilt" -eq 1 ]; then
        printf '%s\n' "$1" | c++filt
    else
        printf '%s\n' "$1"
    fi
}

# 한 줄 형식: 모듈경로(심볼+0x오프셋) [0x주소]  /  모듈경로(+0x오프셋) [0x주소]
re='^([^(]+)\(([^)]*)\) \[(0x[0-9a-fA-F]+)\]$'

while IFS= read -r line; do
    if [[ "$line" =~ $re ]]; then
        module="${BASH_REMATCH[1]}"
        inner="${BASH_REMATCH[2]}"

        if [[ "$inner" == *+* ]]; then
            sym="${inner%%+*}"
            off="${inner#*+}"
        else
            sym="$inner"
            off=""
        fi

        if [ -n "$sym" ]; then
            echo "  $module  $(demangle "$sym")${off:+  (+$off)}"
        elif [ -n "$off" ] && [ "$have_addr2line" -eq 1 ] && [ -f "$module" ]; then
            fn="$(addr2line -f -C -e "$module" "$off" 2> /dev/null | head -n 1)"
            echo "  $module  ${fn:-??}  (+$off)"
        else
            echo "  $line"
        fi
    else
        echo "$line"
    fi
done < "$LOG"
