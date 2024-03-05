all:
	gcc lkfw.c -o lkfw -lmosquitto -lconfuse

debug:
	gcc -g lkfw.c -o lkfw -lmosquitto -lconfuse

clean:
	rm lkfw

install:
	mkdir -p /opt/lkfw
	cp lkfw* /opt/lkfw
	rm /opt/lkfw/lkfw.c
	cp lkrules.conf /etc/monit/conf.d
	rm -f /usr/lib/systemd/system/lkfw.service && cp lkfw.service /usr/lib/systemd/system/lkfw.service
	systemctl daemon-reload
	systemctl restart monit

prepare:
	./install_required.sh
