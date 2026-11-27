# Usage:
#   make EX=ex01_preemption          build one exercise
#   make run EX=ex01_preemption      build and run it in QEMU
#   make run EX=ex01_preemption PREEMPT=0             cooperative scheduler
#   make run EX=ex02_isr_queue VARIANT=BAD_ISR_PRIO   build with -DBAD_ISR_PRIO
#   make results                     run every exercise, save logs to results/

EX      ?= ex01_preemption
PREEMPT ?= 1
VARIANT ?=

KERNEL  := third_party/FreeRTOS-Kernel
PORT    := $(KERNEL)/portable/GCC/ARM_CM3
BUILD   := build/$(EX)-p$(PREEMPT)$(if $(VARIANT),-$(VARIANT))
ELF     := $(BUILD)/$(EX).elf

# Optional per-exercise settings: EX_SRCS, EX_CFLAGS
-include $(EX)/build.mk

CC      := arm-none-eabi-gcc
SIZE    := arm-none-eabi-size

SRCS := $(KERNEL)/tasks.c $(KERNEL)/list.c $(KERNEL)/queue.c \
        $(KERNEL)/timers.c $(KERNEL)/event_groups.c \
        $(KERNEL)/portable/MemMang/heap_4.c $(PORT)/port.c \
        $(wildcard bsp/*.c) $(wildcard $(EX)/*.c) $(EX_SRCS)

CFLAGS := -mcpu=cortex-m3 -mthumb -std=gnu11 -O2 -g3 \
          -ffunction-sections -fdata-sections -ffreestanding \
          -Wall -Wextra -Wno-unused-parameter \
          -DconfigUSE_PREEMPTION=$(PREEMPT) $(if $(VARIANT),-D$(VARIANT)=1) \
          -Ibsp -Ibsp/CMSIS -I$(KERNEL)/include -I$(PORT) -I$(EX) $(EX_CFLAGS)
LDFLAGS := -T bsp/mps2_m3.ld -nostartfiles -specs=nano.specs -specs=nosys.specs \
           -Wl,--gc-sections -Wl,-Map=$(BUILD)/$(EX).map

OBJS := $(patsubst %.c,$(BUILD)/obj/%.o,$(SRCS))

# -icount makes QEMU's virtual time depend only on executed instructions, so
# runs are repeatable and not affected by how busy the host PC is.
QEMU := qemu-system-arm -machine mps2-an385 -cpu cortex-m3 \
        -monitor none -nographic -serial stdio \
        -semihosting-config enable=on,target=native \
        -icount shift=5,align=off,sleep=off

all: $(ELF)

$(BUILD)/obj/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

$(ELF): $(OBJS) bsp/mps2_m3.ld
	$(CC) $(CFLAGS) $(OBJS) $(LDFLAGS) -o $@
	$(SIZE) $@

run: $(ELF)
	@timeout 120 $(QEMU) -kernel $(ELF)

# Build quietly first, then capture only the program's output.
define save_run
	@$(MAKE) -s --no-print-directory all $(1) > /dev/null
	$(MAKE) -s --no-print-directory run $(1) > results/$(2).txt
endef

results:
	@mkdir -p results
	$(call save_run,EX=ex01_preemption PREEMPT=1,ex01_preemptive)
	$(call save_run,EX=ex01_preemption PREEMPT=0,ex01_cooperative)
	$(call save_run,EX=ex02_isr_queue,ex02_isr_queue)
	$(call save_run,EX=ex02_isr_queue VARIANT=BAD_ISR_PRIO,ex02_bad_isr_prio)
	$(call save_run,EX=ex03_priority_inversion,ex03_priority_inversion)
	python3 tools/timeline.py results/ex03_priority_inversion.txt > results/ex03_timeline.txt
	$(call save_run,EX=ex04_mini_ssd,ex04_mini_ssd)

# The FTL core on Linux with the firmware's geometry: sequential writes must
# give WAF 1, random writes must read back correctly.
ftl-test:
	@mkdir -p build/host
	gcc -std=c11 -O2 -Wall -Wextra -Iftl -DFTL_BLOCKS=128 -DFTL_PPB=64 \
	    ftl/ftl_core.c ftl/nand_sim.c ftl/test/host_test.c -o build/host/host_test_128x64
	./build/host/host_test_128x64 --workload=seq --op=0.25
	./build/host/host_test_128x64 --workload=uniform --op=0.25

clean:
	rm -rf build

-include $(OBJS:.o=.d)

.PHONY: all run results ftl-test clean
