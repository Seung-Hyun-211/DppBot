@echo off
REM Windows용 바이너리 빌드 스크립트

echo Building for Windows...
go build -o Go-Local.exe .

if %ERRORLEVEL% EQU 0 (
    echo Build successful! Output: Go-Local.exe
) else (
    echo Build failed!
    exit /b 1
)
