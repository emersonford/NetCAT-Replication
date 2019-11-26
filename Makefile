CC = gcc
CFLAGS = -Wall -W -Werror -g -O2
LDFLAGS = -libverbs
TARGETS = main
OBJECTS = RDMA_RC_Example.o get_clock.o

all: $(TARGETS)

main: $(OBJECTS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)	


%.o: %.c
	$(CC) -c $(CFLAGS) -o $@ $<

clean:
	\rm -f *.o $(TARGETS)
