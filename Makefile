CC = gcc
CFLAGS = -Wall -W -Werror -g -O2
LDFLAGS = -libverbs
TARGETS = main
OBJECTS = main.o get_clock.o

all: $(TARGETS)

main: $(OBJECTS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)	

%.o: %.c
	$(CC) -c $(CFLAGS) -o $@ $<

>>>>>>> 3aa0d42179e1fee462112f8ec8b0ee56c2d813de
clean:
	\rm -f *.o $(TARGETS)
