pi: pi.o
	gcc pi.o -o pi -lpthread

.c.o:
	gcc -O3 -c $< -Wall

clean:
	rm -f pi.o pi
