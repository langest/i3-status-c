LIBS = -lX11 -lasound
FLAGS = -g -std=c99 -pedantic -Wall -O2

i3_status: i3_status.c
	gcc -o i3_status i3_status.c $(FLAGS) $(LIBS)

debug:
	gcc -o i3_status i3_status.c $(FLAGS) $(LIBS) -DDEBUG

clean:
	rm i3_status
