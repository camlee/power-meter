SERIAL_PORT=/dev/ttyS4
BAUD=115200

.PHONY: rshell upload upload_client upload_server rshell_repl repl static

rshell:
	rshell -p $(SERIAL_PORT)

upload_server:
	rshell -p $(SERIAL_PORT) rsync server/ /pyboard/

upload_client:
	rshell -p $(SERIAL_PORT) rsync --mirror dist/ /pyboard/static/


upload: static upload_client upload_server

rshell_repl:
	rshell -p $(SERIAL_PORT) repl

repl:
	picocom $(SERIAL_PORT) -b$(BAUD)

static:
	 npm run build