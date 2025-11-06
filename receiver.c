#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define MAX_PAYLOAD 1400

typedef struct __attribute__((packed)) {
    uint32_t seq;     // sequence number (byte offset)
    uint32_t len;     // payload length
    uint8_t flags;    // bit 0: FIN
} packet_header_t;

typedef struct __attribute__((packed)) {
    uint32_t ack;     // next expected byte (cumulative ACK)
    uint8_t dup;      // duplicate counter hint (unused by receiver)
} ack_packet_t;

static uint32_t rand32(void) {
    return ((uint32_t)rand() << 1) ^ (uint32_t)rand();
}

static bool should_drop(double loss_probability) {
    if (loss_probability <= 0.0) return false;
    if (loss_probability >= 1.0) return true;
    uint32_t r = rand32();
    double u = (double)r / (double)UINT32_MAX;
    return u < loss_probability;
}

static void die(const char *msg) {
    perror(msg);
    exit(EXIT_FAILURE);
}

int main(int argc, char **argv) {
    if (argc < 3 || argc > 5) {
        fprintf(stderr, "사용법: %s <수신_포트> <출력파일|-> [손실확률 0.0-1.0] [강제드롭_seq]\n", argv[0]);
        fprintf(stderr, "예시: %s 9000 output.bin 0.05\n", argv[0]);
        fprintf(stderr, "예시: %s 9000 - 0.05  (파일 저장 안 함)\n", argv[0]);
        fprintf(stderr, "예시: %s 9000 output.bin 0 7000  (seq 7000 패킷 강제 드롭)\n", argv[0]);
        return EXIT_FAILURE;
    }

    int listen_port = atoi(argv[1]);
    const char *output_path = argv[2];
    double loss_prob = 0.0;
    uint32_t force_drop_seq = 0;
    bool use_force_drop = false;
    
    if (argc >= 4) {
        loss_prob = atof(argv[3]);
        if (loss_prob < 0.0) loss_prob = 0.0;
        if (loss_prob > 1.0) loss_prob = 1.0;
    }
    if (argc == 5) {
        force_drop_seq = (uint32_t)atoi(argv[4]);
        use_force_drop = true;
    }

    srand((unsigned)time(NULL));

    printf("=== 수신 프로그램 시작 ===\n");
    printf("수신 포트: %d\n", listen_port);
    bool save_to_file = (strcmp(output_path, "-") != 0);
    if (save_to_file) {
        printf("출력 파일: %s\n", output_path);
    } else {
        printf("출력 파일: 저장 안 함 (화면 출력만)\n");
    }
    if (use_force_drop) {
        printf("강제 패킷 드롭: seq %u 패킷 드롭\n", force_drop_seq);
    } else if (loss_prob > 0.0) {
        printf("패킷 손실 시뮬레이션: %.1f%%\n", loss_prob * 100.0);
    } else {
        printf("패킷 손실 시뮬레이션: 없음\n");
    }
    printf("대기 중...\n");

    FILE *fout = NULL;
    if (save_to_file) {
        fout = fopen(output_path, "wb");
        if (!fout) {
            fprintf(stderr, "오류: 출력 파일을 생성할 수 없습니다: %s\n", output_path);
            exit(EXIT_FAILURE);
        }
    }

    int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) {
        fprintf(stderr, "오류: 소켓 생성 실패\n");
        exit(EXIT_FAILURE);
    }

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((uint16_t)listen_port);
    if (bind(sockfd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "오류: 포트 바인딩 실패 (포트 %d)\n", listen_port);
        exit(EXIT_FAILURE);
    }
    printf("포트 %d에서 수신 대기 중...\n", listen_port);

    uint32_t expected_seq = 0; // cumulative in-order byte offset expected
    bool fin_received = false;
    uint32_t total_packets = 0;
    uint32_t dropped_packets = 0;
    uint32_t out_of_order_packets = 0;
    uint32_t total_bytes = 0;

    uint8_t buffer[sizeof(packet_header_t) + MAX_PAYLOAD];

    printf("----------------------------------------\n");
    while (1) {
        struct sockaddr_in peer = {0};
        socklen_t peerlen = sizeof(peer);
        ssize_t n = recvfrom(sockfd, buffer, sizeof(buffer), 0, (struct sockaddr *)&peer, &peerlen);
        if (n < 0) {
            if (errno == EINTR) continue;
            die("recvfrom");
        }
        if ((size_t)n < sizeof(packet_header_t)) {
            // ignore malformed
            continue;
        }

        packet_header_t hdr;
        memcpy(&hdr, buffer, sizeof(hdr));
        uint32_t seq = ntohl(hdr.seq);
        uint32_t len = ntohl(hdr.len);
        uint8_t flags = hdr.flags;

        if (sizeof(hdr) + len != (size_t)n) {
            // size mismatch, ignore
            continue;
        }

        total_packets++;

        // 강제 드롭 (데모용)
        bool should_drop_packet = false;
        if (use_force_drop && seq == force_drop_seq) {
            should_drop_packet = true;
        } else if (!use_force_drop && should_drop(loss_prob)) {
            should_drop_packet = true;
        }
        
        // Potentially simulate loss
        if (should_drop_packet) {
            // drop silently
            dropped_packets++;
            printf("---→ 패킷 (seq:%u, size:%u) 손실\n", seq, len);
            // Still send ACK for what we expect (cumulative ACK)
            ack_packet_t ack = {0};
            ack.ack = htonl(expected_seq);
            ack.dup = 0;
            (void)sendto(sockfd, &ack, sizeof(ack), 0, (struct sockaddr *)&peer, peerlen);
            printf("<--- ACK %u 송신\n", expected_seq);
            continue;
        }

        // Accept only in-order data (simple cumulative ACK receiver)
        if (seq == expected_seq && len > 0) {
            printf("---→ 패킷 (seq:%u, size:%u) 수신\n", seq, len);
            if (save_to_file && fout) {
                size_t wrote = fwrite(buffer + sizeof(hdr), 1, len, fout);
                if (wrote != len) {
                    fprintf(stderr, "오류: 파일 쓰기 실패\n");
                    exit(EXIT_FAILURE);
                }
            }
            expected_seq += len;
            total_bytes += len;
        } else if (seq != expected_seq && len > 0) {
            out_of_order_packets++;
            printf("---→ 패킷 (seq:%u, size:%u) 수신 오류\n", seq, len);
        }

        if (flags & 0x01) { // FIN
            printf("---→ 패킷 (seq:%u, FIN) 수신\n", seq);
            fin_received = true;
        }

        // Send cumulative ACK
        ack_packet_t ack = {0};
        ack.ack = htonl(expected_seq);
        ack.dup = 0;
        (void)sendto(sockfd, &ack, sizeof(ack), 0, (struct sockaddr *)&peer, peerlen);
        printf("<--- ACK %u 송신\n", expected_seq);

        if (fin_received) {
            // Once FIN observed, after acknowledging, exit
            printf("----------------------------------------\n");
            printf("FIN 패킷 수신! 전송 완료 신호 확인\n");
            break;
        }
    }

    printf("\n=== 수신 통계 ===\n");
    printf("수신된 데이터: %u 바이트 (%.2f KB)\n", total_bytes, (double)total_bytes / 1024.0);
    printf("총 수신 패킷: %u\n", total_packets);
    printf("드롭된 패킷: %u\n", dropped_packets);
    printf("순서 불일치 패킷: %u\n", out_of_order_packets);
    if (save_to_file) {
        printf("출력 파일: %s\n", output_path);
    } else {
        printf("출력 파일: 저장 안 함\n");
    }
    printf("==================\n");

    if (fout) {
        fclose(fout);
    }
    close(sockfd);
    printf("수신 프로그램 종료\n");
    return 0;
}


