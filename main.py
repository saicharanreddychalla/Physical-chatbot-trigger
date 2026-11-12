import network
import time

print("Starting Wi-Fi connection...")

wifi = network.WLAN(network.STA_IF)
wifi.active(True)

wifi.connect("Wokwi-GUEST", "")

while not wifi.isconnected():
    print("Connecting...")
    time.sleep(0.5)

print("Wi-Fi connected!")
print("IP address:", wifi.ifconfig()[0])