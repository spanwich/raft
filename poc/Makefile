CC = gcc
CFLAGS = -Wall -Wextra -O2 -g -I../include -I.
LDFLAGS =

RAFT_SRC = ../src/raft_server.c ../src/raft_log.c ../src/raft_node.c \
           ../src/raft_server_properties.c
POC_SRC  = main.c entry_types.c app_state.c network.c

all: raft_poc

raft_poc: $(POC_SRC) $(RAFT_SRC)
	$(CC) $(CFLAGS) -o $@ $(POC_SRC) $(RAFT_SRC) $(LDFLAGS)

clean:
	rm -f raft_poc

.PHONY: all clean
