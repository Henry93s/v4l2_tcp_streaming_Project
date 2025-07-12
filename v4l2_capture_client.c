#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <linux/videodev2.h>

#define TCP_PORT    5088
#define VIDEO_DEVICE "/dev/video0"
#define WIDTH        640
#define HEIGHT       480

// 네트워크를 통해 요청한 크기의 데이터를 모두 보내는 함수
// send()는 한 번에 모든 데이터를 보내지 못할 수 있으므로, 모두 보낼 때까지 반복해야 함.
int send_all(int sock, const void *data, size_t len) {
    const char *p = data; // 전송할 데이터의 현재 위치를 가리키는 포인터
    while (len > 0) { // 아직 보낼 데이터가 남아있는 동안 반복
        // 현재 위치(p)에서 남은 길이(len)만큼 데이터 전송 시도
        int sent = send(sock, p, len, 0);
        if (sent < 0) {
            perror("send()");
            return -1; // 실패 반환
        }
        // 실제로 전송된 바이트(sent)만큼 포인터와 남은 길이를 갱신
        p += sent;
        len -= sent;
    }
    return 0; // 모든 데이터 전송 성공
}

int main(int argc, char** argv) {
    int v4l2_fd;  // V4L2 카메라 디바이스 파일 디스크립터
    int sock_fd;  // 클라이언트 통신 : 클라이언트 소켓 파일 디스크립터
    struct sockaddr_in servaddr; // 클라이언트 통신 : 서버 주소 정보를 담을 구조체
    
    // clietn 실행 시 서버 IP 주소를 인자로 받았는지 확인
    if (argc < 2) {
        printf("Usage: %s <Server IP Address>\n", argv[0]);
        return -1;
    }

    // 1. TCP 클라이언트 소켓 설정
    // AF_INET: IPv4, SOCK_STREAM: TCP 프로토콜 설정
    if ((sock_fd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        perror("socket()");
        return -1;
    }

    // 서버 주소 구조체(servaddr)를 0으로 초기화
    memset(&servaddr, 0, sizeof(servaddr));
    servaddr.sin_family = AF_INET;
    servaddr.sin_port = htons(TCP_PORT);
    // 입력받은 IP 주소를 서버 주소로 설정
    if (inet_pton(AF_INET, argv[1], &servaddr.sin_addr) <= 0) {
        perror("inet_pton()");
        close(sock_fd);
        return -1;
    }
    
    // 서버에 연결 요청
    if (connect(sock_fd, (struct sockaddr*)&servaddr, sizeof(servaddr)) < 0) {
        perror("connect()");
        close(sock_fd);
        return -1;
    }
    printf("Connected to server %s:%d\n", argv[1], TCP_PORT);

    // 2. V4L2 카메라 장치 설정 ---
    if ((v4l2_fd = open(VIDEO_DEVICE, O_RDWR)) == -1) {
        perror("Failed to open video device");
        close(sock_fd);
        return 1;
    }

    // 카메라 포맷 설정 (YUYV, 640x480)
    struct v4l2_format fmt;
    memset(&fmt, 0, sizeof(fmt));
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width = WIDTH;
    fmt.fmt.pix.height = HEIGHT;
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV; // YUYV 포맷 사용
    fmt.fmt.pix.field = V4L2_FIELD_NONE;

    if (ioctl(v4l2_fd, VIDIOC_S_FMT, &fmt) == -1) {
        perror("Failed to set format");
        close(v4l2_fd);
        close(sock_fd);
        return 1;
    }
    
    // 프레임 데이터를 저장할 버퍼 할당 (YUYV는 픽셀당 2바이트)
    size_t frame_size = fmt.fmt.pix.sizeimage;
    char *buffer = malloc(frame_size);
    if (!buffer) {
        perror("Failed to allocate buffer");
        close(v4l2_fd);
        close(sock_fd);
        return 1;
    }
    
    // 3. 메인 루프: 영상 캡처 및 서버로 전송
    while (1) {
        // 카메라에서 한 프레임 읽기
        int ret = read(v4l2_fd, buffer, frame_size);
        if (ret == -1) {
            perror("Failed to read frame");
            break;
        }
        printf("Captured frame size: %d bytes\n", ret);

        // 읽어온 프레임을 서버로 전송
        if (send_all(sock_fd, buffer, frame_size) == -1) {
            fprintf(stderr, "Failed to send frame to server.\n");
            break;
        }
    }

    // 4. 종료 처리
    printf("Closing connection and devices.\n");
    close(sock_fd);
    free(buffer);
    close(v4l2_fd);

    return 0;
}
