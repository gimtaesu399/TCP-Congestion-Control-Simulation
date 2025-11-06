#!/bin/bash
cd "$(dirname "$0")"
make > /dev/null 2>&1

# input.bin 파일이 없으면 생성
if [ ! -f "input.bin" ]; then
    echo "input.bin 파일이 없어서 테스트용 파일을 생성합니다..."
    dd if=/dev/urandom of=input.bin bs=1024 count=100 2>/dev/null
fi

echo "=== 송신 프로그램 실행 ==="
sleep 1  # 수신측이 먼저 시작되도록 약간 대기
./sender 127.0.0.1 9000 input.bin 1500 200

