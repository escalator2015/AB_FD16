# AudioBus FD16 frame module host unit tests
#
# Build:  make
# Run:    make run
# Clean:  make clean

CC      ?= gcc
CFLAGS  ?= -std=c11 -Wall -Wextra -Werror
INCLUDES = -I../../components/audiobus/include
SRCS     = test_fd16_frame.c ../../components/audiobus/src/fd16_frame.c
TARGET   = test_fd16

.PHONY: all run clean

all: $(TARGET)

$(TARGET): $(SRCS)
	$(CC) $(CFLAGS) $(INCLUDES) $(SRCS) -o $(TARGET)

run: $(TARGET)
	./$(TARGET)

clean:
	rm -f $(TARGET)