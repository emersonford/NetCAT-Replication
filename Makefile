CC = gcc
CFLAGS = -Wall -W -Werror -g -O2
LDFLAGS = -libverbs
TARGETS = main

all: $(TARGETS)

main: main.o
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)	

main.o: main.c
	$(CC) -c $(CFLAGS) $<
clean:
	\rm -f *.o $(TARGETS)
