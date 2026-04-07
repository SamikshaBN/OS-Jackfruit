obj-m += monitor.o

KDIR := /lib/modules/$(shell uname -r)/build
PWD  := $(shell pwd)

# Static linking lets workload binaries run inside the Alpine rootfs
# without needing its libc. Override with WORKLOAD_LDFLAGS= to build
# dynamically (e.g. for CI runners without musl/static support).
WORKLOAD_LDFLAGS ?= -static

USER_TARGETS := engine memory_hog cpu_hog io_pulse

all: $(USER_TARGETS) module

# CI target: build only user-space binaries, no kernel headers required
ci: WORKLOAD_LDFLAGS =
ci: $(USER_TARGETS)

module: monitor.ko

engine: engine.c monitor_ioctl.h
	gcc -O2 -Wall -Wextra -o engine engine.c -lpthread

memory_hog: memory_hog.c
	gcc -O2 -Wall $(WORKLOAD_LDFLAGS) -o memory_hog memory_hog.c

cpu_hog: cpu_hog.c
	gcc -O2 -Wall $(WORKLOAD_LDFLAGS) -o cpu_hog cpu_hog.c

io_pulse: io_pulse.c
	gcc -O2 -Wall $(WORKLOAD_LDFLAGS) -o io_pulse io_pulse.c

monitor.ko: monitor.c monitor_ioctl.h
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	if [ -d "$(KDIR)" ]; then $(MAKE) -C $(KDIR) M=$(PWD) clean; fi
	rm -f $(USER_TARGETS) *.o *.mod *.mod.c *.symvers *.order
	rm -f *.log
	rm -rf logs
	rm -f /tmp/mini_runtime.sock

.PHONY: all ci module clean
