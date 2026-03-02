# lib/Makefile

CC      ?= gcc
CFLAGS  ?= -O2 -Wall -Wextra -Wpedantic -std=c11
CFLAGS  += -I../include -fPIC

SRCS    := libcxlmm.c pagemap_scan.c
OBJS    := $(SRCS:.c=.o)

LIB_SHARED := libcxlmm.so
LIB_STATIC := libcxlmm.a

.PHONY: all clean

all: $(LIB_SHARED) $(LIB_STATIC)

$(LIB_SHARED): $(OBJS)
	$(CC) -shared -o $@ $^ -lpthread

$(LIB_STATIC): $(OBJS)
	ar rcs $@ $^

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(LIB_SHARED) $(LIB_STATIC)
