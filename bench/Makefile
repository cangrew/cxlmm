CC      := gcc
CFLAGS  := -Wall -O2 -I../cxlmm/lib -I../cxlmm/include
LDFLAGS := -L../cxlmm/lib -lcxlmm -lnuma -lpthread

all: cxlmm_bench

cxlmm_bench: cxlmm_bench.c ../cxlmm/lib/libcxlmm.so
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

clean:
	rm -f cxlmm_bench

.PHONY: all clean
