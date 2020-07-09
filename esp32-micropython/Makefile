SERIAL_PORT=/dev/ttyS4
BAUD=115200

.PHONY: rshell upload upload_client upload_server rshell_repl repl static pull_logs backup_data upload_keep

rshell:
	rshell -p $(SERIAL_PORT)

upload_server:
	rshell -p $(SERIAL_PORT) rsync server/ /pyboard/

upload_client:
	rshell -p $(SERIAL_PORT) rsync --mirror dist/ /pyboard/static/

backup_data:
	rshell -p $(SERIAL_PORT) rsync --mirror /pyboard/static/data/ dist/data/

pull_logs:
	rshell -p $(SERIAL_PORT) rsync --mirror /pyboard/static/logs/ dist/logs/



upload_keep: static pull_logs backup_data upload_client upload_server

upload: static upload_client upload_server

rshell_repl:
	rshell -p $(SERIAL_PORT) repl

repl:
	picocom $(SERIAL_PORT) -b$(BAUD)

static:
	 npm run build

