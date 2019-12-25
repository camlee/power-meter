import os
import gc
import sys
import time
import json
import esp32
import ntptime
import machine
import network
from _thread import start_new_thread

import ssd1306
from microWebSrv import MicroWebSrv

from sensor import SensorLogger
from util import set_time_from_epoch, epoch_time, file_size

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

with open("sensor_config.json") as f:
    sensor_config = json.loads(f.read())

sense = SensorLogger("static/data/", sensor_config)
# sense.start(threaded=True)


def set_time_if_provided(httpClient):
    params = httpClient.GetRequestQueryParams()
    client_time = params.get("time")
    if client_time is not None:
        set_time_from_epoch(client_time)
        sense.time_updated()


@MicroWebSrv.route("/stats")
def stats(httpClient, httpResponse):
    try:
        set_time_if_provided(httpClient)
        disk_stats = os.statvfs("/")
        gc.collect()
        mem_free = gc.mem_free()
        httpResponse.WriteResponseJSONOk({
            "time": time.time(),
            "datetime": "%04d-%02d-%02d %02d:%02d:%02d" % time.localtime()[0:6],
            "uptime": int(time.ticks_ms() / 1000),
            "mem_free": mem_free,
            "disk_size": disk_stats[0] * disk_stats[2], # Block size times total blocks
            "disk_free": disk_stats[0] * disk_stats[3], # Block size times free blocks
            "disk_usage": {
                "static": file_size("/static", ["/static/data"]),
                "data": file_size("/static/data"),
                "server": file_size("/", ["/static"]),
                }
        })
    except Exception as e:
        print("Unhandled exception in /stats: %s" % e)
        raise

@MicroWebSrv.route("/set_time", "POST")
def set_time(httpClient, httpResponse):
    set_time_if_provided(httpClient)
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

open_web_sockets = set()

def ws_accept_callback(webSocket, httpClient) :
    # print("Accepted web socket")
    try:
        open_web_sockets.add(webSocket)
        webSocket.RecvTextCallback = ws_receive_text_callback
        webSocket.RecvBinaryCallback = ws_receive_binary_callback
        webSocket.ClosedCallback = ws_close_callback
    except Exception as e:
        print(e)

def ws_receive_text_callback(webSocket, msg):
    try:
        key, value = msg.split("=")
        client_time = int(value)
    except ValueError:
        pass
    else:
        set_time_from_epoch(client_time)
        sense.time_updated()

def ws_receive_binary_callback(webSocket, data):
    print("Received binary from web socket: %s" % data)

def ws_close_callback(webSocket):
    # print("Web socket closed")
    open_web_sockets.remove(webSocket)

mws = MicroWebSrv(port=80, webPath="static")
mws.LetCacheStaticContentLevel = 0 # Disable cache headers for now as they aren't fully functional
mws.StaticHeaders = {"Access-Control-Allow-Origin": "*"}
mws.AcceptWebSocketCallback = ws_accept_callback
mws.Start(threaded=True)
print("Web server started.")


def check_for_reboot():
    if reboot_now:
        print("Rebooting in 1 second...")
        time.sleep(1)
        print("now!")
        machine.reset()
        print("Should never get here!!")

# while time.ticks_ms() < 3000:
#     check_for_reboot()
#     time.sleep(0.2)

disp.fill(0)
disp.text("Wifi: %s" % ssid, 0, 0)
disp.text(" %s" % wlan.ifconfig()[0], 0, 10)
disp.show()


last_main = time.ticks_ms()
while True:
    time.sleep_ms(1)
    sense.refresh()

    if time.ticks_diff(time.ticks_ms(), last_main) >= 1000:
        last_main = time.ticks_ms()

        check_for_reboot()

        start_time = time.ticks_ms()
        disp.fill_rect(0, 20, 128, 128, 0)
        disp.text("In:  %.2fA %.0fW" % (sense.get_current("in") or 0, sense.get_power("in") or 0), 0, 22)
        disp.text("Out: %.2fA %.0fW" % (sense.get_current("out") or 0, sense.get_power("out") or 0), 0, 32)
        disp.text("%02d:%02d:%02d UTC" % time.localtime()[3:6], 0, 55)
        disp.show()

        ws_text = ",".join([str(epoch_time() * 1000), str(sense.get_power("in")), str(sense.get_power("out"))])
        for ws in open_web_sockets:
            start_time = time.ticks_ms()
            ws.SendText(ws_text)

        gc.collect()

#     clients = len(wlan.status("stations"))

#     disp.fill_rect(0, 40, 128, 128, 0)
#     disp.text("Connected: %s" % clients, 0, 40)
#     disp.show()
