import os
import time
import machine

EPOCH_DELTA = 946684800 # (date(2000, 1, 1) - date(1970, 1, 1)).days * 24*60*60

def set_time_from_epoch(epoch):
    """
    Sets the system time from the provided unix epoch time
    """
    # converting from unix epoch to ours (2000-01-01 00:00:00 UTC):
    our_epoch_time = int(epoch) - EPOCH_DELTA
    tm = time.localtime(our_epoch_time)
    tm = tm[0:3] + (0,) + tm[3:6] + (0,)
    machine.RTC().datetime(tm)

def epoch_time():
    """
    Returns the current system time as the unix epoch.
    """
    return time.time() + EPOCH_DELTA

def file_size(path, exclude=[]):
    """
    Returns the number of bytes in the file or directory specified by path.
    Recursively checks all subdirectories. Optionally, ommits the directories
    specified in exclude.
    """
    path = path.rstrip("/")
    try:
        stats = os.stat(path)
    except OSError:
        return 0 # Files that don't exist don't take up any space

    if stats[0] & 0o040000: # Is a directory
        total_size = 0
        for file in os.listdir(path):
            subpath = "%s/%s" % (path, file)
            if subpath in exclude:
                continue
            total_size += file_size(subpath, exclude)
        return total_size
    else:
        return stats[6]