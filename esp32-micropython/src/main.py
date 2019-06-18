import os
import sys
import time
import json
import machine
import network
from lib import ssd1306
from lib.microWebSrv import MicroWebSrv

reboot_now = False

# Settings:
settings = {
    "wifi_mode": "station",
    "ap": {
        "ssid": "power-meter",
        "password": "power-meter",
        },
    "station": {
        "ssid": "power-meter",
        "password": "power-meter",
        },
    }

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
    start_time = time.time()
    while wlan.isconnected() == False:
        if time.time() - start_time > 5:
            wlan.active(False)
            raise Exception("Failed to connect to %s in %s seconds." % (ssid, time.time() - start_time))
    print("Connected to %s" % ssid)
    print("IP: %s" % wlan.ifconfig()[0])
    disp.text("Client: %s" % ssid, 0, 10)
    disp.text(" %s" % wlan.ifconfig()[0], 0, 20)
    disp.show()

# Displaying Network info:
# disp.fill(0)
# disp.text("SSID:", 0, 0)
# disp.text(" %s" % SSID, 0, 10)
# disp.text("Password:", 0, 20)
# disp.text(" %s" % PASSWORD, 0, 30)
# disp.show()


@MicroWebSrv.route("/stats")
def hello(httpClient, httpResponse):
  disk_stats = os.statvfs("/")
  httpResponse.WriteResponseJSONOk({
    "time": time.time(),
    "mem_free": gc.mem_free(),
    "disk_size": disk_stats[0] * disk_stats[2], # Block size times total blocks
    "disk_free": disk_stats[0] * disk_stats[3], # Block size times free blocks
    })

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
mws.Start(threaded=True)
print("Web server started.")


while True:
    time.sleep(0.2)
    if reboot_now:
        print("Rebooting in 0.2 seconds...")
        time.sleep(0.2)
        print("now!")
        machine.reset()
        print("Should never get here!!")
    gc.collect()

#     clients = len(wlan.status("stations"))

#     disp.fill_rect(0, 40, 128, 128, 0)
#     disp.text("Connected: %s" % clients, 0, 40)
#     disp.show()
