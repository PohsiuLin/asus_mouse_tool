/*
 * asus_side.c - ASUS ROG GLADIUS III CORE 側鍵重新對應 (userspace, hidraw -> uinput)
 *
 * 啟動時自動掃描 /dev/hidraw*, 依 VID/PID 與 USB interface 找出側鍵所在的 hidraw,
 * 讀取 vendor report (Report ID 0x03) 後透過 /dev/uinput 注入鍵盤組合鍵。
 *   側上鍵 (0x10) -> Ctrl+V
 *   側下鍵 (0x08) -> Ctrl+C
 *
 * 編譯: gcc -O2 -Wall -o asus_side asus_side.c
 * 執行: sudo ./asus_side             (自動尋找裝置)
 *       sudo ./asus_side /dev/hidraw4 (手動指定裝置)
 */
#include <fcntl.h>
#include <linux/hidraw.h>
#include <linux/uinput.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define MOUSE_VID      0x0b05
#define MOUSE_PID      0x1c8d
#define PHYS_SUFFIX    "/input2"   /* HID_PHYS 結尾: 側鍵 report 在 USB interface 02 */

#define REPORT_ID      0x03
#define BTN_SIDE_DOWN  0x08
#define BTN_SIDE_UP    0x10
#define BTN_MASK       0x1F        /* bit0~4 為按鍵; bit6/7 為滾輪, 排除 */

static int ufd;

/* 掃描 hidraw, 回傳側鍵所在裝置的 fd; 找不到回傳 -1 */
static int find_hidraw(void)
{
    char path[32];
    for (int i = 0; i < 32; i++) {
        snprintf(path, sizeof path, "/dev/hidraw%d", i);
        int fd = open(path, O_RDONLY);
        if (fd < 0)
            continue;

        struct hidraw_devinfo di;
        char phys[128] = {0};
        if (ioctl(fd, HIDIOCGRAWINFO, &di) == 0 &&
            (unsigned short)di.vendor == MOUSE_VID &&
            (unsigned short)di.product == MOUSE_PID &&
            ioctl(fd, HIDIOCGRAWPHYS(sizeof phys - 1), phys) >= 0) {
            size_t sl = strlen(PHYS_SUFFIX), pl = strlen(phys);
            if (pl >= sl && strcmp(phys + pl - sl, PHYS_SUFFIX) == 0) {
                fprintf(stderr, "using %s (%s)\n", path, phys);
                return fd;
            }
        }
        close(fd);
    }
    return -1;
}

static void add(struct input_event *ev, int *n, int type, int code, int val)
{
    memset(&ev[*n], 0, sizeof ev[*n]);
    ev[*n].type = type;
    ev[*n].code = code;
    ev[*n].value = val;
    (*n)++;
}

/* 一次 write 送出 Ctrl+<key>, 減少 syscall 與延遲 */
static void combo(int key)
{
    struct input_event ev[6];
    int n = 0;
    add(ev, &n, EV_KEY, KEY_LEFTCTRL, 1);
    add(ev, &n, EV_KEY, key, 1);
    add(ev, &n, EV_SYN, SYN_REPORT, 0);
    add(ev, &n, EV_KEY, key, 0);
    add(ev, &n, EV_KEY, KEY_LEFTCTRL, 0);
    add(ev, &n, EV_SYN, SYN_REPORT, 0);
    if (write(ufd, ev, n * sizeof ev[0]) < 0)
        perror("uinput write");
}

int main(int argc, char **argv)
{
    int hfd;
    if (argc > 1) {
        hfd = open(argv[1], O_RDONLY);
        if (hfd < 0) { perror(argv[1]); return 1; }
    } else {
        hfd = find_hidraw();
        if (hfd < 0) {
            fprintf(stderr, "ROG GLADIUS III CORE (interface 02) not found\n");
            return 1;
        }
    }

    ufd = open("/dev/uinput", O_WRONLY);
    if (ufd < 0) { perror("/dev/uinput"); return 1; }

    ioctl(ufd, UI_SET_EVBIT, EV_KEY);
    ioctl(ufd, UI_SET_KEYBIT, KEY_LEFTCTRL);
    ioctl(ufd, UI_SET_KEYBIT, KEY_C);
    ioctl(ufd, UI_SET_KEYBIT, KEY_V);

    struct uinput_setup us;
    memset(&us, 0, sizeof us);
    us.id.bustype = BUS_VIRTUAL;
    us.id.vendor  = 0x1234;
    us.id.product = 0x5678;
    strcpy(us.name, "asus-side-btn");
    ioctl(ufd, UI_DEV_SETUP, &us);
    ioctl(ufd, UI_DEV_CREATE);

    unsigned char buf[64];
    unsigned char prev = 0;

    for (;;) {
        ssize_t n = read(hfd, buf, sizeof buf);   /* 阻塞等待, 閒置 CPU 0% */
        if (n < 0) { perror("read"); break; }     /* 滑鼠拔除 -> 離開, 由 systemd 重啟並重新掃描 */
        if (n < 6 || buf[0] != REPORT_ID || buf[1] != 0x12 || buf[2] != 0x01)
            continue;

        unsigned char cur  = buf[5] & BTN_MASK;
        unsigned char down = cur & ~prev;         /* 本次新按下的鍵 */
        prev = cur;

        if (down & BTN_SIDE_UP)   combo(KEY_V);   /* 側上鍵 -> Ctrl+V */
        if (down & BTN_SIDE_DOWN) combo(KEY_C);   /* 側下鍵 -> Ctrl+C */
    }

    ioctl(ufd, UI_DEV_DESTROY);
    close(ufd);
    close(hfd);
    return 1;
}