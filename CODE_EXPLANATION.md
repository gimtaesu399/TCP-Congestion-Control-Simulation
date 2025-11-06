# 프로젝트 구조 및 코드 설명

## 📁 프로젝트 폴더 구조

```
computernetwork/
├── 핵심 소스 파일
│   ├── sender.c          # 송신 프로그램 (TCP Reno 혼잡제어 구현)
│   └── receiver.c        # 수신 프로그램 (누적 ACK, 패킷 손실 시뮬레이션)
│
├── 빌드 시스템
│   └── Makefile          # 빌드 설정 (make로 빌드)
│
├── 실행 스크립트
│   ├── run_sender.sh     # 송신 프로그램 실행 스크립트
│   └── run_receiver.sh   # 수신 프로그램 실행 스크립트
│
└── 데이터 파일 (실행 시 생성)
    ├── input.bin         # 송신할 테스트 데이터
    ├── output.bin        # 수신한 데이터 저장 파일
    ├── sender            # 빌드된 송신 실행 파일
    └── receiver          # 빌드된 수신 실행 파일
```

---

## 📄 sender.c 코드 설명

### 1. 헤더 및 데이터 구조

#### 패킷 헤더 구조
```c
typedef struct __attribute__((packed)) {
    uint32_t seq;     // 바이트 오프셋 (sequence number)
    uint32_t len;     // 페이로드 길이
    uint8_t flags;    // bit0: FIN 플래그
} packet_header_t;
```
- **seq**: 패킷의 바이트 오프셋 (누적 바이트 위치)
- **len**: 실제 데이터 길이 (최대 1400 바이트)
- **flags**: FIN 플래그 (전송 완료 신호)

#### ACK 패킷 구조
```c
typedef struct __attribute__((packed)) {
    uint32_t ack;     // 다음 기대 바이트 (누적 ACK)
    uint8_t dup;      // 중복 카운터 (미사용)
} ack_packet_t;
```
- **ack**: 수신자가 기대하는 다음 바이트 위치 (누적 ACK)

#### 세그먼트 구조
```c
typedef struct {
    uint32_t seq;              // 바이트 오프셋
    uint32_t len;              // 데이터 길이
    uint8_t data[MAX_PAYLOAD]; // 실제 데이터
    bool sent;                 // 전송 여부
    bool acked;                // ACK 수신 여부
} segment_t;
```

### 2. 주요 함수

#### `send_segment()` - 세그먼트 전송
```c
static void send_segment(uint32_t idx, segment_t *segments, int sockfd, 
                         struct sockaddr_in *peer, bool is_retransmit, bool has_timer)
```
- **역할**: 세그먼트를 UDP 패킷으로 전송
- **동작**:
  1. 패킷 헤더 생성 (seq, len, flags)
  2. 헤더 + 데이터를 버퍼에 복사
  3. `sendto()`로 UDP 전송
  4. 전송 로그 출력

#### `send_fin_packet()` - FIN 패킷 전송
```c
static void send_fin_packet(uint32_t seq_cursor, int sockfd, struct sockaddr_in *peer)
```
- **역할**: 전송 완료 신호 (FIN) 전송
- **동작**: len=0, flags=FIN인 패킷 전송

### 3. 메인 로직 흐름

#### 초기화 단계
```c
// 1. 파일 읽기
FILE *fin = fopen(input_path, "rb");
// 파일을 MSS 단위로 세그먼트 분할
while (fread(seg->data, 1, mss, fin) > 0) {
    seg->seq = seq_cursor;
    seg->len = 읽은 바이트 수;
    seq_cursor += seg->len;
}

// 2. 혼잡제어 상태 초기화
double cwnd = (double)mss;      // 초기값: 1 MSS (1500 바이트)
double ssthresh = 65536.0;      // 초기 임계값: 65536 바이트
bool in_fast_recovery = false;  // Fast Recovery 상태 추적
uint32_t dup_ack_count = 0;     // 중복 ACK 카운터
```

#### 메인 루프 - 패킷 전송 및 ACK 처리

**1단계: cwnd 크기만큼 패킷 전송**
```c
while (outstanding_bytes < (uint32_t)cwnd && next_idx < seg_cnt) {
    if (!segments[next_idx].sent) {
        send_segment(next_idx, segments, sockfd, &peer, false, ...);
    }
    outstanding_bytes += segments[next_idx].len;
    next_idx++;
}
```
- **cwnd (혼잡 윈도우) 크기만큼** ACK 없이 패킷 전송
- 바이트 단위로 계산하여 전송 가능한 패킷 수 결정

**2단계: ACK 대기 또는 타임아웃 처리**
```c
select(sockfd + 1, &rfds, NULL, NULL, &tv);
```

**3단계: 타임아웃 처리**
```c
if (ready == 0) {  // 타임아웃 발생
    ssthresh = cwnd / 2.0;      // 임계값 = 현재 cwnd의 절반
    cwnd = (double)mss;         // cwnd = 1 MSS로 초기화
    in_fast_recovery = false;   // Slow Start로 복귀
    // 가장 오래된 미확인 패킷 재전송
}
```

**4단계: 새로운 ACK 수신 처리**
```c
if (ack_seq > last_acked_seq) {  // 새로운 ACK
    // ACK된 세그먼트 표시
    while (segments[base_idx].seq + segments[base_idx].len <= ack_seq) {
        segments[base_idx].acked = true;
        base_idx++;
    }
    
    if (in_fast_recovery) {
        // Fast Recovery 종료
        cwnd = ssthresh;
        in_fast_recovery = false;
        // 바로 Congestion Avoidance 적용
        cwnd += MSS × (MSS / cwnd);
    } else {
        // Normal congestion control
        if (cwnd < ssthresh) {
            // Slow Start: cwnd += MSS (지수적 증가)
            cwnd += (double)mss * (double)acked_packets;
        } else {
            // Congestion Avoidance: cwnd += MSS × (MSS / cwnd) (선형 증가)
            cwnd += (double)mss * ((double)mss / cwnd) * (double)acked_packets;
        }
    }
}
```

**5단계: 중복 ACK 처리**
```c
else if (ack_seq == last_acked_seq) {  // 중복 ACK
    dup_ack_count++;
    
    if (in_fast_recovery) {
        // Fast Recovery 중: 중복 ACK마다 cwnd += MSS
        cwnd += (double)mss;
    } else if (dup_ack_count >= 3) {
        // Fast Retransmit: 3중복 ACK 발생
        ssthresh = cwnd / 2.0;
        cwnd = ssthresh + 3.0 * (double)mss;  // Fast Recovery 시작
        in_fast_recovery = true;
        // 가장 오래된 미확인 패킷 재전송
    }
}
```

### 4. TCP Reno 혼잡제어 알고리즘

#### Slow Start (cwnd < ssthresh)
- **조건**: `cwnd < ssthresh`
- **동작**: `cwnd += MSS` (ACK당)
- **효과**: 지수적 증가 (RTT마다 2배)

#### Congestion Avoidance (cwnd >= ssthresh)
- **조건**: `cwnd >= ssthresh`
- **동작**: `cwnd += MSS × (MSS / cwnd)` (ACK당)
- **효과**: 선형 증가 (RTT마다 1 MSS)

#### Fast Retransmit (3-Dup ACK)
- **조건**: 중복 ACK 3개 수신
- **동작**:
  - `ssthresh = cwnd / 2`
  - `cwnd = ssthresh + 3 × MSS`
  - Fast Recovery 상태 진입

#### Fast Recovery
- **중복 ACK 수신**: `cwnd += MSS`
- **새로운 ACK 수신**: `cwnd = ssthresh`, Congestion Avoidance로 전환

#### Timeout
- **동작**:
  - `ssthresh = cwnd / 2`
  - `cwnd = 1 MSS`
  - Slow Start로 복귀

---

## 📄 receiver.c 코드 설명

### 1. 헤더 및 데이터 구조

sender.c와 동일한 패킷 헤더 구조 사용:
- `packet_header_t`: seq, len, flags
- `ack_packet_t`: ack (누적 ACK)

### 2. 주요 함수

#### `should_drop()` - 패킷 손실 시뮬레이션
```c
static bool should_drop(double loss_probability)
```
- **역할**: 랜덤 패킷 손실 시뮬레이션
- **사용**: 테스트를 위한 패킷 드롭

### 3. 메인 로직 흐름

#### 초기화 단계
```c
// 1. 소켓 생성 및 바인딩
int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
bind(sockfd, (struct sockaddr *)&addr, sizeof(addr));

// 2. 출력 파일 열기 (선택적)
FILE *fout = NULL;
if (save_to_file) {
    fout = fopen(output_path, "wb");
}

// 3. 수신 상태 초기화
uint32_t expected_seq = 0;  // 다음 기대 바이트 위치
```

#### 메인 루프 - 패킷 수신 및 ACK 전송

**1단계: 패킷 수신**
```c
recvfrom(sockfd, buffer, sizeof(buffer), 0, (struct sockaddr *)&peer, &peerlen);
```

**2단계: 패킷 손실 시뮬레이션**
```c
if (use_force_drop && seq == force_drop_seq) {
    // 특정 seq 패킷 강제 드롭 (데모용)
    should_drop_packet = true;
} else if (!use_force_drop && should_drop(loss_prob)) {
    // 랜덤 패킷 드롭
    should_drop_packet = true;
}

if (should_drop_packet) {
    // 패킷 드롭, 누적 ACK 전송
    ack.ack = htonl(expected_seq);
    sendto(sockfd, &ack, sizeof(ack), 0, ...);
    continue;
}
```

**3단계: In-Order 데이터 처리**
```c
if (seq == expected_seq && len > 0) {
    // 순서대로 도착한 패킷
    if (save_to_file && fout) {
        fwrite(buffer + sizeof(hdr), 1, len, fout);  // 파일에 저장
    }
    expected_seq += len;  // 다음 기대 바이트 위치 업데이트
    total_bytes += len;
}
```

**4단계: 누적 ACK 전송**
```c
ack_packet_t ack = {0};
ack.ack = htonl(expected_seq);  // 다음 기대 바이트 위치
sendto(sockfd, &ack, sizeof(ack), 0, ...);
```

**5단계: FIN 패킷 처리**
```c
if (flags & 0x01) {  // FIN 플래그
    fin_received = true;
    // FIN 수신 후 종료
}
```

### 4. 수신자 동작 특징

1. **누적 ACK (Cumulative ACK)**: 다음 기대 바이트 위치를 ACK로 전송
2. **In-Order 수신**: 순서대로 도착한 패킷만 처리
3. **Out-of-Order 무시**: 순서가 맞지 않은 패킷은 무시하고 누적 ACK 전송
4. **패킷 손실 시뮬레이션**: 랜덤 또는 강제 드롭으로 혼잡 사건 발생

---

## 🔄 전체 동작 흐름

### 1. 초기화
```
Sender: 파일 읽기 → 세그먼트 분할 → 소켓 생성
Receiver: 소켓 생성 → 바인딩 → 대기
```

### 2. 전송 단계
```
Sender: cwnd 크기만큼 패킷 전송
Receiver: 패킷 수신 → In-Order 확인 → 파일 저장 → ACK 전송
```

### 3. 혼잡제어
```
Slow Start → Congestion Avoidance
혼잡 사건 발생 → Fast Recovery 또는 Slow Start 복귀
```

### 4. 완료
```
Sender: FIN 패킷 전송
Receiver: FIN 수신 → 종료
```

---

## 📊 주요 알고리즘 요약

### TCP Reno 혼잡제어

| 상태 | 조건 | cwnd 증가 방식 |
|------|------|---------------|
| Slow Start | cwnd < ssthresh | cwnd += MSS (지수적) |
| Congestion Avoidance | cwnd >= ssthresh | cwnd += MSS × (MSS / cwnd) (선형) |
| Fast Recovery | 3-Dup ACK 후 | 중복 ACK마다 cwnd += MSS |

### 혼잡 사건 처리

| 사건 | ssthresh | cwnd | 상태 전이 |
|------|----------|------|----------|
| Timeout | cwnd / 2 | 1 MSS | Slow Start |
| 3-Dup ACK | cwnd / 2 | ssthresh + 3×MSS | Fast Recovery |

---

## 🎯 핵심 구현 포인트

1. **바이트 단위 cwnd 계산**: 패킷 단위가 아닌 바이트 단위로 계산
2. **슬라이딩 윈도우**: cwnd 크기만큼 ACK 없이 전송
3. **누적 ACK**: 수신자가 다음 기대 바이트 위치를 ACK로 전송
4. **타임아웃 처리**: select()로 RTO 시간 동안 대기
5. **Fast Recovery**: 중복 ACK를 받을 때마다 cwnd 증가

