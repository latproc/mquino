mquino
======

A generic MQTT I/O sketch 

This program provides a command channel on the serial port
to configure the Arduino ports and provides runtime configuration
by virtue of the fact the Arduino subscribes to config channels
on the host.

using tools in the mosquitto package, you can say:

  mosquitto_pub -t MyMega/config/dig/10 -m IN

to cause the Arduino to publish changes on digital input 10

  mosquitto_pub -t MyMega/config/ana/0 -m AIN

to cause the Arduino to publish changes on analogue input 0

  mosquitto_pub -t MyMega/config/dig/5 -m OUT

to cause the Arduino to subscribe to a channel and turn pin 5 
   on or off depending on what it sees there.

Given the last configuration command, above, you can use:

  mosquitto_pub -t MyMega/dig/5 -m ON

to turn the output on.

Martin Leadbeater
March 2013
