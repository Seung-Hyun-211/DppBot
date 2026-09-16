//go:build windows
// +build windows

package main

import (
	"syscall"
)

// setConsoleUTF8Windows Windows에서 콘솔을 UTF-8로 설정
func setConsoleUTF8Windows() {
	kernel32 := syscall.NewLazyDLL("kernel32.dll")
	kernel32.NewProc("SetConsoleCP").Call(uintptr(65001))
	kernel32.NewProc("SetConsoleOutputCP").Call(uintptr(65001))
}
