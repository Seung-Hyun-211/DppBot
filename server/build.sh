#!/bin/bash
# Linux에서 빌드하는 스크립트

echo "Building for Linux..."
go build -o Go-Local .

if [ $? -eq 0 ]; then
    echo "Build successful! Output: Go-Local"
    chmod +x Go-Local
else
    echo "Build failed!"
    exit 1
fi
