# Wemos Online Thermometer

Based on Wemos mini-D1 hardware and ESP8266
Main goal is to have a thermometer that upload the data whenever there's a Wi-Fi connection and stores them locally.

# Compilation instruction
`./arduino-cli compile --fqbn esp8266:esp8266:d1_mini wemos/wemos_dht_recorder`

## To upload
`./arduino-cli upload -p /dev/ttyUSB0 --fqbn esp8266:esp8266:d1_mini`


# Tech stack
* Arduino based
* MQTT for online transport
* ASCON for crypto