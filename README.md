## Flash

Clone this repository, `cd` into it. Connect wires to the Sonoff S20 (I've been using a [FT2232 breakout board](http://dangerousprototypes.com/docs/FT2232_breakout_board)). Flash using [PlatformIO CLI](https://docs.platformio.org/en/latest/installation.html).  

Run:

    platformio run -t upload

In case you have multiple USB-to-Serial adapters attached, specify:

    platformio run -t upload --upload-port /dev/cu.usbserial-00202102A 

For OTA upload:

    platformio run -t upload --upload-port SonoffS20_1234567.local

### Serial Monitor

Debug with:

    platformio device monitor

or

    platformio device monitor --port /dev/cu.usbserial-00202102A


[settings_arduino]: https://github.com/arendst/Tasmota/wiki/Arduino-IDE
[settings_platformio]: https://docs.platformio.org/en/stable/boards/espressif8266/sonoff_s20.html
[settings_platformio_raw]: https://github.com/platformio/platform-espressif8266/blob/master/boards/sonoff_s20.json
[pinout]: https://esphome.io/devices/sonoff_s20.html