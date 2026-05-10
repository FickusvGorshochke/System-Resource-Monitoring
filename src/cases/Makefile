# Сборка case-программ для проверки sysmond.
#
# Использование:
#   make           — собрать все 4 case-программы под x86
#   make ARCH=...  — другая архитектура (см. ../Makefile)
#   make deploy    — развернуть на цель через scp

ARCH ?= x86

ifeq ($(ARCH), x86)
    QCC_VARIANT = gcc_ntox86
else ifeq ($(ARCH), armv7)
    QCC_VARIANT = gcc_ntoarmv7le
else ifeq ($(ARCH), aarch64)
    QCC_VARIANT = gcc_ntoaarch64le
else
    $(error Unknown ARCH '$(ARCH)')
endif

CC     = qcc -V$(QCC_VARIANT)
CFLAGS = -Wall -Wextra -std=c99 -D_QNX_SOURCE -O2

CASES = case_cpu_burner       \
        case_periodic_rt      \
        case_ipc_pingpong     \
        case_mutex_contention

QNX_HOST_ALIAS ?= qnx

.PHONY: all clean deploy

all: $(CASES)

%: %.c
	$(CC) $(CFLAGS) -o $@ $<
	@echo "Собран: $@"

deploy: all
	scp $(CASES) $(QNX_HOST_ALIAS):/usr/bin/
	ssh $(QNX_HOST_ALIAS) 'chmod +x $(addprefix /usr/bin/,$(CASES))'
	@echo "case-программы развёрнуты на $(QNX_HOST_ALIAS):/usr/bin/"

clean:
	rm -f $(CASES)
