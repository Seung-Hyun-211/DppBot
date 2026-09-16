@echo off
REM Windows에서 Linux용 바이너리 빌드 스크립트

echo Building for Linux...
set GOOS=linux
set GOARCH=amd64

go build -o Go-Local-linux .

if %ERRORLEVEL% EQU 0 (
    echo Build successful! Output: Go-Local-linux
) else (
    echo Build failed!
    exit /b 1
)
