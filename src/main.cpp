#include <ESP8266WiFi.h>
#include <ESP8266mDNS.h>
#include <WiFiUdp.h>
#include <LittleFS.h>
#include <ArduinoOTA.h>
#include <Wire.h>
// add the below libraries from the Library Manager
#include <PubSubClient.h>
#include <ezTime.h>

// private definitions
#include "private.h"               // <<<<<<<  COMMENT THIS OUT FOR YOUR INSTANCE - this contains stuff for my network, not yours

#define VERSION "Ver 3.1 build 2021.08.22"

// i2c pins are usually D1 & D2, but this application requires use of D1 & D2, so
// D6 & D7 are used instead - see Valve Control Settings below for explanation
#define I2C_ADDR 0x28
#define MAX_PRESSURE 100
#define PIN_SDA D6 // (GPIO12)  Pins where i2c
#define PIN_SCL D7 // (GPIO13)  SDA & SDL are attached

// Valve control settings
// On the ESP8266 pins D1 & D2 are the only two GPIOs that do not glitch HIGH at startup/reset.
// Since the valve relay wiring cannot tolerate the glitch (will short the power), we must use D1 & D2 here.
#define PIN_VALVE_ON D1                // (GPIO5)   Valve works by reversing voltage on a set of two wires
#define PIN_VALVE_OFF D2               // (GPIO4)   DO NOT SET VALVE_ON & VALVE_OFF high at the same time!! It will short out the power supply!!
#define PIN_VALVE_ON_INDICATOR D0      // (GPIO16)  Confirms valve is in ON position when signal high
#define PIN_VALVE_OFF_INDICATOR D5     // (GPIO14)  Confirms valve is in OFF position when signal high

// Flow meter
#define PIN_FLOW_SIGNAL D8             // (GPIO15) Unimplemented

// Name your device here
#define DEVICE_NAME "watermain"

#ifndef WIFI_SSID
#define WIFI_SSID "your_ssid"            //  <<<<<<<  REPLACE WITH YOUR CREDENTIALS
#endif
#ifndef WIFI_PASSWORD
#define WIFI_PASSWORD "your_password"    //  <<<<<<<  REPLACE WITH YOUR CREDENTIALS
#endif

// Time settings
#define MY_TIMEZONE "America/New_York"               // <<<<<<< use Olson format: https://en.wikipedia.org/wiki/List_of_tz_database_time_zones

// MQTT
#define MQTT_USER_NAME "watermain"                   // <<<<<<< replace with your MQTT login
#define MQTT_PASSWORD "watermain"                    // <<<<<<< replace with your MQTT password
#define MQTT_SERVER "haha.shencentral.net"           // <<<<<<< use either your MQTT broker DNS name or IP address surrounded by quotes

#define MSG_BUFFER_SIZE 512                          // for MQTT message payload
#define VERSION_TOPIC "watermain/report/version"     // report software version at connect
#define LAST_BOOT_TOPIC "watermain/report/last_boot" // send boot (not reconnect) time to broker when connected
#define LWT_TOPIC "watermain/status/LWT"             // MQTT Last Will & Testament
#define REPORT_TOPIC "watermain/report/params"       // used to send program operating parameters
#define HELP_TOPIC "watermain/report/help"           // used to send program operating info
#define PRESSURE_TOPIC "watermain/water_pressure"
#define TEMPERATURE_TOPIC "watermain/water_temperature"
#define PRESSURE_SENSOR_FAULT_TOPIC "watermain/report/last_press_sensor_fault" // sends timestamp if pressure error can't be read
#define VALVE_TOPIC "watermain/valve_zeroisclosed"                             // valve position 0 = closed, 1= open
#define LAST_VALVE_STATE_UNK_TOPIC "watermain/report/last_unk_valve_state"     // send timestamp if valve state cannot be determined from indicator inputs
#define SPT_DATA_STATUS_TOPIC "watermain/spt_data_status"                      // 0 when test in progress, 1 when finished
#define SPT_RESULT_TOPIC "watermain/spt_result"                                // send at end of Static Pressure Test - end pressure minus start pressure
#define RECV_COMMAND_TOPIC "watermain/cmd/#"

// Operational parameters & preferences
#define PREFER_FAHRENHEIT 1                          // temperature reported in Celsius unless this is set to 1
#define INITIAL_VALVE_INSTALLED_STATE 1              // 1 if installed, 0 if not - runtime state can be changed & saved in NVRAM via MQTT command
#define INITIAL_PRESSURE_SENSOR_INSTALLED_STATE 1    // 1 if installed, 0 if not - runtime state can be changed & saved in NVRAM via MQTT command
#define PARAMS_FILENAME "/params.bin"
#define VALVE_STATE_FILENAME "/valve_state.bin"
#define OPEN_VALVE 1
#define CLOSE_VALVE 0
#define PRESSURE_SETTLING_DELAY_MS 2000              // wait for pressure to settle a bit after closing valve for SPT
#define VALVE_ROTATION_TIME_MS 10000                 // time required for valve to open/close - relays are only active long enough for the valve to rotate
#define VALVE_ERROR_DEFAULT 0                        // 0=CLOSED, 1=OPEN - how the valve will default if everything goes badly - also used if manual switch has left valve between OPEN/CLOSED
#define VALVE_SYNC_INTERVAL_MS 30000                 // how often actual valve switch will be checked & synced with software valveState (in case manual button has been used)
#define DEFAULT_IDLE_PUBLISH_INTERVAL_MS 300000      // how often sensor data is published if no event driven changes
#define DEFAULT_MIN_PUBLISH_INTERVAL_MS 5000         // don't publish more often than this in non-SPT operation
#define SPT_MIN_PUBLISH_INTERVAL_MS 1000             // don't publish more often than this during SPT
#define PRESSURE_SENSOR_FAULT_PUB_INTERVAL_MS 60000  // how often a pressure sensor error (timestmap) is published if error condition true
#define DEFAULT_SENSOR_READ_INTERVAL_MS 500          // how often the sensor is read (how soon PSI changes are recognized)
#define DEFAULT_SPT_REPORT_PSI_DROP .3               // amount of change in PSI to initiate a publishing event
#define DEFAULT_SPT_DEMAND_WATER_PERCENT_DROP 30     // percent of sudden pressure drop during Static Pressure Test required to assume pressure drop is intentional (water is needed)
#define DEFAULT_SPT_TEST_DURATION_MINUTES 10         // duration of Static Pressure Test (valve off & observe pressure change)
#define SPT_DATA_IN_PROCESS "in_process"             // SPT test is in process and the reported SPT result is old
#define SPT_DATA_VALID "valid"                       // SPT test has completed normally and the SPT result is valid
#define SPT_DATA_ABORTED "aborted"                   // SPT test has terminated abnormally and resultant data is not valid (test must be run again)
#define SPT_DATA_INVALID "not_valid"                 // SPT test has not been run

#define TIMEZONE_EEPROM_OFFSET 0                     // location-to-timezone info - saved in case eztime server is down

#define DEBUG_SPT false                               // Disable valve synce for testing <<<<<  DON'T FORGET TO CHANGE THIS BACK TO false AFTER TESTING <<<<<<<<<<<<<

char msg[MSG_BUFFER_SIZE];
char lastBoot[50];
unsigned long lastReconnectAttempt = 0;
unsigned long lastPublish = 0, lastRead = 0, lastValveSync = 0, lastPressErrReport = 0;
unsigned long tempNow, lastPublishNow, sensorReadNow, mqttNow, valveNow, lastValveSyncNow, lastPressErrReportNow;
byte sensorStatus;
float psiTminus0 = 0, psiTminus1 = 0, psiTminus2 = 0;            // psiTminus0 is the current pressure, psiTminus1 is the previous, psiTminus2 is the one before
float medianPressure, sptBeginningPressure, temperature;
unsigned int pre_spt_idlePublishInterval, pre_spt_minPublishInterval;

struct Parameters
{
  char version[30];
  unsigned int valveInstalled;
  unsigned int pressureInstalled;
  unsigned int idlePublishInterval;
  unsigned int minPublishInterval;
  unsigned int sensorReadInterval;
  float sptPressureDrop;
  float sptDemandWaterPercentDrop;
  unsigned int sptDuration;
  byte filler;  // NVM requires even number of bytes for storage
};

struct Parameters opParams;

byte valvePreSPT, valveState = VALVE_ERROR_DEFAULT; // if all saved data is lost, VALVE_ERROR_DEFAULT is the valve setting
unsigned int sptConsecAborts = 0;
char sptDataStatus[12];
File paramFileObj, valveFileObj;
WiFiClient espClient;
PubSubClient mqttClient(espClient);
Timezone myTZ;

//   ***************************
//   **  WiFi initialization  **
//   ***************************

void setup_wifi()
{
  delay(10); // let Serial comm stream start before using it

  // We start by connecting to a WiFi network

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  Serial.print(F("\nWaiting for WiFi "));
  while (WiFi.status() != WL_CONNECTED)
  {
    Serial.print(F("."));
    delay(500);
  }

  randomSeed(micros());

  Serial.println(F(""));
  Serial.print(F("WiFi connected to "));
  Serial.println(WIFI_SSID);
  Serial.print(F("IP address: "));
  Serial.println(WiFi.localIP());
}

//   ************************
//   ** OTA initialization **
//   ************************

void setup_OTA()
{
  // Port defaults to 8266
  // ArduinoOTA.setPort(8266);

  // Hostname defaults to esp8266-[ChipID]
  // ArduinoOTA.setHostname(DEVICE_NAME);

  // No authentication by default
  // ArduinoOTA.setPassword("admin");

  // Password can be set with it's md5 value as well
  // MD5(admin) = 21232f297a57a5a743894a0e4a801fc3
  // ArduinoOTA.setPasswordHash("21232f297a57a5a743894a0e4a801fc3");

  ArduinoOTA.onStart([]() {
    String type;
    if (ArduinoOTA.getCommand() == U_FLASH)
    {
      type = "sketch";
    }
    else
    { // U_FS
      type = "filesystem";
    }

    // NOTE: if updating FS this would be the place to unmount FS using FS.end()
    paramFileObj.close();
    valveFileObj.close();
    LittleFS.end();          //  <<<<<< This line required to prevent FS damage
    mqttClient.disconnect(); // let broker know it is expected

    Serial.println("Start updating " + type);
  });
  ArduinoOTA.onEnd([]() {
    Serial.println(F("\nEnd"));
  });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    Serial.printf("Progress: %u%%\r", (progress / (total / 100)));
  });
  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("Error[%u]: ", error);
    if (error == OTA_AUTH_ERROR)
    {
      Serial.println(F("Auth Failed"));
    }
    else if (error == OTA_BEGIN_ERROR)
    {
      Serial.println(F("Begin Failed"));
    }
    else if (error == OTA_CONNECT_ERROR)
    {
      Serial.println(F("Connect Failed"));
    }
    else if (error == OTA_RECEIVE_ERROR)
    {
      Serial.println(F("Receive Failed"));
    }
    else if (error == OTA_END_ERROR)
    {
      Serial.println(F("End Failed"));
    }
  });
  ArduinoOTA.begin();
}

//   *************************
//   **  applyValveState()  **
//   *************************

// 0 = closed 1 = open
boolean applyValveState(int desiredState, boolean writeFlag) // this routine uses the global char msg[], it does not set valveState global
{
  char val[3];
  switch (desiredState)
  {
  case 0:
    digitalWrite(PIN_VALVE_OFF, HIGH);                       // turn on just enough to rotate valve
    Serial.print(F("Closing valve..."));
    valveNow = millis();
    while (millis() - valveNow < VALVE_ROTATION_TIME_MS)
      yield();
    digitalWrite(PIN_VALVE_OFF, LOW);
    Serial.println(F("valve is CLOSED (state=0)"));
    sprintf(val, "%d", desiredState);
    mqttClient.publish(VALVE_TOPIC, val, true);
    Serial.printf("%s MQTT SENT: %s/%s \n", myTZ.dateTime("[H:i:s.v]").c_str(), VALVE_TOPIC, val);

    if (writeFlag == true)
    {
      valveFileObj = LittleFS.open(F(VALVE_STATE_FILENAME), "r+");
      if (valveFileObj.write((uint8_t *)&desiredState, sizeof(desiredState)) > 0)
        Serial.println(F("Valve state saved"));
      else
        Serial.println(F("Valve file update error"));
      valveFileObj.close();
    }
    return (true);
    break;
  case 1:
    digitalWrite(PIN_VALVE_ON, HIGH);                        // turn on just enough to rotate valve
    Serial.print(F("Opening valve..."));
    valveNow = millis();
    while (millis() - valveNow < VALVE_ROTATION_TIME_MS)
      yield();
    digitalWrite(PIN_VALVE_ON, LOW);
    Serial.println(F("valve is OPEN (state=1)"));
    sprintf(val, "%d", desiredState);
    mqttClient.publish(VALVE_TOPIC, val, true);
    Serial.printf("%s MQTT SENT: %s/%s \n", myTZ.dateTime("[H:i:s.v]").c_str(), VALVE_TOPIC, val);

    if (writeFlag == true)
    {
      valveFileObj = LittleFS.open(F(VALVE_STATE_FILENAME), "r+");
      if (valveFileObj.write((uint8_t *)&desiredState, sizeof(desiredState)) > 0)
        Serial.println(F("Valve state saved"));
      else
        Serial.println(F("Valve file update error"));
      valveFileObj.close();
    }
    return (true);
    break;
  default:
    Serial.println(F("Invalid valveState requested"));
    return (false);
  }
}

//   ***********************
//   **      sptEnd()     **
//   ***********************
void sptEnd()
{
  if (valveState == CLOSE_VALVE)  // SPT has terminated normally if valve has not been opened during test
  {
    // Publish result
    Serial.printf("%s SPT Ending Pressure = %.2f \n", myTZ.dateTime("[H:i:s.v]").c_str(), medianPressure);
    sprintf(msg, "%.2f", medianPressure - sptBeginningPressure);
    mqttClient.publish(SPT_RESULT_TOPIC, msg, false);      // do not publish as with retain flag
    Serial.printf("%s  MQTT SENT: %s/%s \n", myTZ.dateTime("[H:i:s.v]").c_str(), SPT_RESULT_TOPIC, msg);

    // Publish data ready
    strcpy(sptDataStatus, SPT_DATA_VALID);
    sprintf(msg, "%s", sptDataStatus);
    mqttClient.publish(SPT_DATA_STATUS_TOPIC, msg, true);
    Serial.printf("%s  MQTT SENT: %s/%s \n", myTZ.dateTime("[H:i:s.v]").c_str(), SPT_DATA_STATUS_TOPIC, msg);

    // Publish sptConsecAbort as attribute
    sptConsecAborts = 0;
    sprintf(msg, "{\"consec_aborts\": \"%d\"}", sptConsecAborts);
    mqttClient.publish(SPT_DATA_STATUS_TOPIC"/attributes", msg, true);  
    Serial.printf("%s  MQTT SENT: %s/%s \n", myTZ.dateTime("[H:i:s.v]").c_str(), SPT_DATA_STATUS_TOPIC"/attributes", msg);
    Serial.println(F("SPT event end: Normal"));
  }
  else  // SPT terminated abnormally - manual has intervention occured, so test is not valid
  {
    
    // set status to ABORTED
    strcpy(sptDataStatus, SPT_DATA_ABORTED);
    sprintf(msg, "%s", sptDataStatus);
    mqttClient.publish(SPT_DATA_STATUS_TOPIC, msg, true);
    Serial.printf("%s  MQTT SENT: %s/%s \n", myTZ.dateTime("[H:i:s.v]").c_str(), SPT_DATA_STATUS_TOPIC, msg);
    
    // report consecutive aborts attribute
    sptConsecAborts++;
    sprintf(msg, "{\"consec_aborts\": \"%d\"}", sptConsecAborts);
    mqttClient.publish(SPT_DATA_STATUS_TOPIC"/attributes", msg, true);  
    Serial.printf("%s  MQTT SENT: %s/%s \n", myTZ.dateTime("[H:i:s.v]").c_str(), SPT_DATA_STATUS_TOPIC"/attributes", msg);

    Serial.println(F("SPT event end: Aborted due to manual intervention"));

  }

  // Publish attributes
  sprintf(msg, "{\"test_end\": \"%s\", \"test_minutes\": \"%d\", \"beginning_pressure\": \"%.2f\", \"ending_pressure\": \"%.2f\"}",
        myTZ.dateTime(RFC3339).c_str(), opParams.sptDuration, sptBeginningPressure, medianPressure);
  mqttClient.publish(SPT_RESULT_TOPIC"/attributes", msg, false);    // do not publish with retain flag
  Serial.printf("%s  MQTT SENT: %s/%s \n", myTZ.dateTime("[H:i:s.v]").c_str(), SPT_RESULT_TOPIC"/attributes", msg);

  // Restore previous states
  opParams.idlePublishInterval = pre_spt_idlePublishInterval;  // restore idlePublishInterval
  opParams.minPublishInterval = pre_spt_minPublishInterval;    // restore minPublishInterval
  valveState = valvePreSPT;
  applyValveState(valvePreSPT, false);                         // restore the valveState to state before test
}

//   ***********************
//   **  MQTT reconnect() **
//   ***********************

boolean reconnect()
{
  // PubSubClient::connect(const char *id, const char *user, const char *pass, const char* willTopic, uint8_t willQos, boolean willRetain, const char* willMessage)
  if (mqttClient.connect(DEVICE_NAME, MQTT_USER_NAME, MQTT_PASSWORD, LWT_TOPIC, 2, true, "Disconnected"))
  {
    Serial.print(F("MQTT connected to "));
    Serial.println(F(MQTT_SERVER));

    if (opParams.valveInstalled == 1)
    {
      // Sync valveState

      // if actual valve state cannot be determined, then use last saved state & set valve to match
      if ((digitalRead(PIN_VALVE_ON_INDICATOR) == LOW) && (digitalRead(PIN_VALVE_OFF_INDICATOR) == LOW))
      {
        Serial.println(F("Actual valve state cannot be determined. Setting valve to last saved state."));

        if (LittleFS.exists(F(VALVE_STATE_FILENAME))) // if file exists
        {
          valveFileObj = LittleFS.open(F(VALVE_STATE_FILENAME), "r+");
          valveState = valveFileObj.read();
          if (valveState != -1)
          {
            Serial.printf("Last valveState loaded from file: valveState = %d\n", valveState);
          }
          else
          {
            Serial.println(F("valveState file read error.  valveState set to defined VALVE_ERROR_DEFAULT"));
            valveState = VALVE_ERROR_DEFAULT;
            if (valveFileObj.write((uint8_t *)&valveState, sizeof(valveState)) > 0)
              Serial.printf("Valve file re-created: %s, %d bytes\n", valveFileObj.name(), valveFileObj.size());
            else
              Serial.println(F("Valve file re-creation error"));
          }
          valveFileObj.close();
        }
        else
        { // fill it with default value
          Serial.println(F("No valve file detected"));
          valveState = 0;
          valveFileObj = LittleFS.open(F(VALVE_STATE_FILENAME), "w+");
          if (valveFileObj.write((uint8_t *)&valveState, sizeof(valveState)) > 0)
            Serial.printf("Valve file created: %s, %d bytes\n", valveFileObj.name(), valveFileObj.size());
          else
            Serial.println(F("Valve file creation error"));
          valveFileObj.close();
        }
        applyValveState(valveState, false);                  // no need to write again, so just update MQTT
      }
      else
      {
        if ((digitalRead(PIN_VALVE_ON_INDICATOR) == HIGH) && (valveState != 1))
        {
          Serial.println(F("ValveState set to actual: valveState=1"));
          valveState = 1;
          applyValveState(valveState, true);
        }
        if ((digitalRead(PIN_VALVE_OFF_INDICATOR) == HIGH) && (valveState != 0))
        {
          Serial.println(F("ValveState set to actual: valveState=0"));
          valveState = 0;
          applyValveState(valveState, true);
        }
      }
    }

    // Publish MQTT announcements...

    mqttClient.publish(LWT_TOPIC, "Connected", true); // let broker know we're connected
    Serial.printf("\n%s MQTT SENT: %s/Connected\n", myTZ.dateTime("[H:i:s.v]").c_str(), LWT_TOPIC);

    mqttClient.publish(VERSION_TOPIC, VERSION, true); // report firmware version
    Serial.printf("%s MQTT SENT: Firmware %s\n", myTZ.dateTime("[H:i:s.v]").c_str(), VERSION);

    mqttClient.publish(LAST_BOOT_TOPIC, lastBoot, true);
    Serial.printf("%s MQTT SENT: %s/%s \n", myTZ.dateTime("[H:i:s.v]").c_str(), LAST_BOOT_TOPIC, lastBoot);

    mqttClient.publish(SPT_DATA_STATUS_TOPIC, sptDataStatus, true); // refresh SPT data status
    Serial.printf("%s MQTT SENT: %s/%s \n", myTZ.dateTime("[H:i:s.v]").c_str(), SPT_DATA_STATUS_TOPIC, sptDataStatus);

    sprintf(msg, "{\"valveState\": \"%d\", \"version\": \"%s\", \"valveInstalled\": \"%d\", \"pressureInstalled\": \"%d\", \"idlePublishInterval\": \"%d\", "
                 "\"minPublishInterval\": \"%d\", \"sensorReadInterval\": \"%d\", \"sptPressureDrop\": \"%.2f\", \"sptDemandWaterPercentDrop\": \"%.f\", \"sptDuration\": \"%d\"}\n\n",
            valveState, opParams.version, opParams.valveInstalled, opParams.pressureInstalled, opParams.idlePublishInterval, opParams.minPublishInterval,
            opParams.sensorReadInterval, opParams.sptPressureDrop, opParams.sptDemandWaterPercentDrop, opParams.sptDuration);
    mqttClient.publish(REPORT_TOPIC, msg, true);
    Serial.printf("%s MQTT SENT: %s/%s \n", myTZ.dateTime("[H:i:s.v]").c_str(), REPORT_TOPIC, msg);

    sprintf(msg, "%d", valveState);
    mqttClient.publish(VALVE_TOPIC, msg, true);
    Serial.printf("%s MQTT SENT: %s/%s \n", myTZ.dateTime("[H:i:s.v]").c_str(), VALVE_TOPIC, msg);

    // ... and resubscribe
    mqttClient.subscribe(RECV_COMMAND_TOPIC);
  }
  return mqttClient.connected();
}

//   ***********************
//   **  MQTT callback()  **
//   ***********************

void callback(char *topic, byte *payload, unsigned int length)
{
  // handle MQTT message arrival
  bool cmdValid = false;
  strncpy(msg, (char *)payload, length);
  msg[length] = (char)NULL; // terminate the string
  Serial.printf("\n%s MQTT RECVD: %s/%s \n", myTZ.dateTime("[H:i:s.v]").c_str(), topic, msg);

  // All commands must be prefixed with RECV_COMMAND_TOPIC
  // Valid commands:
  //   valveInstalled/<new_value>             - assigns a <new_value>, but does not save to NVM
  //   pressureInstalled/<new_value>          - assigns a <new_value>, but does not save to NVM
  //   idlePublishInterval/<new_value>        - assigns a <new_value>, but does not save to NVM
  //   minPublishInterval/<new_value>         - assigns a <new_value>, but does not save to NVM
  //   sensorReadInterval/<new_value>         - assigns a <new_value>, but does not save to NVM
  //   sptDuration/<new_value>                - assigns a <new_value> in minutes, but does not save to NVM
  //   valveState/<new_value>                 - 1 = OPEN, 0 = CLOSED, assigns and SAVES new value to NVM
  //   sptPressureDrop/<new_value>            - assings a <new_value> in PSI, but does not save to NVM
  //   sptDemandWaterPercentDrop/<new_value>  - assigns a <new_value> in PSI, but does not save to NVM
  //   sptStart       - starts the Static Pressure Test
  //   reportParams   - publishes parameters to REPORT_TOPIC replacing previous retained report on broker
  //   defaultParams  - sets parameters to default firmware values, but does not save to NVM
  //   readParams     - reads parameters from NVM storage, but does not save to NVM
  //   writeParams    - saves current parameters to NVM storage
  //   deleteParams   - deletes parameters stored in NVM storage to force creation at next boot
 
  //   reboot         - reboots device
  //   help           - sends list of valid commands

  if (strstr(topic, "idlePublishInterval")) // publish this often if not triggered by anything else
  {
    cmdValid = true;
    if (atoi(msg) > DEFAULT_MIN_PUBLISH_INTERVAL_MS)
    {
      Serial.printf("idlePublishInterval set to %s\n", msg);
      opParams.idlePublishInterval = atoi(msg);
    }
    else
      Serial.println("Invalid idlePublishInterval value");
  }
  if (strstr(topic, "minPublishInterval")) // don't publish more often than this even if triggered
  {
    cmdValid = true;
    if (atoi(msg) > DEFAULT_SENSOR_READ_INTERVAL_MS)
    {
      Serial.printf("minPublishInterval set to %s\n", msg);
      opParams.minPublishInterval = atoi(msg);
    }
    else
      Serial.println("Invalid minPublishInterval value");
  }
  if (strstr(topic, "sensorReadInterval")) // frequency of sensor read - independent of publish frequency
  {
    cmdValid = true;
    if (atoi(msg) > 3)
    {
      Serial.printf("sensorReadInterval set to %s\n", msg);
      opParams.sensorReadInterval = atoi(msg);
    }
    else
      Serial.println("Invalid sensorReadInterval value");
  }
  if (strstr(topic, "sptPressureDrop")) // publish if pressure changes this amount or more
  {
    cmdValid = true;
    if (atof(msg) > .1)
    {
      Serial.printf("sptPressureDrop set to %s\n", msg);
      opParams.sptPressureDrop = atof(msg);
    }
    else
      Serial.println("Invalid sptPressureDrop value");
  }
  if (strstr(topic, "sptDemandWaterPercentDrop")) // turn valve on during SPT and invalideate test if pressure drop greater than this amount
  {
    cmdValid = true;
    if (atof(msg) > 5)
    {
      Serial.printf("sptDemandWaterPercentDrop set to %s\n", msg);
      opParams.sptDemandWaterPercentDrop = atof(msg);
    }
    else
      Serial.println("Invalid sptDemandWaterPercentDrop value");
  }
  if (strstr(topic, "valveInstalled")) // valveInstalled = 1 if valve is installed, valveInstalled = 0 otherwise
  {
    cmdValid = true;
    if ((strcmp(msg, "0") == 0) || (strcmp(msg, "1") == 0))
    {
      Serial.printf("valveInstalled set to %s\n", msg);
      opParams.valveInstalled = atoi(msg);
    }
    else
      Serial.println("Invalid valveInstalled value");
  }
  if (strstr(topic, "pressureInstalled")) // pressureInstalled = 1 if valve is installed, pressureInstalled = 0 otherwise
  {
    cmdValid = true;
    if ((strcmp(msg, "0") == 0) || (strcmp(msg, "1") == 0))
    {
      Serial.printf("pressureInstalled set to %s\n", msg);
      opParams.pressureInstalled = atoi(msg);
    }
    else
      Serial.println("Invalid pressureInstalled value");
  }
  if (strstr(topic, "sptDuration")) // duration of Static Pressure Test in millisec
  {
    cmdValid = true;
    if (atoi(msg) >= 1)
    {
      Serial.printf("sptDuration set to %s\n", msg);
      opParams.sptDuration = atoi(msg);
    }
    else
      Serial.println("Invalid sptDuration value");
  }
  if (strstr(topic, "sptStart")) // start the Static Pressure Test
  {
    cmdValid = true;
    if ( (opParams.valveInstalled == 1) && (opParams.pressureInstalled == 1) && (valveState == OPEN_VALVE) )
    {
      strcpy(sptDataStatus, SPT_DATA_IN_PROCESS);
      sprintf(msg, "%s", sptDataStatus);
      mqttClient.publish(SPT_DATA_STATUS_TOPIC, msg, true);
      Serial.printf("%s  MQTT SENT: %s/%s \n", myTZ.dateTime("[H:i:s.v]").c_str(), SPT_DATA_STATUS_TOPIC, sptDataStatus);
      
      // zero SPT previous results to reset anything triggering on result values changing
      mqttClient.publish(SPT_RESULT_TOPIC, "0.00", true);  
      Serial.printf("%s  MQTT SENT: %s/%s \n", myTZ.dateTime("[H:i:s.v]").c_str(), SPT_RESULT_TOPIC, "0.00");
      
      valvePreSPT = valveState;
      valveState = CLOSE_VALVE;
      applyValveState(CLOSE_VALVE, false); // close the valve
      valveNow = millis();
      Serial.printf("Waiting %d msecs for pressure to settle\n", PRESSURE_SETTLING_DELAY_MS);
      while (millis() - valveNow < PRESSURE_SETTLING_DELAY_MS) // wait for pressure to settle
        yield();

      pre_spt_idlePublishInterval = opParams.idlePublishInterval;
      pre_spt_minPublishInterval = opParams.minPublishInterval;
      opParams.idlePublishInterval = 15000;  // temporarily report every 15 secs during SPT if idle
      opParams.minPublishInterval = SPT_MIN_PUBLISH_INTERVAL_MS;  // set to shorter interval during SPT
      sptBeginningPressure = medianPressure;
      Serial.printf("%s SPT Beginning Pressure = %.2f \n", myTZ.dateTime("[H:i:s.v]").c_str(), sptBeginningPressure);
      setEvent(sptEnd, now() + (opParams.sptDuration * 60)); // use ezTime event handler & set event time
    }
    else
      Serial.println("Invalid request - both valve and pressure sensor must be installed valve must be in open position for SPT");
  }
  if (strstr(topic, "valveState")) // set valve 0=closed 1=open
  {
    cmdValid = true;
    if ((strcmp(msg, "0") == 0) || (strcmp(msg, "1") == 0))
    {
      valveState = atoi(msg);
      applyValveState(valveState, true);
    }
    else
      Serial.println(F("Invalid valveState requested"));
  }
  if (strstr(topic, "reportParams")) // report opParams
  {
    cmdValid = true;
    sprintf(msg, "{\"valveState\": \"%d\", \"version\": \"%s\", \"valveInstalled\": \"%d\", \"pressureInstalled\": \"%d\", \"idlePublishInterval\": \"%d\", "
                 "\"minPublishInterval\": \"%d\", \"sensorReadInterval\": \"%d\", \"sptPressureDrop\": \"%.2f\", \"sptDemandWaterPercentDrop\": \"%.f\", \"sptDuration\": \"%d\"}\n\n",
            valveState, opParams.version, opParams.valveInstalled, opParams.pressureInstalled, opParams.idlePublishInterval, opParams.minPublishInterval,
            opParams.sensorReadInterval, opParams.sptPressureDrop, opParams.sptDemandWaterPercentDrop, opParams.sptDuration);
    mqttClient.publish(REPORT_TOPIC, msg, true);
    Serial.printf("%s reportParams > MQTT SENT: %s/%s \n", myTZ.dateTime("[H:i:s.v]").c_str(), REPORT_TOPIC, msg);
  }
  if (strstr(topic, "defaultParams")) // set params to firmware defaults without file write
  {
    cmdValid = true;
    strcpy(opParams.version, VERSION);
    opParams.valveInstalled = INITIAL_VALVE_INSTALLED_STATE;
    opParams.pressureInstalled = INITIAL_PRESSURE_SENSOR_INSTALLED_STATE;
    opParams.idlePublishInterval = DEFAULT_IDLE_PUBLISH_INTERVAL_MS;
    opParams.minPublishInterval = DEFAULT_MIN_PUBLISH_INTERVAL_MS;
    opParams.sensorReadInterval = DEFAULT_SENSOR_READ_INTERVAL_MS;
    opParams.sptPressureDrop = (float)DEFAULT_SPT_REPORT_PSI_DROP;
    opParams.sptDemandWaterPercentDrop = (float)DEFAULT_SPT_DEMAND_WATER_PERCENT_DROP;
    opParams.sptDuration = DEFAULT_SPT_TEST_DURATION_MINUTES;
    Serial.println(F("Parameters set to default firmware values\n"));
  }
  if (strstr(topic, "readParams")) // reload params from file without reboot or file write
  {
    cmdValid = true;
    paramFileObj = LittleFS.open(F(PARAMS_FILENAME), "r");
    if (paramFileObj.readBytes((char *)&opParams, sizeof(opParams)) > 0)
    {
      Serial.printf("Parameters loaded from file %s \n", PARAMS_FILENAME);
      Serial.printf("{\"version\": \"%s\", \"valveInstalled\": \"%d\", \"pressureInstalled\": \"%d\", \"idlePublishInterval\": \"%d\", "
                    "\"minPublishInterval\": \"%d\", \"sensorReadInterval\": \"%d\", \"sptPressureDrop\": \"%.2f\", \"sptDemandWaterPercentDrop\": \"%.f\", \"sptDuration\": \"%d\"}\n\n",
                    opParams.version, opParams.valveInstalled, opParams.pressureInstalled, opParams.idlePublishInterval, opParams.minPublishInterval,
                    opParams.sensorReadInterval, opParams.sptPressureDrop, opParams.sptDemandWaterPercentDrop, opParams.sptDuration);
    }
    else
      Serial.println(F("Unable to read parameters from file"));
    paramFileObj.close();
  }
  if (strstr(topic, "writeParams"))
  {
    cmdValid = true;
    paramFileObj = LittleFS.open(F(PARAMS_FILENAME), "r+");
    if (paramFileObj.write((uint8_t *)&opParams, sizeof(opParams)) > 0)
      Serial.println(F("Parameters file updated"));
    else
      Serial.println(F("Parameters file update error"));
    paramFileObj.close();
  }
  if (strstr(topic, "deleteParams"))
  {
    cmdValid = true;
    if (LittleFS.remove(F(PARAMS_FILENAME)))
      Serial.println(F("Params file deleted"));
    else
      Serial.println(F("Error deleting params file"));
  }
  if (strstr(topic, "reboot"))
  {
    cmdValid = true;
    Serial.println(F("MQTT reboot command received.  Rebooting..."));
    tempNow = millis();
    while ((millis() - tempNow) < 5000) ;
    mqttClient.disconnect();
    paramFileObj.close();
    valveFileObj.close();
    WiFi.disconnect();
    LittleFS.end();
    ESP.restart();
  }
  if (strstr(topic, "help"))
  {
    cmdValid = true;
    sprintf(msg, "{\"commands\" : \"valveInstalled, pressureInstalled, valveState, idlePublishInterval, minPublishInterval, sensorReadInterval, "
                 "sptPressureDrop, sptDemandWaterPercentDrop, sptDuration, sptStart, reportParams, defaultParams, readParams, writeParams, deleteParams, reboot, help\"}");
    mqttClient.publish(HELP_TOPIC, msg);
    Serial.printf("%s help > MQTT SENT: %s/%s \n", myTZ.dateTime("[H:i:s.v]").c_str(), HELP_TOPIC, msg);
  }
  if (!cmdValid)
  {
    Serial.println(F("Invalid command"));
  }
  msg[0] = (char)NULL; // clear msg
}

//   ***********************
//   **     setup()       **
//   ***********************

void setup()
{
  Serial.begin(115200);
  delay(500);
  Serial.printf("\n\n\nWater Main Controller %s\n\n", VERSION);

  if (DEBUG_SPT)
    Serial.println(F(">>>> DEBUG_SPT IS ENABLED!! <<<<"));
  
  // No SPT has been run yet, so old SPT results data is invalid
  strcpy(sptDataStatus, SPT_DATA_INVALID);

  // set GPIOs
  pinMode(PIN_VALVE_ON_INDICATOR, INPUT);
  pinMode(PIN_VALVE_OFF_INDICATOR, INPUT);
  pinMode(PIN_VALVE_ON, OUTPUT);
  pinMode(PIN_VALVE_OFF, OUTPUT);
  digitalWrite(PIN_VALVE_ON, LOW);
  digitalWrite(PIN_VALVE_OFF, LOW);

  Wire.begin(PIN_SDA, PIN_SCL);

  WiFi.hostname(DEVICE_NAME);
  setup_wifi();

  ArduinoOTA.setHostname(DEVICE_NAME);
  setup_OTA();

  if (!myTZ.setCache(TIMEZONE_EEPROM_OFFSET)) // using EEPROM just because it's built into ezTime
    myTZ.setLocation(F(MY_TIMEZONE));         // get TZ info from EEPROM
  waitForSync();                              // NTP
  myTZ.setLocation(F(MY_TIMEZONE));
  strcpy(lastBoot, myTZ.dateTime(RFC3339).c_str());
  Serial.printf("\nLocal time: %s\n\n", lastBoot);

  mqttClient.setBufferSize(MSG_BUFFER_SIZE);
  mqttClient.setServer(MQTT_SERVER, 1883);
  mqttClient.setCallback(callback);
  lastReconnectAttempt = 0;

  Serial.print(F("Initializing LittleFS..."));
  if (LittleFS.begin())
    Serial.println(F("done\n"));
  else
    Serial.println(F("FAIL\n"));

  // Show FS directory
  Dir dir = LittleFS.openDir("/");
  Serial.println("--------- FS Directory ----------");
  while (dir.next())
  {
    Serial.print(dir.fileName());
    if (dir.fileSize())
    {
      File f = dir.openFile("r");
      Serial.printf(" - %d bytes\n", f.size());
    }
  }
  Serial.println("---------------------------------\n");

  if (LittleFS.exists(F(PARAMS_FILENAME))) // if file exists
  {
    paramFileObj = LittleFS.open(F(PARAMS_FILENAME), "r+");
    if (paramFileObj.readBytes((char *)&opParams, sizeof(opParams)) == sizeof(opParams))
    {
      Serial.println(F("Parameters loaded from file:"));
      Serial.printf("{\"version\": \"%s\", \"valveInstalled\": \"%d\", \"pressureInstalled\": \"%d\", \"idlePublishInterval\": \"%d\", "
                    "\"minPublishInterval\": \"%d\", \"sensorReadInterval\": \"%d\", \"sptPressureDrop\": \"%.2f\", \"sptDemandWaterPercentDrop\": \"%.f\", \"sptDuration\": \"%d\"}\n\n",
                    opParams.version, opParams.valveInstalled, opParams.pressureInstalled, opParams.idlePublishInterval, opParams.minPublishInterval,
                    opParams.sensorReadInterval, opParams.sptPressureDrop, opParams.sptDemandWaterPercentDrop, opParams.sptDuration);
      if (opParams.valveInstalled == 1)
        Serial.println(F("Valve configuration: INSTALLED\n"));
      else
        Serial.println(F("Valve configuration: NOT INSTALLED\n"));

      if (opParams.pressureInstalled == 1)
        Serial.println(F("Pressure sensor configuration: INSTALLED\n"));
      else
        Serial.println(F("pressure sensor configuration: NOT INSTALLED\n"));
    }
    else
    {
      Serial.println(F("Parameters file read error.  Using default values."));
      strcpy(opParams.version, VERSION);
      opParams.valveInstalled = INITIAL_VALVE_INSTALLED_STATE;
      opParams.pressureInstalled = INITIAL_PRESSURE_SENSOR_INSTALLED_STATE;
      opParams.idlePublishInterval = DEFAULT_IDLE_PUBLISH_INTERVAL_MS;
      opParams.minPublishInterval = DEFAULT_MIN_PUBLISH_INTERVAL_MS;
      opParams.sensorReadInterval = DEFAULT_SENSOR_READ_INTERVAL_MS;
      opParams.sptPressureDrop = (float)DEFAULT_SPT_REPORT_PSI_DROP;
      opParams.sptDemandWaterPercentDrop = (float)DEFAULT_SPT_DEMAND_WATER_PERCENT_DROP;
      opParams.sptDuration = DEFAULT_SPT_TEST_DURATION_MINUTES;
      if (paramFileObj.write((uint8_t *)&opParams, sizeof(opParams)) > 0)
        Serial.printf("Parameters file re-created: %s, %d bytes\n", paramFileObj.name(), paramFileObj.size());
      else
        Serial.println(F("Parameters file re-creation error"));
    }
    paramFileObj.close();
  }
  else
  { // fill it with default values
    Serial.println(F("No parameters file detected. Using default values."));
    strcpy(opParams.version, VERSION);
    opParams.valveInstalled = INITIAL_VALVE_INSTALLED_STATE;
    opParams.pressureInstalled = INITIAL_PRESSURE_SENSOR_INSTALLED_STATE;
    opParams.idlePublishInterval = DEFAULT_IDLE_PUBLISH_INTERVAL_MS;
    opParams.minPublishInterval = DEFAULT_MIN_PUBLISH_INTERVAL_MS;
    opParams.sensorReadInterval = DEFAULT_SENSOR_READ_INTERVAL_MS;
    opParams.sptPressureDrop = (float)DEFAULT_SPT_REPORT_PSI_DROP;
    opParams.sptDemandWaterPercentDrop = (float)DEFAULT_SPT_DEMAND_WATER_PERCENT_DROP;
    opParams.sptDuration = DEFAULT_SPT_TEST_DURATION_MINUTES;

    paramFileObj = LittleFS.open(F(PARAMS_FILENAME), "w+");
    if (paramFileObj.write((uint8_t *)&opParams, sizeof(opParams)) > 0)
      Serial.printf("Parameters file created: %s, %d bytes\n", paramFileObj.name(), paramFileObj.size());
    else
      Serial.println(F("Parameters file creation error"));
    paramFileObj.close();
  }
}

//   ***********************
//   **     loop()        **
//   ***********************

void loop()
{

  ArduinoOTA.handle();
  events(); // exececute ezTime events i.e. Static Pressure Test

  if (!mqttClient.connected())
  {
    mqttNow = millis();
    if (mqttNow - lastReconnectAttempt > 1000)
    {
      Serial.printf("[%s] Waiting for MQTT...\n", myTZ.dateTime(RFC3339).c_str());
      lastReconnectAttempt = mqttNow;
      // Attempt to reconnect
      if (reconnect())
      {
        lastReconnectAttempt = 0;
      }
    }
  }
  else
  {
    // Client connected
    mqttClient.loop();
  }

  // Sync valve state
  if ( (opParams.valveInstalled == 1) && (!DEBUG_SPT) )
  {
    // Periodically check to sync software valveState with actual indicator inputs in case manual valve switch was used
    //  - this polling method used because manual override may result in half on/off state for an unknown amount of time
    lastValveSyncNow = millis();
    if ((unsigned long)(lastValveSyncNow - lastValveSync) > (unsigned long)VALVE_SYNC_INTERVAL_MS)
    {
      if ((digitalRead(PIN_VALVE_ON_INDICATOR) == LOW) && (digitalRead(PIN_VALVE_OFF_INDICATOR) == LOW)) // valve left half open/closed
      {
        Serial.println(F("Actual valve state cannot be determined.  Setting valve to defined VALVE_ERROR_DEFAULT"));
        mqttClient.publish(LAST_VALVE_STATE_UNK_TOPIC, myTZ.dateTime(RFC3339).c_str(), true);
        Serial.printf("%s MQTT SENT: %s/%s \n", myTZ.dateTime("[H:i:s.v]").c_str(), LAST_VALVE_STATE_UNK_TOPIC, myTZ.dateTime(RFC3339).c_str());
        valveState = VALVE_ERROR_DEFAULT;
        applyValveState(VALVE_ERROR_DEFAULT, false); // this can be a loop if valve is half open/closed, so do not write to flash
        Serial.println(F("To protect flash memory, valveState not saved"));
      }
      else
      {
        if ((digitalRead(PIN_VALVE_ON_INDICATOR) == HIGH) && (valveState != 1))
        {
          Serial.println(F("valveState CONFLICT DETECTED - syncing to actual: valveState=1"));
          valveState = 1;
          applyValveState(valveState, true);
        }
        if ((digitalRead(PIN_VALVE_OFF_INDICATOR) == HIGH) && (valveState != 0))
        {
          Serial.println(F("valveState CONFLICT DETECTED - syncing to actual: valveState=0"));
          valveState = 0;
          applyValveState(valveState, true);
        }
      }
      lastValveSync = millis();
    }
  }

  // Sanity check to prevent MQTT flooding - reset ALL to defaults if parameters seem corrupted
  if ((opParams.idlePublishInterval < DEFAULT_MIN_PUBLISH_INTERVAL_MS) || (opParams.minPublishInterval < DEFAULT_SENSOR_READ_INTERVAL_MS) 
       || (opParams.sensorReadInterval < 3) || (opParams.sptPressureDrop <= (float).2) || (opParams.sptDuration < 1))
  {
    Serial.printf("Parameters loaded from file %s \n", PARAMS_FILENAME);
    Serial.printf("{\"version\": \"%s\", \"valveInstalled\": \"%d\", \"pressureInstalled\": \"%d\", \"idlePublishInterval\": \"%d\", "
                  "\"minPublishInterval\": \"%d\", \"sensorReadInterval\": \"%d\", \"sptPressureDrop\": \"%.2f\", \"sptDemandWaterPercentDrop\": \"%.f\", \"sptDuration\": \"%d\"}\n\n",
                  opParams.version, opParams.valveInstalled, opParams.pressureInstalled, opParams.idlePublishInterval, opParams.minPublishInterval,
                  opParams.sensorReadInterval, opParams.sptPressureDrop, opParams.sptDemandWaterPercentDrop, opParams.sptDuration);
    strcpy(opParams.version, VERSION);
    opParams.valveInstalled = INITIAL_VALVE_INSTALLED_STATE;
    opParams.pressureInstalled = INITIAL_PRESSURE_SENSOR_INSTALLED_STATE;
    opParams.idlePublishInterval = DEFAULT_IDLE_PUBLISH_INTERVAL_MS;
    opParams.minPublishInterval = DEFAULT_MIN_PUBLISH_INTERVAL_MS;
    opParams.sensorReadInterval = DEFAULT_SENSOR_READ_INTERVAL_MS;
    opParams.sptPressureDrop = (float)DEFAULT_SPT_REPORT_PSI_DROP;
    opParams.sptDemandWaterPercentDrop = (float)DEFAULT_SPT_DEMAND_WATER_PERCENT_DROP;
    opParams.sptDuration = DEFAULT_SPT_TEST_DURATION_MINUTES;
    Serial.println(F("PARAMETER SANITY CHECK FAILED.  All parameters reset to defaults. "));
    paramFileObj = LittleFS.open(F(PARAMS_FILENAME), "w+");
    if (paramFileObj.write((uint8_t *)&opParams, sizeof(opParams)) > 0)
      Serial.printf("New parameters file created: %s, %d bytes\n", paramFileObj.name(), paramFileObj.size());
    else
      Serial.println(F("Parameters file creation error"));
    paramFileObj.close();
  }


  // Read sensor
  if (opParams.pressureInstalled == 1)
  {
    sensorStatus = 0xFF; // set to non-zero for initial while() test
    sensorReadNow = millis();
    if ((unsigned long)(sensorReadNow - lastRead) > opParams.sensorReadInterval)
    {
      lastRead = millis();
      while (sensorStatus != 0) // continue reading until valid
      {
        Wire.requestFrom(I2C_ADDR, 4); // request 4 bytes  - if optional 3rd argument false = don't share i2c bus
        int n = Wire.available();
        if (n == 4)
        {
          sensorStatus = 0;  // set to zero to exit while loop when read is successful
          uint16_t rawP; // pressure data from sensor
          uint16_t rawT; // temperature data from sensor

          rawP = (uint16_t)Wire.read(); // upper 8 bits
          rawP <<= 8;
          rawP |= (uint16_t)Wire.read(); // lower 8 bits
          rawT = (uint16_t)Wire.read();  // upper 8 bits
          rawT <<= 8;
          rawT |= (uint16_t)Wire.read(); // lower 8 bits

          sensorStatus = rawP >> 14; // The status is 0, 1, 2 or 3
          rawP &= 0x3FFF;            // keep 14 bits, remove status bits

          rawT >>= 5; // the lowest 5 bits are not used

          psiTminus0 = ((rawP - 1000.0) / (15000.0 - 1000.0)) * MAX_PRESSURE;
          temperature = ((rawT - 512.0) / (1075.0 - 512.0)) * 55.0;
        }
        else
        {
          lastPressErrReportNow = millis();
          if ((unsigned long)(lastPressErrReportNow - lastPressErrReport) > (unsigned long)PRESSURE_SENSOR_FAULT_PUB_INTERVAL_MS)
          {
            Serial.println(F("Error reading pressure sensor"));
            mqttClient.publish(PRESSURE_SENSOR_FAULT_TOPIC, myTZ.dateTime(RFC3339).c_str(), true);
            Serial.printf("%s MQTT SENT: %s/%s \n", myTZ.dateTime("[H:i:s.v]").c_str(), PRESSURE_SENSOR_FAULT_TOPIC, myTZ.dateTime(RFC3339).c_str());
            lastPressErrReport = millis();
          }
          while ((millis() - lastPressErrReportNow) < opParams.sensorReadInterval); // prevent cycling too fast
          break;
        }
      }
    }

    lastPublishNow = millis();
    if (  ( ((unsigned long)(lastPublishNow - lastPublish) > opParams.idlePublishInterval) ||
        ((fabs(psiTminus1 - psiTminus0) > opParams.sptPressureDrop) && (lastPublishNow - lastPublish >= opParams.minPublishInterval)) ) &&
        mqttClient.connected()  )
    {
      // use MEDIAN of last three readings to filter glitches - psiTminus0 is latest reading, psiTminus2 is oldest
      // MEDIAN is the middle of the last three readings - not the average
      if ((psiTminus1 != 0) && (psiTminus2 != 0))
      {
        if ((psiTminus1 > psiTminus2 && psiTminus2 > psiTminus0) || (psiTminus0 > psiTminus2 && psiTminus2 > psiTminus1))
          medianPressure = psiTminus2;
        if ((psiTminus2 > psiTminus1 && psiTminus1 > psiTminus0) || (psiTminus0 > psiTminus1 && psiTminus1 > psiTminus2))
          medianPressure = psiTminus1;
        if ((psiTminus2 > psiTminus0 && psiTminus0 > psiTminus1) || (psiTminus1 > psiTminus0 && psiTminus0 > psiTminus2))
          medianPressure = psiTminus0;
      }
      else
      {
        medianPressure = psiTminus0;
      }
      sprintf(msg, "%.2f", medianPressure);
      mqttClient.publish(PRESSURE_TOPIC, msg);
      Serial.printf("\n%s MQTT SENT: %s/%s \n", myTZ.dateTime("[H:i:s.v]").c_str(), PRESSURE_TOPIC, msg);
      if (PREFER_FAHRENHEIT == 1)
        temperature = (1.8 * temperature + 32);
      sprintf(msg, "%.2f", temperature);
      mqttClient.publish(TEMPERATURE_TOPIC, msg);
      Serial.printf("%s MQTT SENT: %s/%s \n", myTZ.dateTime("[H:i:s.v]").c_str(), TEMPERATURE_TOPIC, msg);
      lastPublish = millis();
      psiTminus2 = psiTminus1; // rotate queue
      psiTminus1 = psiTminus0;
    }

    // automatically open valve if demand pressure drop is met during SPT
    if (strcmp(sptDataStatus, SPT_DATA_IN_PROCESS) == 0)
    {
      if (fabs(sptBeginningPressure - medianPressure) > (sptBeginningPressure * DEFAULT_SPT_DEMAND_WATER_PERCENT_DROP/100))
      {       
        deleteEvent(sptEnd);  // delete event from ezTime event handler
        
        // set status to ABORTED
        strcpy(sptDataStatus, SPT_DATA_ABORTED);
        sprintf(msg, "%s", sptDataStatus);
        mqttClient.publish(SPT_DATA_STATUS_TOPIC, msg, true);
        Serial.printf("%s  MQTT SENT: %s/%s \n", myTZ.dateTime("[H:i:s.v]").c_str(), SPT_DATA_STATUS_TOPIC, msg);
        
        // report consecutive aborts attribute
        sptConsecAborts++;
        sprintf(msg, "{\"consec_aborts\": \"%d\"}", sptConsecAborts);
        mqttClient.publish(SPT_DATA_STATUS_TOPIC"/attributes", msg, true);  
        Serial.printf("%s  MQTT SENT: %s/%s \n", myTZ.dateTime("[H:i:s.v]").c_str(), SPT_DATA_STATUS_TOPIC"/attributes", msg);

        // Restore previous states
        opParams.idlePublishInterval = pre_spt_idlePublishInterval;  // restore idlePublishInterval
        opParams.minPublishInterval = pre_spt_minPublishInterval;    // restore minPublishInterval
        valveState = valvePreSPT;
        applyValveState(valvePreSPT, false);                         // restore the valveState to state before test
        Serial.println(F("SPT event end: Aborted due to water demand"));
      }
    }
  }
}
