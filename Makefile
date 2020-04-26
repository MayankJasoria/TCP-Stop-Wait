# set the compiler
CC=gcc

# set compiler flags
CFLAGS=-o

#set dependencies for the program

program:
	$(CC) server.c $(CFLAGS) server
	$(CC) client.c $(CFLAGS) client

clean:
	rm -rf server client