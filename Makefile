default: main.o my_print.o
	@g++ main.o my_print.o -o main
	@./main

my_print.o: my_print.asm
	@nasm my_print.asm -f macho64 -o my_print.o

main.o: main.cpp
	@g++ -c main.cpp -o main.o -std=c++2a

clean:
	@rm main.o my_print.o main