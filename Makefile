PREFIX=/usr

all: dzentoaster

install: dzentoaster
	install dzentoaster $(PREFIX)/bin

clean:
	rm -f dzentoaster

dzentoaster: dzentoaster.c config.h
	gcc -ggdb -Werror -Wall -pedantic -lpthread -lrt $< -o $@