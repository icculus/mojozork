all: mojozork

mojozork: mojozork.c
	$(CC) -Wall -Os -s -o $@ $<

clean:
	rm -f mojozork


