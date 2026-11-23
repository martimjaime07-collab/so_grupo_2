CC = gcc

CFLAGS = -ggdb -std=c17 -D_POSIX_C_SOURCE=200809L -pthread \
		 -Wall -Werror -Wextra \
		 -Wcast-align -Wconversion -Wfloat-equal -Wformat=2 -Wnull-dereference -Wshadow -Wsign-conversion -Wundef -Wunreachable-code -Wunused \
 		 -fsanitize=undefined

ifneq ($(shell uname -s),Darwin) # if not MacOS
	CFLAGS += -fmax-errors=5
endif

all: cloudIST

cloudIST: main.c ../common/constants.h  ../common/common.h parser.o datacenter.o datacenter_utils.o resources.o ../common/filesystem.o
	$(CC) $(CFLAGS) $(SLEEP) -o cloudIST main.c parser.o datacenter.o datacenter_utils.o resources.o filesystem.o -lm

%.o: %.c %.h
	$(CC) $(CFLAGS) -c ${@:.o=.c}

clean:
	rm -f *.o cloudIST

format:
	@which clang-format >/dev/null 2>&1 || echo "Please install clang-format to run this command"
	clang-format -i *.c *.h