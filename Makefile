CC ?= gcc
CFLAGS ?= -O3 -s
CPPFLAGS ?=
LDFLAGS ?= -mconsole
LDLIBS ?= -luser32 -lgdi32 -lhid -lsetupapi

TARGET := precision-trackpad-mapper.exe
SRC := precision-trackpad-mapper.c

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LDLIBS)

clean:
	rm -f $(TARGET) *.o *.obj