/*
 * Created by Roy van Dongen
 * See https://www.github.com/miepermans/ for details
 *
 * NTP DST code made by Niek Blankers https://www.github.com/niekproductions/
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * version 2 as published by the Free Software Foundation.
 */

/* Please use Arduino JSON library version 5 */


/*
 * Schema
 * 5volt ---------------------------------
 *             |                         |
 *           regulator                   |
 *        |--to 3.3 volt                 |
 *        |    |                 |       |
 *        |    |------10kOhm-----|       |
 *        |    |                 |       |
 *        | ESP8266              |       |
 *        |  GPIO2  -----------------  DHT22
 *        |    |                         |
 *        |    |                         |
 * Ground --------------------------------
 */
#include <FS.h>
#include <ESP8266WiFi.h>
#include <DNSServer.h>            //Local DNS Server used for redirecting all requests to the configuration portal
#include <ESP8266WebServer.h>     //Local WebServer used to serve the configuration portal
#include <WiFiManager.h>          //https://github.com/tzapu/WiFiManager WiFi Configuration Magic

#include <WiFiUdp.h>
#include <TimeLib.h>
#include <DHT.h>

#include <ArduinoJson.h>

#define DHTTYPE DHT22
#define DHTPIN  2
DHT dht(DHTPIN, DHTTYPE);

//InfluxDB Server
char INFLUXDB_SERVER[40];             // Your InfluxDB Server FQDN
char INFLUXDB_PORT[5] = "8089";       // Default InfluxDB UDP Port
char INFLUXDB_INTERVAL[6] = "600";    // Seconds between measurements (600 secs is 10 minutes)
char SENSOR_LOCATION[20] = "test";    // This location is used for the "device=" part of the InfluxDB update

//flag for saving data
bool shouldSaveConfig = false;

//callback notifying us of the need to save config
void saveConfigCallback () {
  Serial.println("Should save config");
  shouldSaveConfig = true;
}

WiFiUDP udp;

//Time settings
#define SECS_PER_MIN  (60UL)
#define SECS_PER_HOUR (3600UL)
#define SECS_PER_DAY  (SECS_PER_HOUR * 24L)
#define SYNC_INTERVAL   1200

const int   timeZone        = 1;                        //Central European Time
bool        autoDST         = true;                     //Enable auto Daylight Savings time
const char* ntpServerName   = "0.pool.ntp.org";         //Fill in your NTP server
bool isDST(int d, int m, int y);
bool isDSTSwitchDay(int d, int m, int y);
const int   NTP_PACKET_SIZE = 48;                       //NTP time stamp is in the first 48 bytes of the message
byte        packetBuffer[ NTP_PACKET_SIZE];             //buffer to hold incoming and outgoing packets
WiFiUDP     Udp;
unsigned int localPort = 8888;                          //If you do not set a localport, NTP wont work

time_t getNtpTime();
void sendNTPpacket(IPAddress &address);

unsigned long   lastdisplayupdate   = 0;
unsigned long   lastInfluxDBupdate  = 0;

void setup(void) {
  Serial.begin ( 115200 );
  
  Serial.println("mounting FS...");
  if (SPIFFS.begin()) {
    Serial.println("mounted file system");
    if (SPIFFS.exists("/config.json")) {
      //file exists, reading and loading
      Serial.println("reading config file");
      File configFile = SPIFFS.open("/config.json", "r");
      if (configFile) {
        Serial.println("opened config file");
        size_t size = configFile.size();
        // Allocate a buffer to store contents of the file.
        std::unique_ptr<char[]> buf(new char[size]);

        configFile.readBytes(buf.get(), size);
        DynamicJsonBuffer jsonBuffer;
        JsonObject& json = jsonBuffer.parseObject(buf.get());
        json.printTo(Serial);
        if (json.success()) {
          Serial.println("\nparsed json");

          strcpy(INFLUXDB_SERVER, json["INFLUXDB_SERVER"]);
          strcpy(INFLUXDB_PORT, json["INFLUXDB_PORT"]);
          strcpy(INFLUXDB_INTERVAL, json["INFLUXDB_INTERVAL"]);
          strcpy(SENSOR_LOCATION, json["SENSOR_LOCATION"]);

        } else {
          Serial.println("failed to load json config");
        }
      }
    }
  } else {
    Serial.println("failed to mount FS");
  }

  WiFiManagerParameter custom_influxdb_server("server", "InfluxDB Server", INFLUXDB_SERVER, 40);
  WiFiManagerParameter custom_influxdb_port("port", "InfluxDB Port (8089)", INFLUXDB_PORT, 5);
  WiFiManagerParameter custom_influxdb_interval("interval", "Sensor Interval (seconds)", INFLUXDB_INTERVAL, 6);
  WiFiManagerParameter custom_sensor_location("location", "Sensor ID/Location", SENSOR_LOCATION, 6);

  WiFiManager wifiManager;
  //reset saved settings
  //wifiManager.resetSettings();

  wifiManager.setSaveConfigCallback(saveConfigCallback);

  wifiManager.addParameter(&custom_influxdb_server);
  wifiManager.addParameter(&custom_influxdb_port);
  wifiManager.addParameter(&custom_influxdb_interval);
  wifiManager.addParameter(&custom_sensor_location);
  
  String hostname = "SENSOR-DHT22-" + String(ESP.getChipId());
  WiFi.hostname("SENSOR-DHT22-" + String(ESP.getChipId()));
  
  String ssid = "SENSOR-DHT22-" + String(ESP.getChipId());
  if(!wifiManager.autoConnect(ssid.c_str())) {
    Serial.println("failed to connect and hit timeout");
    delay(3000);
    //reset and try again, or maybe put it to deep sleep
    ESP.reset();
    delay(5000);
  }

  //read updated parameters
  strcpy(INFLUXDB_SERVER, custom_influxdb_server.getValue());
  strcpy(INFLUXDB_PORT, custom_influxdb_port.getValue());
  strcpy(INFLUXDB_INTERVAL, custom_influxdb_interval.getValue());
  strcpy(SENSOR_LOCATION, custom_sensor_location.getValue());
  Serial.println(custom_influxdb_port.getValue());

  //save the custom parameters to FS
  if (shouldSaveConfig) {
    Serial.println("saving config");
    DynamicJsonBuffer jsonBuffer;
    JsonObject& json = jsonBuffer.createObject();
    json["INFLUXDB_SERVER"] = INFLUXDB_SERVER;
    json["INFLUXDB_PORT"] = INFLUXDB_PORT;
    json["INFLUXDB_INTERVAL"] = INFLUXDB_INTERVAL;
    json["SENSOR_LOCATION"] = SENSOR_LOCATION;

    File configFile = SPIFFS.open("/config.json", "w");
    if (!configFile) {
      Serial.println("failed to open config file for writing");
    }

    json.printTo(Serial);
    json.printTo(configFile);
    configFile.close();
    //end save
  }

  Serial.print ( "Connected to your network" );
  Serial.print ( "IP address: " );
  Serial.println ( WiFi.localIP() );

  Serial.print ( "Starting UDP: " );
  Udp.begin(localPort);

  setSyncProvider(getNtpTime);
  setSyncInterval(SYNC_INTERVAL);

  if(strlen(INFLUXDB_SERVER) == 0 || strlen(INFLUXDB_PORT) == 0 || strlen(INFLUXDB_INTERVAL) == 0 || strlen(SENSOR_LOCATION) == 0) {
    Serial.print("Config Faulty, Kicking config");
    SPIFFS.format();
    wifiManager.resetSettings();
    delay(2000);
    ESP.reset();
  }
}

void loop(void) {

  // only update clock every 50ms
  if(millis()-lastdisplayupdate > 50) {
    lastdisplayupdate = millis();
  }else{
    return;
  }
    if(timeStatus() == timeNotSet) {
    Serial.println("time not yet known");
  }
  time_t t = now();

  // only send update to InfluxDB every INFLUXDB_INTERVAL * 1000
  int INFLUXDB_INTERVAL_INT = atoi(INFLUXDB_INTERVAL);
  int INFLUXDB_INTERVAL_MILLIS = INFLUXDB_INTERVAL_INT * 1000;
  
  if(millis()-lastInfluxDBupdate > INFLUXDB_INTERVAL_MILLIS) {
    lastInfluxDBupdate = millis();

    sendData(t);
  }
}

/*-------- NTP code ----------*/

time_t getNtpTime()
{
    IPAddress ntpServerIP; // NTP server's ip address

    while (Udp.parsePacket() > 0) ; // discard any previously received packets
    Serial.println("Transmit NTP Request");
    // get a random server from the pool
    WiFi.hostByName(ntpServerName, ntpServerIP);
    Serial.print(ntpServerName);
    Serial.print(": ");
    Serial.println(ntpServerIP);
    sendNTPpacket(ntpServerIP);
    uint32_t beginWait = millis();
    while (millis() - beginWait < 1500) {
        int size = Udp.parsePacket();
        if (size >= NTP_PACKET_SIZE) {
            Serial.println("Receive NTP Response");
            Udp.read(packetBuffer, NTP_PACKET_SIZE);  // read packet into the buffer
            unsigned long secsSince1900;
            // convert four bytes starting at location 40 to a long integer
            secsSince1900 =  (unsigned long)packetBuffer[40] << 24;
            secsSince1900 |= (unsigned long)packetBuffer[41] << 16;
            secsSince1900 |= (unsigned long)packetBuffer[42] << 8;
            secsSince1900 |= (unsigned long)packetBuffer[43];
            // New time in seconds since Jan 1, 1970
            unsigned long newTime = secsSince1900 - 2208988800UL +
                timeZone * SECS_PER_HOUR;

            // Auto DST
            if (autoDST) {
                if (isDSTSwitchDay(day(newTime), month(newTime), year(newTime))) {
                    if (month(newTime) == 3 && hour(newTime) >= 2) {
                        newTime += SECS_PER_HOUR;
                    } else if (month(newTime) == 10 && hour(newTime) < 2) {
                        newTime += SECS_PER_HOUR;
                    }
                } else if (isDST(day(newTime), month(newTime), year(newTime))) {
                    newTime += SECS_PER_HOUR;
                }
            }

            setSyncInterval(SYNC_INTERVAL);
            return newTime;
        }
    }
    Serial.println("No NTP Response :-(");
    // Retry soon
    setSyncInterval(10);
    return 0; // return 0 if unable to get the time
}

// send an NTP request to the time server at the given address
void sendNTPpacket(IPAddress &address)
{
    // set all bytes in the buffer to 0
    memset(packetBuffer, 0, NTP_PACKET_SIZE);
    // Initialize values needed to form NTP request
    // (see URL above for details on the packets)
    packetBuffer[0] = 0b11100011;   // LI, Version, Mode
    packetBuffer[1] = 0;     // Stratum, or type of clock
    packetBuffer[2] = 6;     // Polling Interval
    packetBuffer[3] = 0xEC;  // Peer Clock Precision
    // 8 bytes of zero for Root Delay & Root Dispersion
    packetBuffer[12] = 49;
    packetBuffer[13] = 0x4E;
    packetBuffer[14] = 49;
    packetBuffer[15] = 52;
    // all NTP fields have been given values, now
    // you can send a packet requesting a timestamp:
    Udp.beginPacket(address, 123); //NTP requests are to port 123
    Udp.write(packetBuffer, NTP_PACKET_SIZE);
    Udp.endPacket();
}

// Check if Daylight saving time (DST) applies
// Northern Hemisphere - +1 hour between March and October
bool isDST(int d, int m, int y){
    bool dst = false;
    dst = (m > 3 && m < 10); // October-March

    if (m == 3){
        // Last sunday of March
        dst = (d >= ((31 - (5 * y /4 + 4) % 7)));
    }else if (m == 10){
        // Last sunday of October
        dst = (d < ((31 - (5 * y /4 + 1) % 7)));
    }

    return dst;
}

bool isDSTSwitchDay(int d, int m, int y){
    bool dst = false;
    if (m == 3){
        // Last sunday of March
        dst = (d == ((31 - (5 * y /4 + 4) % 7)));
    }else if (m == 10){
        // Last sunday of October
        dst = (d == ((31 - (5 * y /4 + 1) % 7)));
    }
    return dst;
}

void sendData( time_t time ) {
 
  float t = dht.readTemperature();
  if(int(t) > 80) { //Temperature is in degrees Celsius Ceilingvalue is 80 which is the sensor max.
    //Drop temperature measurement because of false value
    Serial.print("Temperature measurement dropped because of false value: ");
    Serial.println(t);
  } else {
    String line;
    line = String("temperature,device=" + String(SENSOR_LOCATION) + " value=" + String((int)t));
    Serial.println(line);

    // send the packet
    Serial.println("Sending first UDP packet...");
    udp.beginPacket(INFLUXDB_SERVER, atoi(INFLUXDB_PORT));
    udp.print(line);
    udp.endPacket();
  }
  


  float h = dht.readHumidity();
  if(int(h) > 101) { //Humidity is in percentage Ceilingvalue is 101% which is more than max.
    //Drop humidity measurement because of false value
    Serial.print("Humidity measurement dropped because of false value: ");
    Serial.println(t);    
  } else {
    String line;
    line = String("humidity,device=" + String(SENSOR_LOCATION) + " value=" + String((int)h));
    Serial.println(line);
  
    // send the packet
    Serial.println("Sending second UDP packet...");
    udp.beginPacket(INFLUXDB_SERVER, atoi(INFLUXDB_PORT));
    udp.print(line);
    udp.endPacket();
  }
}
