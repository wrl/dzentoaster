all: dzentoaster

dzentoaster: dzentoaster.c config.h
	gcc -ggdb -Werror -Wall -pedantic -lpthread -lrt $< -o $@