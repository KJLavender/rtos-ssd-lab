# rtos-ssd-lab

**English** | [繁體中文](README.zh-TW.md)

This repo is where I practice RTOS basics. An RTOS (real-time operating system) is the small OS many firmware projects use to decide which piece of work runs next. SSD controller firmware is a set of jobs competing for the CPU too: taking host commands, looking up the mapping table, moving data, and garbage collection.

I use FreeRTOS on the Arm Cortex-M3 board (mps2-an385) that QEMU emulates, so no hardware is needed. Three basic exercises are done so far, each with measured numbers. The next step is to rewrite the FTL core of my other project, [ftl-sim](https://github.com/KJLavender/ftl-sim), in C and split it into tasks as a small SSD firmware (not done yet).

## Setup

On Linux or Ubuntu under WSL2:

```bash
sudo apt install gcc-arm-none-eabi libnewlib-arm-none-eabi qemu-system-arm make python3
git clone --recurse-submodules <this repo's URL>
cd rtos-ssd-lab
```

Versions I used: arm-none-eabi-gcc 14.2, QEMU 10.2, FreeRTOS-Kernel V11.3.3 (a git submodule under `third_party/`).

## Running

```bash
make run EX=ex01_preemption              # exercise 1, preemptive scheduler
make run EX=ex01_preemption PREEMPT=0    # exercise 1, cooperative scheduler
make run EX=ex02_isr_queue               # exercise 2
make run EX=ex02_isr_queue VARIANT=BAD_ISR_PRIO   # exercise 2 with a wrong interrupt priority
make run EX=ex03_priority_inversion      # exercise 3
make results                             # run everything, save output to results/
```

Each exercise stops QEMU when it finishes. To quit by hand, press `Ctrl+A` then `X`.

## Exercise 1: preemption

Two tasks. L has low priority and works all the time, adding one to its progress counter per 1 ms of work. H has high priority, wakes every 500 ms and does 2 ms of work. Each time H wakes it records how long it took from its tick to actually getting the CPU, and whether L's counter moved while H was running.

| Scheduler | H wake latency (avg) | H wake latency (max) |
|---|---|---|
| Preemptive | 7 µs | 7 µs |
| Cooperative (L yields every 50 ms) | 19.7 ms | 37.5 ms |

With preemption, L stops as soon as H wakes, and L's counter does not move during H's 2 ms (e.g. `L progress 497 -> 497`). With the cooperative scheduler, H has to wait until L gives up the CPU, which can take up to L's 50 ms yield interval. The average in the table depends on how many rounds run; the point is that the upper bound is set by L, and H has no say in it.

## Exercise 2: interrupt + queue (ISR → task)

TIMER0 fires every 700 µs and pretends to be the host sending a command. The ISR (interrupt handler) only stamps the command with a time and pushes it into a queue with `xQueueSendFromISR`. A handler task does the real work (200 µs per command). It waits in `xQueueReceive` and sleeps while the queue is empty.

The same 1000 commands run twice. The only difference is whether the ISR ends with `portYIELD_FROM_ISR`:

| ISR ending | received / sent | lost | out of order | ISR → task latency (avg / max) | handler CPU | idle CPU |
|---|---|---|---|---|---|---|
| with `portYIELD_FROM_ISR` | 1000 / 1000 | 0 | 0 | 11 µs / 11 µs | 31% | 68% |
| without | 1000 / 1000 | 0 | 0 | 304 µs / 789 µs | 31% | 68% |

- The handler uses 31% of the CPU, close to 200 / 700. The rest is idle time, so the task really sleeps when there is nothing to do.
- Without the yield, the task is ready but only gets switched in at the next tick (every 1 ms), so latency grows to hundreds of µs.
- `VARIANT=BAD_ISR_PRIO` makes TIMER0 more urgent than `configMAX_SYSCALL_INTERRUPT_PRIORITY`. Interrupts that urgent must not call FreeRTOS APIs, and FreeRTOS stops with `ASSERT failed: .../ARM_CM3/port.c:803`.

## Exercise 3: priority inversion

Three tasks share one lock:

- L (low): takes the lock and does 10 ms of work while holding it
- H (high): wants the same lock 2 ms later
- M (medium): does not need the lock; wakes at 3 ms and does 50 ms of work

A highest-priority CTRL task wakes them at fixed times, so every round follows the same script. The script runs 5 rounds with a binary semaphore as the lock, then 5 rounds with a mutex:

| Lock | H's wait for the lock (avg of 5 rounds) |
|---|---|
| binary semaphore | 58.2 ms |
| mutex | 8.1 ms |

Context switches drawn as a timeline (`#` running, `^` running at a borrowed priority, `.` waiting for the lock):

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

With the binary semaphore, H waits for L, but M takes the CPU away from L, so H waits for the rest of L's 8 ms plus all 50 ms of M. With the mutex, as soon as H starts waiting the RTOS raises L to H's priority (`^`). M can't get in, and H only waits for L to finish its critical section. The difference is that a mutex has an owner, so the RTOS knows whose priority to raise; a semaphore is only a count with no owner.

## How the numbers are measured

- QEMU is not cycle accurate and does not implement the Cortex-M DWT cycle counter. Times come from the board's TIMER1 used as a free-running 25 MHz counter.
- QEMU runs with `-icount shift=5,align=off,sleep=off`, so emulated time depends only on the number of executed instructions, not on how busy the host is. Running an exercise twice gives identical output.
- So these are QEMU virtual times. They are good for comparing two approaches, not as absolute numbers for a real chip.
- "Do X ms of work" burns the CPU with a loop calibrated at boot. If the task gets preempted, the work takes longer in real time, which is how a fixed amount of work should behave.

## Layout

```
bsp/          board code: startup, linker script, UART output, timing, FreeRTOSConfig.h, context switch recorder
ex01_preemption/          exercise 1
ex02_isr_queue/           exercise 2
ex03_priority_inversion/  exercise 3
tools/timeline.py         draws the context switch record as a text timeline
results/                  output of make results
third_party/FreeRTOS-Kernel   FreeRTOS kernel (submodule)
```

## License

- FreeRTOS-Kernel: MIT
- The headers in `bsp/CMSIS/` come from FreeRTOS's MPS2 QEMU demo; their original licenses (Arm's Apache-2.0 and BSD-3-Clause) are in each file header
