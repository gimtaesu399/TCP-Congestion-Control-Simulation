#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define MAX_PAYLOAD 1400
#define MAX_WINDOW_PACKETS 1024

typedef struct __attribute__((packed)) {
    uint32_t seq;     // byte offset
    uint32_t len;     // payload length
    uint8_t flags;    // bit0: FIN
} packet_header_t;

typedef struct __attribute__((packed)) {
    uint32_t ack;     // next expected byte
    uint8_t dup;
} ack_packet_t;

typedef struct {
    uint32_t seq;
    uint32_t len;
    uint8_t data[MAX_PAYLOAD];
    bool sent;
    bool acked;
} segment_t;

static void die(const char *msg) {
    perror(msg);
    exit(EXIT_FAILURE);
}

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

static void send_segment(uint32_t idx, segment_t *segments, int sockfd, struct sockaddr_in *peer, bool is_retransmit, bool has_timer) {
    packet_header_t hdr;
    hdr.seq = htonl(segments[idx].seq);
    hdr.len = htonl(segments[idx].len);
    hdr.flags = 0;
    uint8_t buffer[sizeof(hdr) + MAX_PAYLOAD];
    memcpy(buffer, &hdr, sizeof(hdr));
    memcpy(buffer + sizeof(hdr), segments[idx].data, segments[idx].len);
    ssize_t n = sendto(sockfd, buffer, sizeof(hdr) + segments[idx].len, 0, (struct sockaddr *)peer, sizeof(*peer));
    if (n < 0) {
        perror("sendto");
        exit(EXIT_FAILURE);
    }
    segments[idx].sent = true;
    
    if (is_retransmit) {
        printf("---→ 패킷 (seq:%u, size:%u) 재전송\n", segments[idx].seq, segments[idx].len);
    } else {
        printf("→ 패킷 (seq:%u, size:%u) 송신", segments[idx].seq, segments[idx].len);
        if (has_timer) {
            printf(" (타이머)");
        }
        printf("\n");
    }
}

static void send_fin_packet(uint32_t seq_cursor, int sockfd, struct sockaddr_in *peer) {
    packet_header_t hdr;
    hdr.seq = htonl(seq_cursor);
    hdr.len = htonl(0);
    hdr.flags = 0x01; // FIN
    (void)sendto(sockfd, &hdr, sizeof(hdr), 0, (struct sockaddr *)peer, sizeof(*peer));
}

int main(int argc, char **argv) {
    if (argc < 5 || argc > 6) {
        fprintf(stderr, "사용법: %s <수신자_IP> <수신자_포트> <입력파일> <MSS_바이트> [RTO_밀리초]\n", argv[0]);
        fprintf(stderr, "예시: %s 127.0.0.1 9000 input.bin 1000 200\n", argv[0]);
        return EXIT_FAILURE;
    }

    const char *receiver_ip = argv[1];
    int receiver_port = atoi(argv[2]);
    const char *input_path = argv[3];
    int mss = atoi(argv[4]);
    if (mss <= 0 || mss > MAX_PAYLOAD) mss = MAX_PAYLOAD;
    int rto_ms = (argc == 6) ? atoi(argv[5]) : 200; // default 200ms retransmission timeout
    if (rto_ms < 50) rto_ms = 50;

    printf("=== 송신 프로그램 시작 ===\n");
    printf("수신자: %s:%d\n", receiver_ip, receiver_port);
    printf("입력 파일: %s\n", input_path);
    printf("MSS: %d 바이트\n", mss);
    printf("RTO: %d 밀리초\n", rto_ms);
    printf("파일 읽는 중...\n");

    FILE *fin = fopen(input_path, "rb");
    if (!fin) {
        fprintf(stderr, "오류: 입력 파일을 열 수 없습니다: %s\n", input_path);
        exit(EXIT_FAILURE);
    }

    // Read file into segments
    segment_t *segments = NULL;
    size_t seg_cap = 0, seg_cnt = 0;
    uint32_t seq_cursor = 0;
    while (1) {
        if (seg_cnt == seg_cap) {
            seg_cap = seg_cap ? seg_cap * 2 : 1024;
            segments = (segment_t *)realloc(segments, seg_cap * sizeof(segment_t));
            if (!segments) die("realloc");
        }
        segment_t *seg = &segments[seg_cnt];
        seg->seq = seq_cursor;
        seg->len = (uint32_t)fread(seg->data, 1, (size_t)mss, fin);
        seg->sent = false;
        seg->acked = false;
        if (seg->len == 0) break;
        seq_cursor += seg->len;
        seg_cnt++;
    }
    fclose(fin);
    printf("파일 읽기 완료: 총 %zu 세그먼트 (%u 바이트)\n", seg_cnt, seq_cursor);
    printf("소켓 설정 중...\n");

    // Socket setup
    int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) {
        fprintf(stderr, "오류: 소켓 생성 실패\n");
        exit(EXIT_FAILURE);
    }
    struct sockaddr_in peer = {0};
    peer.sin_family = AF_INET;
    peer.sin_port = htons((uint16_t)receiver_port);
    if (inet_pton(AF_INET, receiver_ip, &peer.sin_addr) != 1) {
        fprintf(stderr, "오류: 잘못된 IP 주소: %s\n", receiver_ip);
        exit(EXIT_FAILURE);
    }
    printf("전송 시작!\n");
    printf("----------------------------------------\n");

    // Congestion control state (TCP Reno) - 바이트 단위
    double cwnd = (double)mss;    // 초기값: 1 MSS (바이트 단위)
    double ssthresh = 65536.0;    // 초기 임계값: 65536 바이트 (임의 설정)
    uint32_t base_idx = 0;       // oldest unacked segment index
    uint32_t next_idx = 0;       // next segment index to send
    uint32_t last_acked_seq = 0; // last cumulatively acked byte
    uint32_t dup_ack_count = 0;
    bool in_fast_recovery = false; // Fast Recovery 상태 추적
    double timer_start_ms = -1.0;
    uint32_t total_retransmits = 0;
    uint32_t timeout_count = 0;
    uint32_t dup_ack_retransmits = 0;
    double start_time = now_ms();

    // Main loop
    while (base_idx < seg_cnt) {
        // Send as much as allowed by cwnd (바이트 단위)
        uint32_t outstanding_bytes = 0;
        for (uint32_t i = base_idx; i < next_idx && i < seg_cnt; i++) {
            outstanding_bytes += segments[i].len;
        }
        while (outstanding_bytes < (uint32_t)cwnd && next_idx < seg_cnt) {
            if (!segments[next_idx].sent) {
                // normal send
                send_segment(next_idx, segments, sockfd, &peer, false, timer_start_ms >= 0.0);
            }
            if (timer_start_ms < 0.0) timer_start_ms = now_ms();
            outstanding_bytes += segments[next_idx].len;
            next_idx++;
        }

        // Wait for ACKs or timeout
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(sockfd, &rfds);

        struct timeval tv;
        int wait_ms = rto_ms;
        if (timer_start_ms >= 0.0) {
            double elapsed = now_ms() - timer_start_ms;
            if (elapsed >= (double)rto_ms) {
                wait_ms = 0; // trigger timeout immediately
            } else {
                wait_ms = (int)((double)rto_ms - elapsed);
            }
        }
        tv.tv_sec = wait_ms / 1000;
        tv.tv_usec = (wait_ms % 1000) * 1000;

        int ready = select(sockfd + 1, &rfds, NULL, NULL, &tv);
        if (ready < 0) {
            if (errno == EINTR) continue;
            die("select");
        } else if (ready == 0) {
            // timeout -> loss detected
            timeout_count++;
            total_retransmits++;
            printf("<<<타임아웃 사건 발생>>>\n");
            // 현재 cwnd의 절반을 ssthresh로 설정
            ssthresh = cwnd / 2.0;
            if (ssthresh < (double)mss) ssthresh = (double)mss;
            cwnd = (double)mss; // 1 MSS로 초기화
            // Slow Start로 시작하기 위해 ssthresh가 cwnd보다 크도록 보장
            if (ssthresh <= (double)mss) ssthresh = (double)mss * 2.0;
            in_fast_recovery = false;
            printf("- cwin: %.0f 바이트 (1 MSS)로 조정\n", cwnd);
            printf("- 임계값: %.0f 바이트로 설정\n", ssthresh);
            // retransmit the oldest unacked segment
            if (base_idx < seg_cnt) {
                segments[base_idx].sent = false; // mark for resend
                send_segment(base_idx, segments, sockfd, &peer, true, false);
            }
            next_idx = base_idx + 1;
            // cwnd 크기만큼만 재전송 표시
            uint32_t resend_bytes = 0;
            for (uint32_t i = base_idx + 1; i < seg_cnt && resend_bytes < (uint32_t)cwnd; ++i) {
                segments[i].sent = false; // mark for resend
                resend_bytes += segments[i].len;
            }
            timer_start_ms = now_ms();
            dup_ack_count = 0;
            continue;
        }

        if (FD_ISSET(sockfd, &rfds)) {
            ack_packet_t ack;
            ssize_t n = recv(sockfd, &ack, sizeof(ack), 0);
            if (n < 0) die("recv ack");
            if ((size_t)n < sizeof(ack)) continue;
            uint32_t ack_seq = ntohl(ack.ack);

            if (ack_seq > last_acked_seq) {
                // New ACK
                uint32_t old_acked_seq = last_acked_seq;
                last_acked_seq = ack_seq;
                
                // Mark segments acked
                uint32_t old_base = base_idx;
                while (base_idx < seg_cnt) {
                    uint32_t seg_end = segments[base_idx].seq + segments[base_idx].len;
                    if (seg_end <= ack_seq) {
                        segments[base_idx].acked = true;
                        base_idx++;
                    } else {
                        break;
                    }
                }

                // Advance congestion window (바이트 단위)
                double old_cwnd = cwnd;
                uint32_t acked_packets = base_idx - old_base;
                
                if (in_fast_recovery) {
                    // Fast Recovery 종료: 새로운 ACK를 받으면 inflight를 고려하여 cwnd 조정
                    // inflight를 계산하여 cwnd를 적절히 조정 (안 채워진 부분을 채워서 비슷한 단위로 유지)
                    uint32_t inflight_bytes = 0;
                    for (uint32_t i = base_idx; i < next_idx && i < seg_cnt; i++) {
                        if (!segments[i].acked) {
                            inflight_bytes += segments[i].len;
                        }
                    }
                    // Fast Recovery 종료 시: cwnd = max(ssthresh, inflight + 3*MSS)
                    // inflight를 고려하여 cwnd를 조정 (다음 데이터 전송 시 자연스럽게 MSS 단위로 맞춰짐)
                    double new_cwnd = (inflight_bytes > 0) ? (double)inflight_bytes + 3.0 * (double)mss : ssthresh;
                    if (new_cwnd < ssthresh) {
                        new_cwnd = ssthresh;
                    }
                    cwnd = new_cwnd;
                    in_fast_recovery = false;
                    dup_ack_count = 0;
                    // Fast Recovery 종료 후 바로 Congestion Avoidance 적용
                    // cwnd >= ssthresh이므로 Congestion Avoidance
                    double before_ca = cwnd;
                    cwnd += (double)mss * ((double)mss / cwnd) * (double)acked_packets;
                    double cwnd_increase = cwnd - before_ca; // Congestion Avoidance로 인한 증가량만 표시
                    printf("<--- ACK %u 수신 (Fast Recovery 종료) => cwin %.0f 바이트 증가(%.0f 바이트, Congestion Avoidance)\n", ack_seq, cwnd_increase, cwnd);
                } else {
                    // Normal congestion control
                    if (cwnd < ssthresh) {
                        // Slow Start: ACK당 +MSS (지수적 증가)
                        // 각 ACK에 대해 cwnd = cwnd + MSS
                        cwnd += (double)mss * (double)acked_packets;
                    } else {
                        // Congestion Avoidance: ACK당 +MSS × (MSS / cwnd) (선형 증가)
                        // 각 ACK에 대해 cwnd = cwnd + MSS × (MSS / cwnd)
                        cwnd += (double)mss * ((double)mss / cwnd) * (double)acked_packets;
                    }
                    double cwnd_increase = cwnd - old_cwnd;
                    printf("<--- ACK %u 수신 => cwin %.0f 바이트 증가(%.0f 바이트)\n", ack_seq, cwnd_increase, cwnd);
                }

                // If all outstanding acked, stop timer
                if (base_idx == next_idx) {
                    timer_start_ms = -1.0;
                } else {
                    timer_start_ms = now_ms();
                }
            } else if (ack_seq == last_acked_seq) {
                // Duplicate ACK
                dup_ack_count++;
                printf("<--- ACK %u 수신 (중복 #%u)\n", ack_seq, dup_ack_count);
                
                if (in_fast_recovery) {
                    // Fast Recovery 중: 중복 ACK를 받을 때마다 cwnd += MSS (새 세그먼트 전송 허용)
                    cwnd += (double)mss;
                    printf("  => Fast Recovery: cwin %.0f 바이트로 증가\n", cwnd);
                } else if (dup_ack_count >= 3 && base_idx < seg_cnt) {
                    // Fast Retransmit: 3중복 ACK를 받으면
                    dup_ack_retransmits++;
                    total_retransmits++;
                    printf("<<< 3-Dup ACK 사건 발생>>>\n");
                    // 현재 cwnd의 절반을 ssthresh로 설정
                    ssthresh = cwnd / 2.0;
                    if (ssthresh < (double)mss) ssthresh = (double)mss;
                    // Fast Recovery: cwnd = ssthresh + 3 * MSS (이미 받은 중복 ACK 3개)
                    cwnd = ssthresh + 3.0 * (double)mss;
                    // ssthresh가 cwnd보다 작도록 보장 (Fast Recovery 후 Congestion Avoidance로 전환)
                    if (ssthresh >= cwnd) ssthresh = cwnd - (double)mss;
                    in_fast_recovery = true;
                    printf("- cwin: ½ + 3×MSS = %.0f 바이트\n", cwnd);
                    printf("- 임계값: %.0f 바이트로 설정\n", ssthresh);
                    // Retransmit oldest unacked
                    segments[base_idx].sent = false;
                    send_segment(base_idx, segments, sockfd, &peer, true, false);
                    next_idx = base_idx + 1;
                    timer_start_ms = now_ms();
                    dup_ack_count = 0; // Fast Recovery 시작 후 리셋
                }
            }
        }
    }

    // Send FIN and wait a bit for final ACK
    printf("----------------------------------------\n");
    printf("전송 완료! FIN 패킷 전송 중...\n");
    send_fin_packet(seq_cursor, sockfd, &peer);
    struct timeval tv2 = { .tv_sec = 0, .tv_usec = 200 * 1000 };
    setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv2, sizeof(tv2));
    ack_packet_t dummy;
    (void)recv(sockfd, &dummy, sizeof(dummy), 0);

    double elapsed = (now_ms() - start_time) / 1000.0;
    double throughput = (double)seq_cursor / elapsed / 1024.0 / 1024.0; // MB/s

    printf("\n=== 전송 통계 ===\n");
    printf("전송된 데이터: %u 바이트 (%.2f KB)\n", seq_cursor, (double)seq_cursor / 1024.0);
    printf("총 세그먼트 수: %zu\n", seg_cnt);
    printf("전송 시간: %.2f 초\n", elapsed);
    printf("처리량: %.2f MB/s\n", throughput);
    printf("타임아웃 횟수: %u\n", timeout_count);
    printf("중복 ACK 재전송: %u\n", dup_ack_retransmits);
    printf("총 재전송 횟수: %u\n", total_retransmits);
    printf("최종 cwnd: %.0f 바이트\n", cwnd);
    printf("최종 ssthresh: %.0f 바이트\n", ssthresh);
    printf("==================\n");

    close(sockfd);
    free(segments);
    return 0;
}


