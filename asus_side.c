#include <fcntl.h>
#include <linux/uinput.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int ufd;

static void add(struct input_event *ev, int *n, int type, int code, int val)
{
    memset(&ev[*n], 0, sizeof ev[*n]);
    ev[*n].type = type;
    ev[*n].code = code;
    ev[*n].value = val;
    (*n)++;
}

/* 一次 write 送出整組 Ctrl+Shift+<key> */
static void combo(int key)
{
    struct input_event ev[8];
    int n = 0;
    add(ev, &n, EV_KEY, KEY_LEFTCTRL, 1);
    //add(ev, &n, EV_KEY, KEY_LEFTSHIFT, 1);
    add(ev, &n, EV_KEY, key, 1);
    add(ev, &n, EV_SYN, SYN_REPORT, 0);
    add(ev, &n, EV_KEY, key, 0);
    //add(ev, &n, EV_KEY, KEY_LEFTSHIFT, 0);
    add(ev, &n, EV_KEY, KEY_LEFTCTRL, 0);
    add(ev, &n, EV_SYN, SYN_REPORT, 0);
    if (write(ufd, ev, n * sizeof ev[0]) < 0)
        perror("uinput write");
}

int main(int argc, char **argv)
{
    const char *dev = argc > 1 ? argv[1] : "/dev/hidraw4";

    int hfd = open(dev, O_RDONLY);
    if (hfd < 0) { perror(dev); return 1; }

    ufd = open("/dev/uinput", O_WRONLY);
    if (ufd < 0) { perror("/dev/uinput"); return 1; }

    ioctl(ufd, UI_SET_EVBIT, EV_KEY);
    ioctl(ufd, UI_SET_KEYBIT, KEY_LEFTCTRL);
    //ioctl(ufd, UI_SET_KEYBIT, KEY_LEFTSHIFT);
    ioctl(ufd, UI_SET_KEYBIT, KEY_C);
    ioctl(ufd, UI_SET_KEYBIT, KEY_V);

    struct uinput_setup us;
    memset(&us, 0, sizeof us);
    us.id.bustype = BUS_VIRTUAL;
    us.id.vendor = 0x1235;
    us.id.product = 0x5679;
    strcpy(us.name, "asus-side-btn");
    ioctl(ufd, UI_DEV_SETUP, &us);
    ioctl(ufd, UI_DEV_CREATE);

    unsigned char buf[65];
    unsigned char prev = 0;

    for (;;) {
        ssize_t n = read(hfd, buf, sizeof buf);
        if (n < 0) { perror("read"); break; }   /* 拔除滑鼠時離開,交給 systemd 重啟 */
        if (n < 6 || buf[0] != 0x03 || buf[1] != 0x12 || buf[2] != 0x01)
            continue;

        unsigned char cur = buf[5] & 0x1F;      /* 只看按鍵 bit,排除滾輪 bit */
        unsigned char down = cur & ~prev;       /* 這次新按下的鍵 */
        prev = cur;

        if (down & 0x10) combo(KEY_V);          /* 側上鍵 -> Ctrl+Shift+V */
        if (down & 0x08) combo(KEY_C);          /* 側下鍵 -> Ctrl+Shift+C */
    }
    return 1;
}
