#!/bin/bash
cd "$(dirname "$0")"
make > /dev/null 2>&1
echo "=== 수신 프로그램 실행 ==="
./receiver 9000 output.bin 0.05

