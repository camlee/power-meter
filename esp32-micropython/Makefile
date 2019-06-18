SERIAL_PORT=/dev/ttyS4
BAUD=115200

rshell:
	rshell -p $(SERIAL_PORT)

upload:
	rshell -p $(SERIAL_PORT) rsync src/ /pyboard

rshell_repl:
	rshell -p $(SERIAL_PORT) repl

repl:
	picocom $(SERIAL_PORT) -b$(BAUD)