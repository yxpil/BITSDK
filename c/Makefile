# BITSDK C Makefile
#
# Targets:
#   all      — build library objects into build/ (libbitsdk.a)
#   example  — build examples/example.c
#   smoke    — build the smoke test binary
#   test     — build + run the smoke test (needs the fake BIT server)
#   clean    — remove build/

CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra -Wno-unused-parameter
INCLUDES = -Iinclude -Isrc -Isrc/cjson
LIBS     = -lcurl

BUILD := build

all: $(BUILD)/libbitsdk.a

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/bitsdk.o: src/bitsdk.c include/bitsdk.h src/cjson/cJSON.h | $(BUILD)
	$(CC) $(CFLAGS) $(INCLUDES) -c src/bitsdk.c -o $@

$(BUILD)/cJSON.o: src/cjson/cJSON.c src/cjson/cJSON.h | $(BUILD)
	$(CC) $(CFLAGS) $(INCLUDES) -c src/cjson/cJSON.c -o $@

$(BUILD)/libbitsdk.a: $(BUILD)/bitsdk.o $(BUILD)/cJSON.o
	ar rcs $@ $^

example: $(BUILD)/example
$(BUILD)/example: examples/example.c $(BUILD)/libbitsdk.a
	$(CC) $(CFLAGS) $(INCLUDES) examples/example.c $(BUILD)/libbitsdk.a $(LIBS) -o $@

smoke: $(BUILD)/smoke
$(BUILD)/smoke: test/test_smoke.c src/bitsdk.c src/cjson/cJSON.c include/bitsdk.h | $(BUILD)
	$(CC) $(CFLAGS) -Iinclude -Isrc -Isrc/cjson test/test_smoke.c src/bitsdk.c src/cjson/cJSON.c -lcurl -o $(BUILD)/smoke

test: smoke
	./$(BUILD)/smoke

clean:
	rm -rf $(BUILD)

.PHONY: all example smoke test clean
