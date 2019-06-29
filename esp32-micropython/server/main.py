import os
import sys
import time
import json
import esp32
import ntptime
import machine
import network

from lib import ssd1306
from lib.microWebSrv import MicroWebSrv

from sensor import SensorLogger

reboot_now = False
ssid = ""

# Settings:
settings = {
    "wifi_mode": "ap",
    "ap": {
        "ssid": "powermeter",
        "password": "powermeter",
        },
    "station": {
        "ssid": "power-meter",
        "password": "power-meter",
        },
    }

with open("settings.json") as f:
    try:
        custom_settings = json.loads(f.read())
    except ValueError:
        custom_settings = {}

    settings.update(custom_settings)

# Setting up the display:
i2c = machine.I2C(scl=machine.Pin(4), sda=machine.Pin(5))
disp = ssd1306.SSD1306_I2C(128, 64, i2c)

disp.fill(0)
disp.text("Starting...", 0, 0)
disp.show()

# Network:
wifi_mode = settings.get("wifi_mode", "ap")

if wifi_mode == "ap":
    wlan = network.WLAN(network.AP_IF)
    ssid = settings.get("ap", {}).get("ssid", "esp32")
    wlan.config(
        essid=ssid,
        authmode=network.AUTH_WPA2_PSK,
        password=settings.get("ap", {}).get("password", "esp32"))
    wlan.active(True)
    print("Access point enabled: %s" % ssid)
    print("IP: %s" % wlan.ifconfig()[0])
    disp.text("AP: %s" % ssid, 0, 10)
    disp.text(" %s" % wlan.ifconfig()[0], 0, 20)
    disp.show()

elif wifi_mode == "station":
    wlan = network.WLAN(network.STA_IF)
    wlan.active(True)
    ssid = settings.get("station", {}).get("ssid")
    wlan.connect(ssid, settings.get("station", {}).get("password"))
    print("Connecting to %s..." % ssid)
    start_time = time.ticks_ms()
    while wlan.isconnected() == False:
        if time.ticks_ms() - start_time > 5000:
            wlan.active(False)
            raise Exception("Failed to connect to %s in %.0f seconds." % (ssid, (time.ticks_ms() - start_time)/1000))
    print("Connected to %s" % ssid)
    print("IP: %s" % wlan.ifconfig()[0])
    disp.text("Client: %s" % ssid, 0, 10)
    disp.text(" %s" % wlan.ifconfig()[0], 0, 20)
    disp.show()

# Try to set the clock if we have internet:
# try:
#     ntptime.settime()
# except Exception:
#     pass

# Displaying Network info:
# disp.fill(0)
# disp.text("SSID:", 0, 0)
# disp.text(" %s" % SSID, 0, 10)
# disp.text("Password:", 0, 20)
# disp.text(" %s" % PASSWORD, 0, 30)
# disp.show()

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


@MicroWebSrv.route("/stats")
def hello(httpClient, httpResponse):
  disk_stats = os.statvfs("/")
  httpResponse.WriteResponseJSONOk({
    "time": time.time(),
    "datetime": "%04d-%02d-%02d %02d:%02d:%02d" % time.localtime()[0:6],
    "uptime": int(time.ticks_ms() / 1000),
    "mem_free": gc.mem_free(),
    "disk_size": disk_stats[0] * disk_stats[2], # Block size times total blocks
    "disk_free": disk_stats[0] * disk_stats[3], # Block size times free blocks
    "disk_usage": {
        "static": file_size("/static"),
        "data": file_size("/data"),
        "server": file_size("/", ["/static", "/data"]),
        }
    })


@MicroWebSrv.route("/set_time", "POST")
def set_time(httpClient, httpResponse):
    params = httpClient.GetRequestQueryParams()
    client_time = params.get("time")
    # converting from unix epoch to ours (2000-01-01 00:00:00 UTC):
    EPOCH_DELTA = 946684800 # (date(2000, 1, 1) - date(1970, 1, 1)).days * 24*60*60
    epoch_time = int(client_time) - EPOCH_DELTA
    tm = time.localtime(epoch_time)
    tm = tm[0:3] + (0,) + tm[3:6] + (0,)
    machine.RTC().datetime(tm)
    httpResponse.WriteResponse(202, None, None, None, None)

# (disabled for now)
# @MicroWebSrv.route("/upload/", "POST")
def upload(httpClient, httpResponse):
    global reboot_now
    params = httpClient.GetRequestQueryParams()
    path = params.get("path")
    if path:
        print(path)
        with open(path, "wb") as f:
            while True:
                buf = httpClient.ReadRequestContent(1024)
                if buf:
                    f.write(buf)
                else:
                    break
        if params.get("reboot"):
            print("Request to reboot.")
            reboot_now = True
        print("202")
        httpResponse.WriteResponse(202, None, None, None, None)
    else:
        print("400")
        httpResponse.WriteResponse(400, None, None, None, None)


mws = MicroWebSrv(port=80, webPath="static")
mws.LetCacheStaticContentLevel = 0 # Disable cache headers for now as they aren't fully functional
mws.StaticHeaders = {"Access-Control-Allow-Origin": "*"}
mws.Start(threaded=True)
print("Web server started.")

sense = SensorLogger("data", {
    "panel": {
        "nominal_voltage": 22,
        "voltage_pin": None,
        "current_pin": 36,
        "voltage_factor": 7.126,
        "current_zero": 2.529,
        "current_factor": 14.776,
        },
    "load": {
        "nominal_voltage": 12,
        "voltage_pin": None,
        "current_pin": 39,
        "voltage_factor": 7.126,
        "current_zero": 2.529,
        "current_factor": 14.776,
        }
    })
sense.start(threaded=True)

def check_for_reboot():
    if reboot_now:
        print("Rebooting in 0.2 seconds...")
        time.sleep(0.2)
        print("now!")
        machine.reset()
        print("Should never get here!!")

while time.ticks_ms() < 2000:
    check_for_reboot()
    time.sleep(0.2)

disp.fill(0)
disp.text("Wifi: %s" % ssid, 0, 0)
disp.text(" %s" % wlan.ifconfig()[0], 0, 10)
disp.show()

while True:
    check_for_reboot()
    time.sleep(0.2)

    disp.fill_rect(0, 20, 128, 128, 0)

    disp.text("In:  %.1fV %.1fA" % (sense.get_voltage("panel"), sense.get_current("panel")), 0, 22)
    disp.text("Out: %.1fV %.1fA" % (sense.get_voltage("load"), sense.get_current("load")), 0, 32)
    disp.show()

    gc.collect()

#     clients = len(wlan.status("stations"))

#     disp.fill_rect(0, 40, 128, 128, 0)
#     disp.text("Connected: %s" % clients, 0, 40)
#     disp.show()
