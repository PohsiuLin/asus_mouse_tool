# ASUS 滑鼠側鍵 Linux 客製化:hidraw → uinput 研究報告

在 Linux 上,ASUS 滑鼠的側鍵沒有官方支援,現有工具也抓不到。本專案記錄從「抓不到側鍵」到「userspace 小程式重新對應側鍵」的完整過程,並附上最終程式碼。

## 目標

| 按鍵 | 目標行為 |
|---|---|
| 側上鍵 | 貼上 (Ctrl+V) |
| 側下鍵 | 複製 (Ctrl+C) |

需求:背景常駐、最輕量、延遲最低。

## 結論摘要

- **不需要寫 kernel driver。** `/dev/hidrawN` 已提供 raw HID report 給 userspace 讀取,userspace 程式就足夠。
- 此滑鼠的側鍵走 **vendor-specific report(Report ID 0x03)**,不走標準滑鼠 report,所以一般 evdev 工具看不到。
- 最終方案:C 語言阻塞式 `read(hidraw)` → `write(uinput)` 注入鍵盤事件,由 systemd 常駐。實測常駐記憶體約 172 KB,閒置 CPU 約 0。

## 架構

```
ASUS 滑鼠 ──USB──> /dev/hidrawN ──read()──> asus_side ──write()──> /dev/uinput ──> 虛擬鍵盤 asus-side-btn ──> 桌面/應用程式
                                              (解析 report)
```

## 環境

- OS:Linux(systemd)
- 裝置:ASUS 滑鼠,VID `0b05`(ASUS),介面 `/dev/hidraw4`
- 工具:gcc、hexdump、evtest

## 研究過程

### 1. 找到對的 interface

ASUS 滑鼠通常會開多個 HID interface。用下列指令確認:

```bash
cat /sys/class/hidraw/hidraw4/device/uevent
sudo evtest                     # 確認標準 input 層看不到側鍵
sudo hexdump -C /dev/hidraw4    # 直接看 raw report
```

hidraw4 在按側鍵時有輸出,代表側鍵在這個 interface。

### 2. Report 格式分析

`hexdump` 會把連續 report 串在一起。`03` 開頭的 report 出現在 offset 0x00、0x15、0x2A、0x3F…,間隔固定 0x15,所以**每個 report 固定 21 bytes**,`read()` 一次回傳一個 report。

Report ID `0x03` 格式:

```
byte: 0   1   2   3   4   5     6..20
      03  12  01  00  00  MASK  00...
```

`byte[5]` 為 bitmask:

| byte[5] | 意義 | 型態 |
|---|---|---|
| 0x01 | 左鍵 | level(放開回 0x00) |
| 0x02 | 右鍵 | level |
| 0x04 | 中鍵 | level |
| 0x08 | **側下鍵** | level |
| 0x10 | **側上鍵** | level |
| 0x40 | 滾輪向上 | pulse(無放開事件) |
| 0x80 | 滾輪向下 | pulse |
| 0x00 | 全部放開 | |

要點:
- 按鍵為 level 型態,需與上一次狀態比較才能找出「按下」邊緣。
- 滾輪為 pulse 型態,每個 report 算一次 tick。
- 程式只取 `byte[5] & 0x1F`,排除滾輪 bit。
- 另有 Report ID `0x04` 的 report(`04 01 00 ...` / `04 00 00 ...`),用途**尚未確認**,本專案未使用。

### 3. 實作

核心流程(完整程式見 [`asus_side.c`](asus_side.c)):

1. 開啟 `/dev/hidrawN`(唯讀)與 `/dev/uinput`。
2. 建立虛擬鍵盤 `asus-side-btn`。
3. 迴圈阻塞 `read()`,比對前後狀態找出新按下的側鍵。
4. 用**單次 `write()`** 送出整組「Ctrl 按下 → 鍵按下 → SYN → 鍵放開 → Ctrl 放開 → SYN」,減少 syscall 次數。

選 C 而不是 Python 的理由:無 interpreter 啟動與執行開銷、阻塞讀取不需 polling、常駐記憶體僅數百 KB。

### 4. 組合鍵的選擇(遇到的問題)

| 版本 | 結果 |
|---|---|
| 上鍵 Ctrl+Shift+V、下鍵 Ctrl+Shift+C | 上鍵可貼上;**下鍵在一般應用程式無法複製** |
| 上鍵 Ctrl+V、下鍵 Ctrl+C(最終版) | **實測成功**,一般應用程式可正常貼上/複製 |

原因分析:Ctrl+Shift+C 只有終端機模擬器視為「複製」;在瀏覽器等程式是「檢查元素」或無作用。Ctrl+Shift+V 則恰好被許多程式當成「貼上純文字」,所以上鍵看起來正常。

**代價**:在終端機中,Ctrl+C 是 SIGINT(中斷正在執行的程式)。側下鍵在終端機按下會中斷前景程式,而不是複製。

替代方案(未採用 / 可選):
- 下鍵 Ctrl+Insert、上鍵 Shift+Insert:在終端機與多數 GUI 程式都是複製/貼上,不必判斷視窗。
- 依焦點視窗切換組合鍵(X11 可用 xdotool,Wayland 較複雜):會增加延遲與複雜度。

## 安裝與使用

```bash
# 1. 編譯與安裝
gcc -O2 -o asus_side asus_side.c
sudo install -m 755 asus_side /usr/local/bin/

# 2. 安裝 service
sudo cp asus-side.service /etc/systemd/system/
sudo modprobe uinput
echo uinput | sudo tee /etc/modules-load.d/uinput.conf   # 開機自動載入
sudo systemctl daemon-reload
sudo systemctl enable --now asus-side

# 3. 檢查
systemctl status asus-side
journalctl -u asus-side -e
```

> `asus-side.service` 內的 `/dev/hidraw4` 請依你的裝置調整。service 以 root 執行,因此不需另外處理 hidraw / uinput 權限。

### 固定裝置路徑(建議)

hidraw 編號會在重開機或重插後改變。可用 udev rule 依 VID:PID 建立固定 symlink:

```bash
lsusb                                    # 查出滑鼠 PID
# 編輯 99-asus-mouse.rules, 把 xxxx 換成 PID
sudo cp 99-asus-mouse.rules /etc/udev/rules.d/
sudo udevadm control --reload && sudo udevadm trigger
ls -l /dev/asus_mouse
```

再把 service 的 `ExecStart` 改為 `/usr/local/bin/asus_side /dev/asus_mouse`。同一支滑鼠有多個 hidraw interface,symlink 可能對到錯誤的 interface,必要時在 rule 加上 `ATTRS{bInterfaceNumber}` 限定。

## 已知限制與待辦

- **原生側鍵事件可能重複觸發**:滑鼠的標準 input interface 可能仍會送出上一頁/下一頁(`BTN_SIDE`/`BTN_EXTRA`)。可用 `sudo evtest` 選滑鼠本體確認;若有,需對該 evdev 裝置使用 `EVIOCGRAB` 獨佔。本專案尚未實作。
- **Report ID 0x04** 的用途未確認(可能是 DPI 鍵或其他附帶事件)。
- **bit5(0x20)** 未出現於測試 dump,可能對應其他按鍵,待確認。
- 目前綁定單一裝置路徑,拔除滑鼠時程式離開,由 systemd 每 2 秒重試。
- 已在上述環境實測:hidraw 偵測正常,側上鍵 Ctrl+V、側下鍵 Ctrl+C 均可正常貼上/複製。

## 檔案

| 檔案 | 說明 |
|---|---|
| `asus_side.c` | 主程式 |
| `asus-side.service` | systemd 常駐設定 |
| `99-asus-mouse.rules` | udev 固定 symlink 範本 |

## 參考

- Linux HID raw 介面:`hidraw`
- Linux uinput:`linux/uinput.h`
- HID-BPF(kernel 6.3+),若日後需在 kernel 層修正 report descriptor
