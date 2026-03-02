# NEX Compress — Build System
CC       = gcc
CFLAGS   = -O3 -march=native -Wall -Wextra -std=c11 -Iinclude -D_GNU_SOURCE \
           -fstack-protector-strong -D_FORTIFY_SOURCE=3 -fPIE -fPIC \
           -Wformat -Wformat-security -Werror=format-security
LDFLAGS_BIN = -lpthread -lm -pie -Wl,-z,relro,-z,now
LDFLAGS_LIB = -lpthread -lm -shared -Wl,-soname,libnex.so -Wl,-z,relro,-z,now
SRCDIR   = src
OBJDIR   = build
SOURCES  = $(wildcard $(SRCDIR)/*.c)
OBJECTS  = $(patsubst $(SRCDIR)/%.c,$(OBJDIR)/%.o,$(SOURCES))
LIB_OBJS = $(filter-out $(OBJDIR)/main.o,$(OBJECTS))
TARGET   = nexc
LIB_TARGET = libnex.so

# Debug build: make DEBUG=1
ifdef DEBUG
	CFLAGS += -g -fsanitize=address,undefined -DNEX_DEBUG
	LDFLAGS_BIN += -fsanitize=address,undefined
	LDFLAGS_LIB += -fsanitize=address,undefined
else
	CFLAGS += -DNDEBUG
endif

.PHONY: all clean test bench

all: $(OBJDIR) $(TARGET) $(LIB_TARGET)

$(OBJDIR):
	mkdir -p $(OBJDIR)

$(TARGET): $(OBJECTS)
	$(CC) $(OBJECTS) -o $@ $(LDFLAGS_BIN)

$(LIB_TARGET): $(LIB_OBJS)
	$(CC) $(LIB_OBJS) -o $@ $(LDFLAGS_LIB)

$(OBJDIR)/%.o: $(SRCDIR)/%.c | $(OBJDIR)
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -rf $(OBJDIR) $(TARGET) $(LIB_TARGET) tests/test_units *.nex *.out

test: $(TARGET) $(LIB_TARGET)
	@echo "=== Running unit tests ==="
	$(CC) $(CFLAGS) tests/test_units.c $(LIB_OBJS) -o tests/test_units $(LDFLAGS_BIN)
	./tests/test_units
	@echo "=== Running round-trip tests ==="
	bash tests/test_roundtrip.sh

bench: $(TARGET)
	@echo "=== Benchmark Mode ==="
	@if [ -z "$(FILE)" ]; then \
		echo "Usage: make bench FILE=<testfile>"; \
	else \
		./$(TARGET) -b $(FILE); \
	fi

