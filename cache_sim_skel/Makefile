.PHONY: all

all: cache_sim 

util.o: util.h util.c
	gcc -c util.c

cache.o: cache.h cache.c
	gcc -c cache.c

main.o: main.c
	gcc -c main.c

cache_sim: cache.o util.o main.o
	gcc -lm -o cache_sim cache.o util.o main.o

clean:
	rm -rf ./cache_sim  ./util.o o ./cache.o ./main.o
