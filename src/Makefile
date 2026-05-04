# Makefile для сервиса системного профилирования ЗОСРВ "Нейтрино" 2021.
#
# Использует qcc — рекомендованный КПДА драйвер компиляции.
#
# Использование:
#   make                       — собрать всё (sysmond + sysmon_cli) для x86
#   make ARCH=armv7            — для ARMv7
#   make ARCH=aarch64          — для ARMv8
#   make DEBUG=1               — отладочная сборка с символами
#   make deploy                — собрать и развернуть на Нейтрино
#   make clean                 — удалить артефакты
#
# Перед сборкой должны быть выставлены QNX_HOST и QNX_TARGET, например:
#   export QNX_HOST=/opt/kpda2021a/host/linux64/x86_64
#   export QNX_TARGET=/opt/kpda2021a/target/neutrino
#   export PATH=$QNX_HOST/usr/bin:$PATH

ARCH ?= x86

ifeq ($(ARCH), x86)
    QCC_VARIANT = gcc_ntox86
else ifeq ($(ARCH), armv7)
    QCC_VARIANT = gcc_ntoarmv7le
else ifeq ($(ARCH), aarch64)
    QCC_VARIANT = gcc_ntoaarch64le
else
    $(error Unknown ARCH '$(ARCH)'. Use: x86, armv7, aarch64)
endif

CC = qcc -V$(QCC_VARIANT)

CFLAGS = -Wall -Wextra -std=c99 \
         -D_QNX_SOURCE          \
         -I./common             \
         -fstack-protector-all

ifdef DEBUG
    CFLAGS += -g -O0 -DDEBUG
else
    CFLAGS += -O2
endif


LIBS_DAEMON =
LIBS_CLI    =

DAEMON_SRCS = daemon/main.c            \
              daemon/collector.c       \
              daemon/ringbuf.c         \
              daemon/resmgr_handler.c

CLI_SRCS    = cli/sysmon_cli.c

DAEMON_OBJS = $(DAEMON_SRCS:.c=.o)
CLI_OBJS    = $(CLI_SRCS:.c=.o)

QNX_HOST_ALIAS ?= qnx
QNX_BIN_DIR    ?= /usr/bin

.PHONY: all clean check-env deploy

all: check-env sysmond sysmon_cli

check-env:
	@if [ -z "$$QNX_HOST" ] || [ -z "$$QNX_TARGET" ]; then \
	    echo "ОШИБКА: QNX_HOST/QNX_TARGET не установлены."; \
	    echo "Выставьте окружение КПДА перед сборкой:"; \
	    echo "  export QNX_HOST=/opt/kpda2021a/host/linux64/x86_64"; \
	    echo "  export QNX_TARGET=/opt/kpda2021a/target/neutrino"; \
	    echo "  export PATH=\$$QNX_HOST/usr/bin:\$$PATH"; \
	    exit 1; \
	fi
	@which qcc >/dev/null 2>&1 || { \
	    echo "ОШИБКА: qcc не найден в PATH."; \
	    exit 1; \
	}

sysmond: $(DAEMON_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LIBS_DAEMON)
	@echo "Собран: $@  (ARCH=$(ARCH))"

sysmon_cli: $(CLI_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LIBS_CLI)
	@echo "Собран: $@  (ARCH=$(ARCH))"

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

deploy: all
	scp sysmond sysmon_cli $(QNX_HOST_ALIAS):$(QNX_BIN_DIR)/
	ssh $(QNX_HOST_ALIAS) 'chmod +x $(QNX_BIN_DIR)/sysmond $(QNX_BIN_DIR)/sysmon_cli'
	@echo "Развёрнуто на $(QNX_HOST_ALIAS):$(QNX_BIN_DIR)/"

clean:
	rm -f $(DAEMON_OBJS) $(CLI_OBJS) sysmond sysmon_cli
