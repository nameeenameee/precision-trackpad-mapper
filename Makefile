CC ?= x86_64-w64-mingw32-gcc
CFLAGS ?= -O2
CPPFLAGS ?=
LDFLAGS ?= -mconsole
LDLIBS ?= -lhid -lsetupapi

TARGET := finger-draw.exe

.PHONY: all clean

all: $(TARGET)

$(TARGET): finger-draw.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LDLIBS)

clean:
	rm -f $(TARGET) *.obj
