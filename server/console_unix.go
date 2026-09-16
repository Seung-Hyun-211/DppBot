//go:build !windows
// +build !windows

package main

// setConsoleUTF8Windows Unix 계열에서는 별도 처리가 필요 없음
func setConsoleUTF8Windows() {
	// Linux/Darwin에서는 기본적으로 UTF-8이므로 별도 처리 불필요
}
