#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <linux/fb.h>

#define TCP_PORT    5088
#define FRAMEBUFFER_DEVICE "/dev/fb0"
#define WIDTH        640
#define HEIGHT       480

// 프레임버퍼의 정보를 담을 전역 변수
static struct fb_var_screeninfo vinfo;

// YUYV 포맷의 픽셀 데이터를 RGB565 포맷으로 변환하여 프레임버퍼에 쓰는 함수
void display_frame(uint16_t *fbp, uint8_t *data, int width, int height) {
    // 화면 중앙에 영상을 표시하기 위한 오프셋 계산
    int x_offset = (vinfo.xres - width) / 2;
    int y_offset = (vinfo.yres - height) / 2;
    
    // YUYV는 4바이트(Y1, U, Y2, V)가 2개의 픽셀을 표현함
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; x += 2) {
            // YUYV 데이터 추출
            // (y * width + x) * 2 는 Y1의 위치
            uint8_t Y1 = data[(y * width + x) * 2 + 0];
            uint8_t U  = data[(y * width + x) * 2 + 1];
            uint8_t Y2 = data[(y * width + x) * 2 + 2];
            uint8_t V  = data[(y * width + x) * 2 + 3];

            // YUV를 RGB로 변환 (정수 연산으로 최적화 가능하지만, 표준 공식 사용)
            // 소수점 연산은 임베디드에서 느릴 수 있음
            int C1 = Y1 - 16;
            int C2 = Y2 - 16;
            int D = U - 128;
            int E = V - 128;

            // 첫 번째 픽셀 (Y1, U, V) -> (R1, G1, B1)
            int R1 = (298 * C1 + 409 * E + 128) >> 8;
            int G1 = (298 * C1 - 100 * D - 208 * E + 128) >> 8;
            int B1 = (298 * C1 + 516 * D + 128) >> 8;
            
            // 두 번째 픽셀 (Y2, U, V) -> (R2, G2, B2)
            int R2 = (298 * C2 + 409 * E + 128) >> 8;
            int G2 = (298 * C2 - 100 * D - 208 * E + 128) >> 8;
            int B2 = (298 * C2 + 516 * D + 128) >> 8;

            // 0~255 범위를 벗어나는 값 보정
            R1 = R1 < 0 ? 0 : (R1 > 255 ? 255 : R1);
            G1 = G1 < 0 ? 0 : (G1 > 255 ? 255 : G1);
            B1 = B1 < 0 ? 0 : (B1 > 255 ? 255 : B1);
            R2 = R2 < 0 ? 0 : (R2 > 255 ? 255 : R2);
            G2 = G2 < 0 ? 0 : (G2 > 255 ? 255 : G2);
            B2 = B2 < 0 ? 0 : (B2 > 255 ? 255 : B2);
            
            // RGB888을 RGB565 포맷으로 변환 (R:5bit, G:6bit, B:5bit)
            // (R >> 3) << 11 | (G >> 2) << 5 | (B >> 3) 와 동일한 원리
            uint16_t pixel1 = ((R1 & 0xF8) << 8) | ((G1 & 0xFC) << 3) | (B1 >> 3);
            uint16_t pixel2 = ((R2 & 0xF8) << 8) | ((G2 & 0xFC) << 3) | (B2 >> 3);

            // 계산된 픽셀 값을 프레임버퍼 메모리에 직접 쓰기
            long location1 = (x + x_offset) + (y + y_offset) * vinfo.xres;
            long location2 = (x + x_offset + 1) + (y + y_offset) * vinfo.xres;
            *(fbp + location1) = pixel1;
            *(fbp + location2) = pixel2;
        }
    }
}

// 네트워크로부터 요청한 크기의 데이터를 모두 수신하는 함수
// recv()는 한 번에 모든 데이터를 받지 못할 수 있으므로, 모두 받을 때까지 반복해야 함.
int recv_all(int sock, void *data, size_t len) {
    char *p = data; // receive 한 데이터의 현재 위치를 가리키는 포인터
    while (len > 0) { // 아직 맏을 데이터가 있을 때까지 반복
        int received = recv(sock, p, len, 0); // 현재 위치(p) 에서 남은 길이 len 만큼 수신
        if (received <= 0) { // 0이면 연결 종료, -1이면 에러
            perror("recv()");
            return -1; // 에러 처리
        }
        p += received; // 수신한 바이트 receive byte 만큼 포인터와 남은 길이를 갱신 처리
        len -= received;
    }
    return 0;
}


int main(int argc, char** argv) {
	// 통신 소켓 및 구조체 선언
    int listen_sock, client_sock;
    struct sockaddr_in servaddr, cliaddr;
    socklen_t clen = sizeof(cliaddr);
    
    // 1. 프레임버퍼 장치 설정
    int fb_fd = open(FRAMEBUFFER_DEVICE, O_RDWR);
    if (fb_fd == -1) {
        perror("Error opening framebuffer device");
        return 1;
    }

    if (ioctl(fb_fd, FBIOGET_VSCREENINFO, &vinfo) == -1) {
        perror("Error reading variable information");
        close(fb_fd);
        return 1;
    }
    
    long screensize = vinfo.xres * vinfo.yres * vinfo.bits_per_pixel / 8;
    
	// 프레임버퍼를 메모리에 매핑하여 프로그램에서 직접 접근 가능하게 함
    uint16_t *fbp = mmap(0, screensize, PROT_READ | PROT_WRITE, MAP_SHARED, fb_fd, 0);
    if ((intptr_t)fbp == -1) {
        perror("Error mapping framebuffer device to memory");
        close(fb_fd);
        return 1;
    }

    // 수신할 프레임 데이터를 저장할 버퍼 할당
    size_t frame_size = WIDTH * HEIGHT * 2; // YUYV는 픽셀당 2바이트
    uint8_t *buffer = malloc(frame_size);
    if (!buffer) {
        perror("Failed to allocate buffer");
        munmap(fbp, screensize);
        close(fb_fd);
        return 1;
    }

    // 2. TCP 서버 소켓 설정 ---
    if ((listen_sock = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        perror("socket()"); // 소켓 생성
        return -1;
    }

    memset(&servaddr, 0, sizeof(servaddr));
    servaddr.sin_family = AF_INET;
    servaddr.sin_addr.s_addr = htonl(INADDR_ANY); // 모든 IP에서 오는 연결을 허용
    servaddr.sin_port = htons(TCP_PORT);

    if (bind(listen_sock, (struct sockaddr*)&servaddr, sizeof(servaddr)) < 0) {
        perror("bind()"); // 소켓에 주소 할당
        return -1;
    }

    if (listen(listen_sock, 1) < 0) {
        perror("listen()"); // 클라이언트 연결 대기
        return -1;
    }

    // 3. 메인 루프: 클라이언트 연결 대기 및 데이터 수신/출력 ---
    while (1) {
        printf("conect on port %d\n", TCP_PORT);
        
        // 클라이언트의 연결 요청을 수락하고, 통신을 위한 새로운 소켓(client_sock)을 생성
        client_sock = accept(listen_sock, (struct sockaddr*)&cliaddr, &clen);
        if (client_sock < 0) {
            perror("accept()");
            continue; // 에러 발생 시 다음 연결을 다시 기다림
        }

        char client_ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &cliaddr.sin_addr, client_ip, sizeof(client_ip));
        printf("client connect: %s\n", client_ip);

        // 한 클라이언트와 연결이 유지되는 동안 계속해서 프레임을 수신
        while (1) {
            // 한 프레임 전체를 수신
            if (recv_all(client_sock, buffer, frame_size) == -1) {
                printf("client disconnect or err\n");
                break; // 수신 실패 시 내부 루프 탈출
            }

            // 수신한 데이터를 프레임버퍼에 출력
            display_frame(fbp, buffer, WIDTH, HEIGHT);
        }
        
        close(client_sock); // 클라이언트 소켓 닫기
    }

    // 4. 종료 처리
    free(buffer);
    munmap(fbp, screensize);
    close(fb_fd);
    close(listen_sock);

    return 0;
}
