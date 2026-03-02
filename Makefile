# NEX Compress — Build System
CC       = gcc
CFLAGS   = -O3 -march=native -Wall -Wextra -std=c11 -Iinclude -D_GNU_SOURCE \
           -fstack-protector-strong -D_FORTIFY_SOURCE=3 -fPIE -fPIC \
           -Wformat -Wformat-security -Werror=format-security
LDFLAGS  = -lpthread -lm -pie -Wl,-z,relro,-z,now
SRCDIR   = src
OBJDIR   = build
SOURCES  = $(wildcard $(SRCDIR)/*.c)
OBJECTS  = $(patsubst $(SRCDIR)/%.c,$(OBJDIR)/%.o,$(SOURCES))
TARGET   = nexc

# Debug build: make DEBUG=1
ifdef DEBUG
	CFLAGS += -g -fsanitize=address,undefined -DNEX_DEBUG
	LDFLAGS += -fsanitize=address,undefined
else
	CFLAGS += -DNDEBUG
endif

.PHONY: all clean test bench

all: $(OBJDIR) $(TARGET)

$(OBJDIR):
	mkdir -p $(OBJDIR)

$(TARGET): $(OBJECTS)
	$(CC) $(OBJECTS) -o $@ $(LDFLAGS)

$(OBJDIR)/%.o: $(SRCDIR)/%.c | $(OBJDIR)
	$(CC) $(CFLAGS) -c $< -o $@

test: $(TARGET)
	@echo "=== Running unit tests ==="
	$(CC) $(CFLAGS) tests/test_units.c $(filter-out $(OBJDIR)/main.o,$(OBJECTS)) -o tests/test_units $(LDFLAGS)
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

clean:
	rm -rf $(OBJDIR) $(TARGET) tests/test_units *.nex *.out
