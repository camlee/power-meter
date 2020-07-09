import os

log_directory = "static/logs"

try:
    os.mkdir(log_directory)
except Exception:
    pass # Directory already exists

def log_exception(msg):
    with open("%s/%s" % (log_directory, "exception.txt"), "a") as f:
        f.write(msg)