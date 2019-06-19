SERIAL_PORT=/dev/ttyS4
BAUD=115200

rshell:
	rshell -p $(SERIAL_PORT)

upload: static
	rshell -p $(SERIAL_PORT) rsync server/ /pyboard/
	rshell -p $(SERIAL_PORT) rsync --mirror dist/ /pyboard/static/

rshell_repl:
	rshell -p $(SERIAL_PORT) repl

repl:
	picocom $(SERIAL_PORT) -b$(BAUD)

static:
	 npm run build

.PHONY: rshell upload rshell_repl repl static