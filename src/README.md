# **Water Main Controller**
This project started as just a pressure sensor for my home plumbing and expanded to include a electric valve.  The valve was added after I realized I could check for small leaks by performing a static pressure test by turning off the water coming into the house for a short time and watching for a pressure drop when water was not being used and when the hot water heater was not heating.

Of course, the valve can be used to turn off water if water leak sensors external to this project detect a leak as well.  Using external water leak sensors require a home automation system such as Home Assistant to be integrated with this project and the external sensors.

MQTT is used as the integration interface to this project.  See additional comments below.

## **Pressure sensor**
The pressure sensor used is a M3200 series I2C interfaced model from TE Connectivity Industrial Pressure Sensors.  The specific model I used is M32JM-000105-100PG purchased from Mouser Electronics.  If you use this model be aware that external 4.7K pull up resistors are required for the SDA and SCL wires.

### **Pressure Sensor Default Programming**
Programming behavior can be changed by varying the *#define* statements in the program.  Here is the default behavior:
- Sensor is read every 500ms
- Under quiescent conditions, pressure is published every 5 minutes
- If there is a pressure change of more than 0.3 PSI, the current pressure is published every five seconds
- If the pressure sensor cannot be read a fault will be published every 5 minutes

## **Motorized Valve**
The motorized valve is completely optional.  The code will run without the valve provided *#define DEFAULT_VALVE_INSTALLED_STATE* is set correctly.

The motorized valve used is a U.S. Solid model JFMSV00014 3/4" diameter Motorized Ball Valve (available on Amazon).  Be sure you order the diameter approriate for your plumbing.  This model has five wires - two to control the valve (via polarity reversal) and three to connected to open/closed indicator switches.  There are other models from U.S. Solid that look the same but have different wire configurations.  The five-wire model is required if you wish to use this code without modification.

## **Software**
The Water Main Controller software is written using PlatformIO.  However, it can be compiled using Arduino IDE by simply copying the text in src/main.cpp to a Arduino sketch file (e.g watermain.ino) and compiling using the IDE.  You will need to add the libraries indicated in the code comments before compiling.

### **MQTT**
All reporting and interaction with The Water Main Montior is accomplished via MQTT.  If you've never heard of MQTT, there are many good tutorials on the web,  It is best to have a firm grasp on MQTT before beginning this project.

Although there is robust logging available from the serial port, the primary interface for interaction and configuration is through MQTT reports and commands.  The documentation for commands are in the code and are not purposely not duplicated here to avoid having to maintain documentation in multiple places.

Using a MQTT tool such as MQTT Explorer is strongly recommended to fully understand, configure, test, and debug the project.

### **Static Pressure Test**
The optional valve is required to use this feature.  A Static Pressure Test is used to check for small leaks by turning off the water coming into the house for a short time and watching for a pressure drop when water is not being used and when the hot water heater is not heating.  Sensing hot water heater activity requires an additional sensor and is outside the scope of this code.

A Static Pressure Test is initiated by sending the sptStartTest MQTT command (read code comments for all commands) to the controller.  Once the command is received, the test begins immediately.  

Results are published via MQTT at the end of the test.  It is up to an external process to determine if the test passed, failed, or  invalid due to water use or hot water heater activity.  In my case the external process is done in Home Assistant.
- Default SPT duration is 10 minutes
- Valve is closed at the start and restored to pre-test state at end of test
- Timestamp of start of test, beginning pressure, timestamp of end of test, ending pressure, and the difference is published at the end of the test


### **Home Assistant**
If you use Home Assistant, the following is the yaml code required for your configutation.yaml.  This should make the entities sensors.water_pressure, sensors.water_temperature, sensor.water_static_pressure_test, and switch.water_valve available for your use.

```
sensors:
  - platform: mqtt
    unique_id: water_pressure
    name: "Water Pressure"
    state_topic: "watermain/water_pressure"
    qos: 0
    device_class: "pressure"
    unit_of_measurement: "PSI"
    
  - platform: mqtt
    unique_id: water_temperature
    name: "Water Temperature"
    state_topic: "watermain/water_temperature"
    qos: 0
    device_class: "temperature"
    unit_of_measurement: "ºF"

  #
  # no need for the following if you don't have a valve installed
  #
  - platform: mqtt
    unique_id: water_static_pressure_test
    name: "Water Static Pressure Test"
    state_topic: "watermain/spt_result"
    json_attributes_topic: "watermain/spt_result/attributes"

  switch:
  - platform: mqtt
    name: "Water Valve"
    unique_id: water_valve
    state_topic: "watermain/zeroisclosed"
    command_topic: "watermain/cmd/valveState"
    payload_on: "1"
    payload_off: "0"
    qos: 2

  ```

## **Electronics**
This is the detailed list of components used.  If you plan to 3D print the enclosure, buying these specific parts will ensure fit inside the enclosure.

- 824-M32JM-000105-100  (Stocked at Mouser Electronics)
M32JM-000105-100PG
TE Connectivity Industrial Pressure Sensors
US HTS:8532290040 ECCN:EAR99 COO:CN
- Motorized Ball Valve model JFMSV00014- .75" Stainless Steel Electrical Ball Valve with Full Port, 9-24V DC and 5 Wire Setup, can be used with Indicator Lights, [Indicate Open or Closed Position] by U.S. Solid
https://amazon.com/dp/B06Y11B8VN/ref=cm_sw_em_r_mt_dp_zI.YFbWGFTD1B?_encoding=UTF8&th=1
- WeMos ESP8266 D1 Mini https://www.amazon.com/Makerfocus-NodeMcu-Development-ESP8266-Compatible/dp/B01N3P763C/ref=sr_1_26?dchild=1&keywords=Wemos+esp8266&qid=1609860600&sr=8-26
- mxuteuk 3pcs/pkg (only one is needed) Momentary Rocker Switch Toggle Power Button (ON)/Off/(ON) 6 Pin 250V/10A 125V/15A, Use for Car Auto Boat Household Appliances KCD2-223-JT  by mxuteuk
https://amazon.com/dp/B0885W19KL/ref=cm_sw_em_r_mt_dp_.A.YFbV7YQZZY?_encoding=UTF8&psc=1
- Electronics-Salon 2 DPDT Signal Relay Module Board, DC 5V Version, for Arduino Raspberry-Pi 8051 PIC.  by Electronics-Salon.  https://www.amazon.com/Electronics-Salon-Signal-Version-Arduino-Raspberry-Pi/dp/B00SKG6OM4/ref=sr_1_11?dchild=1&keywords=Electronics+Salon&qid=1609815721&sr=8-11
- RGB LED (only one needed) https://www.amazon.com/Diffused-Multicolor-Common-Cathode-Arduino/dp/B01FDD3B72/ref=sr_1_39?crid=2UPMVM1W8WIF2&dchild=1&keywords=single+rgb+led&qid=1609817012&sprefix=single+RGB+led%2Caps%2C176&sr=8-39
- LM2596 Buck Converter (only one needed) https://www.amazon.com/DZS-Elec-Adjustable-Electronic-Stabilizer/dp/B06XRN7NFQ/ref=sr_1_12_sspa?dchild=1&keywords=buck+converter+arduino+2596&qid=1609860095&sr=8-12-spons&psc=1&spLa=ZW5jcnlwdGVkUXVhbGlmaWVyPUEzUVM4MExIWE5HQkM5JmVuY3J5cHRlZElkPUEwMjc5Mjg0RlZYOFpLUlk3VEJUJmVuY3J5cHRlZEFkSWQ9QTA2Nzk2NzAxMkoxS1lZWUdYRzlHJndpZGdldE5hbWU9c3BfbXRmJmFjdGlvbj1jbGlja1JlZGlyZWN0JmRvTm90TG9nQ2xpY2s9dHJ1ZQ==
- 12VDC Power supply 
https://amazon.com/gp/product/B07HNV6SBJ/ref=ppx_yo_dt_b_asin_title_o01_s00?ie=UTF8&psc=1

The panel mount holes of the 3D printed enclosure accomodates the specific connectors listed below.  Note the aviation connectors are specified with different number of pins to prevent accidentally interchanging the pressure sensor and the valve control wires.  It is possible to use the 6-pin version for both wires if you don't confuse the two or select pin connections so no harm is done if interchanged.

- Female DC Power Jack Panel Mount (only one is needed) https://www.amazon.com/TOTOT-5-5mm-Female-Socket-Electrical/dp/B077YB75N3/ref=sr_1_10?dchild=1&keywords=power+jack+5.5mm&qid=1609815794&sr=8-10
- 4-pin Aviation Connector set (only one is needed) https://amazon.com/gp/product/B07GZFQDNS/ref=ppx_yo_dt_b_search_asin_title?ie=UTF8&psc=1
- 6-pin Aviation Connector set (only one is needed) https://smile.amazon.com/gp/product/B07L1Q69R5/ref=ppx_yo_dt_b_search_asin_title?ie=UTF8&psc=1

## 3D Printed Enclosure

3D Printing files and the Fusion360 source file are availble in the repository in the 3D Printed Enclosure directory.  I printed my enclosure using PETG to avoid shrinkage issues, but there is nothing special about PETG otherwise.  Both STL and Fusion360 files are provided.  If you don't have a 3D printer, there are many services available online that will print for a fee.

You will need to print the bottom, the lid, and one of the two slide-in mounts depending on desired final orientation.



