TARGET=c8

all:
	gcc -o $(TARGET) chip8.c -lSDL2 -lm

clean:
	rm $(TARGET)
