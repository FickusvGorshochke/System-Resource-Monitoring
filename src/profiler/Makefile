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
CFLAGS = -Wall -std=c99 -D_QNX_SOURCE -O2

QNX_HOST_ALIAS ?= qnx

.PHONY: all clean deploy

all: kev_profile pin0

kev_profile: kev_profile.c
	$(CC) $(CFLAGS) -o $@ $< -ltraceparser

pin0: pin0.c
	$(CC) $(CFLAGS) -o $@ $<

deploy: all
	scp kev_profile pin0 $(QNX_HOST_ALIAS):/tmp/

clean:
	rm -f kev_profile pin0
