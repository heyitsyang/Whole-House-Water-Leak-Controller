# Water Main Controller
This project started as just a pressure sensor for my home plumbing and expanded to include a electric valve.  The valve was added after I realized I could check for small leaks by performing a static pressure test by turning off the water coming into the house for a short time and watching for a pressure drop when water was not being used and when the hot water heater was not heating.

MQTT is used as the reporting and command interface to the sensor program.  See the code for the MQTT topics and commands.

## Pressure sensor
The pressure sensor used is a M3200 series I2C interfaced model from TE Connectivity Industrial Pressure Sensors.  The specific model I used is M32JM-000105-100PG purchased from Mouser Electronics.  If you use this model be aware that external 4.7K pull up resistors are required for the SDA and SCL wires.

### Pressure Sensor Default Programming
Programming behavior can be changed by varying the *#define* statements in the program.  Here is the default behavior:
- Sensor is read every 500ms
- Under quiescent conditions, pressure is published every 5 minutes
- If there is a pressure change of more than 0.3 PSI, the current pressure is published every five seconds
- If the pressure sensor cannot be read a fault will be published every 5 minutes

## Motorized Valve
The motorized valve is completely optional.  The code will run without the valve provided *#define DEFAULT_VALVE_INSTALLED_STATE* is set correctly.

The motorized valve used is a U.S. Solid model JFMSV00014 3/4" diameter Motorized Ball Valve (available on Amazon).  Be sure you order the diameter approriate for your plumbing.  This model has five wires - two to control the valve (via polarity reversal) and three to connected to open/closed indicator switches.  There are other models from U.S. Solid that look the same but have different wire configurations.  The five-wire model is required if you wish to use this code without modification.

## Software
### MQTT
All reporting and interaction with The Water Main Montior is accomplished via MQTT.  If you've never heard of MQTT, there are many good tutorials on the web,  It is best to have a firm grasp on MQTT before beginning this project.

Although there is robust logging available from the serial port, the primary interface for interaction and configuration is through MQTT reports and commands.  The documentation for commands are in the code and are not purposely not duplicated here to avoid having to maintain documentation in multiple places.

Using a MQTT tool such as MQTT Explorer is strongly recommended to fully understand, configure, test, and debug the project.

### Static Pressure Test
The optional valve is required to use this feature.  A Static Pressure Test is used to check for small leaks by turning off the water coming into the house for a short time and watching for a pressure drop when water is not being used and when the hot water heater is not heating.  Sensing hot water heater activity requires an additional sensor and is outside the scope of this code.

A Static Pressure Test is initiated by sending the sptStartTest MQTT command (read code comments for all commands) to the controller.  Once the command is received, the test begins immediately.  

Results are published via MQTT at the end of the test.  It is up to an external process to determine if the test passed, failed, or  invalid due to water use or hot water heater activity.  In my case the external process is done in Home Assistant.
- Default SPT duration is 10 minutes
- Valve is closed at the start and opened at the end of the test
- Beginning pressure is published at start of test
- Ending pressure is published at end of test
- Difference between beginning pressure and ending pressure is published at end of test 

### Home Assistant
## Electronics





