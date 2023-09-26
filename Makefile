lk: lkfw.c
	gcc lkfw.c -o lkfw -lmosquitto -lconfuse

debug:
	gcc -g lkfw.c -o lkfw -lmosquitto -lconfuse

clean:
	rm lkfw 
