#pragma once

// 리눅스에서 치명적 시그널(SIGSEGV, SIGABRT 등)이 발생하면 스택 트레이스를
// stderr와 ./crash-backtrace.log 에 남긴 뒤 원래 동작(코어 덤프/종료)대로 종료한다.
// 리눅스 이외의 플랫폼에서는 아무것도 하지 않는다.
void InstallCrashHandler();
