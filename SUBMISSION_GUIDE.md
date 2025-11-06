# 제출 가이드

## 📦 제출할 파일

### 필수 파일 (소스 파일만)
- `sender.c` - 송신 프로그램 소스
- `receiver.c` - 수신 프로그램 소스
- `Makefile` - 빌드 설정

### 선택 파일 (편의용)
- `run_sender.sh` - 송신 실행 스크립트
- `run_receiver.sh` - 수신 실행 스크립트

### 제출 전 삭제할 파일
- `sender` - 컴파일된 실행 파일 (삭제)
- `receiver` - 컴파일된 실행 파일 (삭제)
- `input.bin` - 테스트 데이터 (삭제)
- `output.bin` - 수신 데이터 (삭제)
- `CODE_EXPLANATION.md` - 설명 문서 (삭제, 최종보고서에 포함)

## 🎬 데모 영상 구성

### 1. 코드 설명 (약 2-3분)

#### sender.c 주요 부분 설명
1. **데이터 구조** (19-36줄)
   - `packet_header_t`: 패킷 헤더 구조
   - `ack_packet_t`: ACK 패킷 구조
   - `segment_t`: 세그먼트 구조

2. **파일 읽기 및 세그먼트 분할** (111-131줄)
   - 파일을 MSS 단위로 읽어 세그먼트 배열 생성
   - 각 세그먼트에 seq 번호 할당

3. **혼잡제어 상태 초기화** (150-162줄)
   - `cwnd = 1 MSS` (초기값)
   - `ssthresh = 65536 바이트` (초기 임계값)

4. **메인 루프 - 패킷 전송** (165-179줄)
   - cwnd 크기만큼 패킷 전송
   - 바이트 단위로 계산

5. **타임아웃 처리** (203-229줄)
   - `ssthresh = cwnd / 2`
   - `cwnd = 1 MSS`
   - Slow Start로 복귀

6. **새로운 ACK 수신 처리** (239-285줄)
   - Slow Start: `cwnd += MSS`
   - Congestion Avoidance: `cwnd += MSS × (MSS / cwnd)`
   - Fast Recovery 종료: `cwnd = ssthresh`, Congestion Avoidance 적용

7. **중복 ACK 처리** (293-321줄)
   - 3-Dup ACK: Fast Retransmit
   - Fast Recovery 중: `cwnd += MSS`

#### receiver.c 주요 부분 설명
1. **패킷 수신** (104-126줄)
   - UDP 소켓으로 패킷 수신

2. **패킷 손실 시뮬레이션** (140-168줄)
   - 랜덤 또는 강제 드롭
   - 누적 ACK 전송

3. **In-Order 데이터 처리** (170-181줄)
   - 순서대로 도착한 패킷만 처리
   - 파일에 저장 (선택적)

4. **누적 ACK 전송** (182-187줄)
   - 다음 기대 바이트 위치를 ACK로 전송

### 2. 시나리오별 데모 (각 2-3분)

#### 시나리오 1: 오류 없는 경우
**실행**:
```bash
# 터미널 1
./run_receiver.sh

# 터미널 2
./run_sender.sh
```

**설명 포인트**:
1. Slow Start: cwnd가 1400 → 2800 → 4200 ... (2배씩 증가)
2. Congestion Avoidance: cwnd가 점진적으로 증가 (선형 증가)
3. 패킷 전송 과정과 cwnd 변화 실시간 관찰

**예상 출력**:
- Slow Start: "cwin 1400 바이트 증가"
- Congestion Avoidance: "cwin 700 바이트 증가" (증가량이 작아짐)

---

#### 시나리오 2: 3-Dup ACK 사건 발생
**실행**:
```bash
# 터미널 1 - seq 7000 패킷 강제 드롭
./receiver 9000 output.bin 0 7000

# 터미널 2
./run_sender.sh
```

**설명 포인트**:
1. seq 7000 패킷 드롭
2. 중복 ACK 수신 (ACK 7000 반복)
3. 3-Dup ACK 발생 → Fast Retransmit
4. `ssthresh = cwnd / 2`, `cwnd = ssthresh + 3 × MSS`
5. Fast Recovery 상태 진입
6. 중복 ACK마다 `cwnd += MSS`
7. 새로운 ACK 수신 → Fast Recovery 종료, Congestion Avoidance로 전환

**예상 출력**:
```
<--- ACK 7000 수신 (중복 #1)
<--- ACK 7000 수신 (중복 #2)
<--- ACK 7000 수신 (중복 #3)
<<< 3-Dup ACK 사건 발생>>>
- cwin: ½ + 3×MSS = 8400 바이트
- 임계값: 4200 바이트로 설정
---→ 패킷 (seq:7000, size:1400) 재전송
<--- ACK 7000 수신 (중복 #1)
  => Fast Recovery: cwin 9800 바이트로 증가
...
<--- ACK 8400 수신 (Fast Recovery 종료) => cwin 4200 바이트 (Congestion Avoidance)
```

---

#### 시나리오 3: 타임아웃 사건 발생
**실행**:
```bash
# 터미널 1 - 10% 패킷 손실
./receiver 9000 output.bin 0.1

# 터미널 2
./run_sender.sh
```

**설명 포인트**:
1. 패킷 손실로 ACK 미수신
2. RTO(200ms) 시간 경과 → 타임아웃 발생
3. `ssthresh = cwnd / 2`, `cwnd = 1 MSS`
4. Slow Start로 복귀
5. 타임아웃 후 재전송 및 Slow Start 재시작

**예상 출력**:
```
<<<타임아웃 사건 발생>>>
- cwin: 1400 바이트 (1 MSS)로 조정
- 임계값: 1750 바이트로 설정
---→ 패킷 (seq:9800, size:1400) 재전송
```

### 3. 결과 설명 (약 1-2분)

**전송 통계 설명**:
- 전송된 데이터 크기
- 전송 시간 및 처리량
- 타임아웃 횟수
- 중복 ACK 재전송 횟수
- 최종 cwnd 및 ssthresh 값

**주요 관찰 사항**:
- Slow Start에서 지수적 증가
- Congestion Avoidance에서 선형 증가
- 혼잡 사건 발생 시 cwnd 및 ssthresh 조정
- Fast Recovery 동작

---

## 📝 제출 전 체크리스트

### 파일 정리
- [ ] `sender`, `receiver` 실행 파일 삭제 (`make clean` 또는 수동 삭제)
- [ ] `input.bin`, `output.bin` 데이터 파일 삭제
- [ ] `CODE_EXPLANATION.md` 설명 문서 삭제 (최종보고서에 포함)

### 제출 파일 확인
- [ ] `sender.c` - 송신 프로그램 소스
- [ ] `receiver.c` - 수신 프로그램 소스
- [ ] `Makefile` - 빌드 설정
- [ ] `run_sender.sh`, `run_receiver.sh` - 실행 스크립트 (선택)

### 데모 영상 확인
- [ ] 코드 설명 (sender.c, receiver.c 주요 부분)
- [ ] 시나리오 1: 오류 없는 경우
- [ ] 시나리오 2: 3-Dup ACK 사건
- [ ] 시나리오 3: 타임아웃 사건
- [ ] 결과 설명 (전송 통계)

### 최종보고서 확인
- [ ] 주요 코드 부분 설명
- [ ] 코드에 대한 설명
- [ ] 시나리오별 실행 화면 캡쳐 이미지
- [ ] 실행 결과에 대한 설명

---

## 🎯 데모 영상 제작 팁

### 화면 구성
- 왼쪽: 코드 편집기 (코드 설명 시)
- 오른쪽: 터미널 (실행 결과)
- 또는 화면 분할하여 동시에 보여주기

### 설명 순서
1. **코드 설명** (2-3분)
   - sender.c 주요 로직 설명
   - receiver.c 주요 로직 설명
   - TCP Reno 혼잡제어 알고리즘 설명

2. **시나리오별 데모** (각 2-3분)
   - 시나리오 1: 오류 없는 경우
   - 시나리오 2: 3-Dup ACK 사건
   - 시나리오 3: 타임아웃 사건

3. **결과 설명** (1-2분)
   - 전송 통계 설명
   - 주요 관찰 사항

### 강조할 포인트
- **Slow Start**: 지수적 증가 (cwnd += MSS)
- **Congestion Avoidance**: 선형 증가 (cwnd += MSS × (MSS / cwnd))
- **Fast Recovery**: cwnd = ssthresh + 3 × MSS
- **타임아웃**: cwnd = 1 MSS로 초기화
- **바이트 단위 계산**: 패킷 단위가 아닌 바이트 단위

