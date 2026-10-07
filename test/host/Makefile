CC ?= gcc
CFLAGS ?= -std=c11 -Wall -Wextra -Werror -O1 -g
INC = -I../../components/ble_midi_parser/include -I../../components/midi_uart/include

.PHONY: test clean
test: test_parser test_tx_queue
	./test_parser
	./test_tx_queue

test_parser: test_parser.c ../../components/ble_midi_parser/ble_midi_parser.c
	$(CC) $(CFLAGS) $(INC) -o $@ $^

test_tx_queue: test_tx_queue.c ../../components/midi_uart/include/midi_tx_queue.h ../../components/midi_uart/include/midi_ring.h
	$(CC) $(CFLAGS) $(INC) -o $@ test_tx_queue.c

clean:
	rm -f test_parser test_parser.exe test_tx_queue test_tx_queue.exe
