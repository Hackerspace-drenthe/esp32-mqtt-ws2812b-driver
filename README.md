# ESP32 MQTT WS2812B driver
This simple firmware connects to MQTT and listens for a JSON message that specifies the colors of the LEDs.

# Getting started
1. Install make, CMake and Python
2. Run `make prepare` (Downloads submodules and ESP-IDF)
3. Run `make menuconfig` (Configure the firmware)
    1. Go to `Component config`
    2. Scroll all the way down
    3. Go to `MQTT WS2812 driver`
    4. Configure it to your heart's content
4. Run `make flash` (To build and upload it to your ESP32)

Other useful commands:
- `make build` (Builds the project without uploading)
- `make monitor` (Starts the serial monitor)
- `make clean` (Clean the build directory)
