# AudioBus FD16 host unit tests
#
# Build:  make
# Run:    make run
# Clean:  make clean

CC      ?= gcc
CFLAGS  ?= -std=c11 -Wall -Wextra -Werror
INCLUDES = -I../../components/audiobus/include -Istubs -Istubs/driver

TARGETS = test_fd16 test_si5351a

.PHONY: all run clean

all: $(TARGETS)

test_fd16: test_fd16_frame.c ../../components/audiobus/src/fd16_frame.c
	$(CC) $(CFLAGS) -I../../components/audiobus/include \
	    test_fd16_frame.c ../../components/audiobus/src/fd16_frame.c -o $@

test_si5351a: test_si5351a.c ../../components/audiobus/src/si5351a.c
	$(CC) $(CFLAGS) $(INCLUDES) \
	    test_si5351a.c ../../components/audiobus/src/si5351a.c -o $@

run: all
	./test_fd16
	./test_si5351a

clean:
	rm -f $(TARGETS)