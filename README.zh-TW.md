# rtos-ssd-lab

[English](README.md) | **繁體中文**

這個 repo 是我練 RTOS 的地方。RTOS（即時作業系統）是很多韌體在用的小型作業系統，負責決定「現在輪到哪個工作跑」。SSD 控制器裡的韌體，也是一堆工作在搶 CPU：接主機指令、查對應表、搬資料、做垃圾回收。

我用 FreeRTOS，跑在 QEMU 模擬的 Arm Cortex-M3 開發板（mps2-an385）上，所以不需要買硬體。目前完成了三個基本練習，每個練習都有實際量到的數字。之後的目標是把我另一個專案 [ftl-sim](https://github.com/KJLavender/ftl-sim) 的 FTL 核心改寫成 C，拆成幾個 task，做成一個迷你 SSD 韌體（還沒做）。

## 環境

在 Linux 或 WSL2 的 Ubuntu 上：

```bash
sudo apt install gcc-arm-none-eabi libnewlib-arm-none-eabi qemu-system-arm make python3
git clone --recurse-submodules <這個 repo 的網址>
cd rtos-ssd-lab
```

我用的版本：arm-none-eabi-gcc 14.2、QEMU 10.2、FreeRTOS-Kernel V11.3.3（放在 `third_party/`，是 git submodule）。

## 怎麼跑

```bash
make run EX=ex01_preemption              # 練習 1，搶占式排程
make run EX=ex01_preemption PREEMPT=0    # 練習 1，改成合作式排程
make run EX=ex02_isr_queue               # 練習 2
make run EX=ex02_isr_queue VARIANT=BAD_ISR_PRIO   # 練習 2，故意把中斷優先權設錯
make run EX=ex03_priority_inversion      # 練習 3
make results                             # 全部跑一次，輸出存到 results/
```

每個練習跑完會自己結束 QEMU。如果卡住想手動離開，按 `Ctrl+A` 再按 `X`。

## 練習 1：搶占（Preemption）

兩個 task。L 優先權低，一直在做事，每做 1 ms 進度加一。H 優先權高，每 500 ms 醒來一次，做 2 ms 的事。H 每次醒來會記兩件事：從該醒的那個 tick 到真正拿到 CPU 隔了多久，還有它自己在跑的時候，L 的進度有沒有動。

| 排程方式 | H 喚醒延遲（平均） | H 喚醒延遲（最大） |
|---|---|---|
| 搶占式 | 7 µs | 7 µs |
| 合作式（L 每 50 ms 主動讓出一次） | 19.7 ms | 37.5 ms |

搶占式時，H 一醒來 L 就被暫停，H 跑的那 2 ms 裡 L 的進度完全沒動（例如 `L progress 497 -> 497`）。合作式時，H 只能等 L 自己讓出 CPU，延遲最多可以到 L 讓出的間隔 50 ms。表裡的平均值會隨跑的輪數變，重點是上限由 L 決定，H 管不到。

## 練習 2：中斷 + Queue（ISR → Task）

TIMER0 每 700 µs 發一次中斷，假裝主機送來一筆指令。ISR（中斷處理函式）只做兩件事：在指令上蓋時間戳，然後用 `xQueueSendFromISR` 丟進 queue。真正的處理（每筆 200 µs）交給處理指令的 task，它用 `xQueueReceive` 等，queue 是空的就睡覺。

同樣 1000 筆指令跑兩次，差別在 ISR 結尾有沒有呼叫 `portYIELD_FROM_ISR`：

| ISR 結尾 | 收到 / 送出 | 漏掉 | 順序錯 | 中斷到 task 的延遲（平均 / 最大） | 處理 task 的 CPU | idle 的 CPU |
|---|---|---|---|---|---|---|
| 有 `portYIELD_FROM_ISR` | 1000 / 1000 | 0 | 0 | 11 µs / 11 µs | 31% | 68% |
| 沒有 | 1000 / 1000 | 0 | 0 | 304 µs / 789 µs | 31% | 68% |

- 處理 task 只用了 31% 的 CPU，差不多就是 200 / 700。剩下的時間都在 idle，代表 queue 空的時候 task 真的在睡，沒有空轉。
- 沒有 yield 的話，task 雖然已經可以跑了，還是要等到下一次 tick（1 ms 一次）才換它上場，所以延遲變成幾百 µs。
- `VARIANT=BAD_ISR_PRIO` 把 TIMER0 的優先權設得比 `configMAX_SYSCALL_INTERRUPT_PRIORITY` 還緊急。這種中斷不能呼叫 FreeRTOS 的 API，FreeRTOS 一抓到就停下來：`ASSERT failed: .../ARM_CM3/port.c:803`。

## 練習 3：優先權反轉（Priority Inversion）

三個 task 共用一把鎖：

- L（低）：拿鎖，在鎖裡做 10 ms 的事
- H（高）：2 ms 後也要這把鎖
- M（中）：不需要鎖，3 ms 時醒來，做 50 ms 的事

另外有一個最高優先權的 CTRL task，照固定時間點叫醒它們，所以每一輪的劇本都一樣。同一個劇本，鎖分別用 binary semaphore 和 mutex 各跑 5 輪：

| 鎖的種類 | H 等鎖的時間（5 輪平均） |
|---|---|
| binary semaphore | 58.2 ms |
| mutex | 8.1 ms |

把 context switch 錄下來畫成時間軸（`#` 在跑，`^` 用借來的優先權在跑，`.` 在等鎖）：

```
[binary_semaphore]  1 char = 1 ms
     0         10        20        30        40        50        60
     |---------|---------|---------|---------|---------|---------|-
  H    ..........................................................#
  M     ##################################################
  L  ###                                                  #######

[mutex]  1 char = 1 ms
     0         10        20        30        40        50        60
     |---------|---------|---------|---------|---------|---------|-
  H    ........#
  M             ##################################################
  L  ##^^^^^^^^
```

用 binary semaphore 時，H 在等 L，L 卻被 M 搶走 CPU，所以 H 等了 L 剩下的 8 ms 再加上 M 的 50 ms。換成 mutex，H 一開始等，RTOS 就把 L 暫時升到 H 的優先權（`^`）。M 插不進來，H 只要等 L 把鎖裡的事做完。差別在 mutex 有「擁有者」，RTOS 知道該幫誰升級；semaphore 只是一個計數，沒有擁有者。

## 數字怎麼量的

- QEMU 不是逐週期精準的模擬器，也沒有實作 Cortex-M 的 DWT cycle counter。我用板子上的 TIMER1 當 25 MHz 的自由計數器來量時間。
- QEMU 加了 `-icount shift=5,align=off,sleep=off`，讓模擬時間只跟執行了幾條指令有關，跟主機忙不忙無關。同一個練習跑兩次，輸出一模一樣。
- 所以這些數字是 QEMU 的虛擬時間，適合拿來比較兩種做法，不代表真實晶片上的絕對值。
- 「做 X ms 的事」是用開機時校正過的空迴圈來燒 CPU。被搶占的話，實際花的時間會變長，這樣才能模擬「固定的工作量」。

## 目錄

```
bsp/          開發板相關：啟動碼、連結腳本、UART 輸出、計時、FreeRTOSConfig.h、context switch 紀錄
ex01_preemption/          練習 1
ex02_isr_queue/           練習 2
ex03_priority_inversion/  練習 3
tools/timeline.py         把 context switch 紀錄畫成文字時間軸
results/                  make results 的輸出
third_party/FreeRTOS-Kernel   FreeRTOS 核心（submodule）
```

## 授權

- FreeRTOS-Kernel：MIT
- `bsp/CMSIS/` 裡的標頭檔從 FreeRTOS 的 MPS2 QEMU demo 複製過來，原始授權（Arm 的 Apache-2.0 和 BSD-3-Clause）寫在各檔案開頭
