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

CASES = case_cpu_burner            case_cpu_burner_instr       \
        case_periodic_rt           case_periodic_rt_instr      \
        case_ipc_pingpong          case_ipc_pingpong_instr     \
        case_mutex_contention      case_mutex_contention_instr \
        case_prio_inversion        case_prio_inversion_instr   \
        case_prod_cons             case_prod_cons_instr

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
