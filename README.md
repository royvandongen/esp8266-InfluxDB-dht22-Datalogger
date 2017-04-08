===ESP8266 InfluxDB Data logger ( DHT22 Sensor ) ===

This sketch can be used to create a data logger with a single DHT22 sensor from Adafruit.

In this latest release i made an upgrade, You can now fill in your variables via the WiFiManager ( on first boot ). Just connect to the SENSOR-DHT22-***** network and fill in all the fields.

If one field is missing, the device will become an AP again so you can change the values.

Please note, NTP code is available because this is needed for some logging platforms. At this moment it is not used.

NTP Code written by Niek Blankers https://www.github.com/niekproductions
