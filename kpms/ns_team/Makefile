ifndef TARGET_COMPILE
    $(error TARGET_COMPILE not set. Set TARGET_COMPILE=aarch64-linux-gnu- or aarch64-none-elf-)
endif

ifndef KP_DIR
    KP_DIR = ../..
endif

CC = $(TARGET_COMPILE)gcc
LD = $(TARGET_COMPILE)ld

# Must match kernel/Makefile: no system includes, no builtins, aarch64-bare-metal
KPM_CFLAGS := -Wall -fno-builtin -std=gnu11 -nostdinc -mgeneral-regs-only -mno-outline-atomics -O2

INCLUDE_DIRS := . include patch/include linux/include linux/arch/arm64/include linux/tools/arch/arm64/include

INCLUDE_FLAGS := $(foreach dir,$(INCLUDE_DIRS),-I$(KP_DIR)/kernel/$(dir))

objs := driver.o

all: ns_team.kpm

ns_team.kpm: $(objs)
	$(CC) -r -o $@ $^

%.o: %.c
	$(CC) $(KPM_CFLAGS) $(INCLUDE_FLAGS) -Tdriver.lds -c -o $@ $<

.PHONY: clean
clean:
	rm -rf *.kpm
	find . -name "*.o" | xargs rm -f
