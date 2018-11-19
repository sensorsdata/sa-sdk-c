CC=gcc
CFLAGS=-Wall -W -I. -DUSE_POSIX

all: demo
	ar rcs libsensorsanalytics.a sensors_analytics.o
	mkdir -p ./output/include ./output/lib
	cp *.h ./output/include/.
	cp *.a ./output/lib/.

demo: sensors_analytics.o
	$(CC) -o $@ demo.c $^ $(CFLAGS)

sensors_analytics.o: sensors_analytics.c sensors_analytics.h
	$(CC) -c sensors_analytics.c $(CFLAGS)

.PHONY: clean

clean:
	rm -rf *.o *.a
	rm -rf output
	rm -rf demo
	rm -rf demo.out.log.*
