# daemon/Makefile

CC      ?= gcc
CFLAGS  ?= -O2 -Wall -Wextra -std=c11
CFLAGS  += -I../include -I../lib
LDFLAGS += -L../lib -Wl,-rpath,'$$ORIGIN/../lib'
LIBS    := -lcxlmm -lnuma -lpthread

SRCS    := cxlmm_daemon.c classifier.c
OBJS    := $(SRCS:.c=.o)
TARGET  := cxlmm_daemon

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS) $(LIBS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(TARGET)
