CC = gcc
CFLAGS = -Wall -W -Werror -g -O2
LDFLAGS = -libverbs
TARGETS = RDMA_RC_Example
OBJECTS = RDMA_RC_Example.o get_clock.o

all: $(TARGETS)

RDMA_RC_Example: $(OBJECTS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)	


%.o: %.c
	$(CC) -c $(CFLAGS) -o $@ $<

clean:
	\rm -f *.o $(TARGETS)
