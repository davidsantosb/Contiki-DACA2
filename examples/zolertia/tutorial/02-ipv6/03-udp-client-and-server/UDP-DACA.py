#! /usr/bin/env python

#------------------------------------------------------------#
# UDP example to forward data from a local IPv6 DODAG
# Antonio Lignan <alinan@zolertia.com>
#------------------------------------------------------------#
import sys
import json
import datetime
from socket import*
from socket import error
from time import sleep
import struct
from ctypes import *
import paho.mqtt.client as mqtt
import csv
#------------------------------------------------------------#
ID_STRING      = "V0.1"
#------------------------------------------------------------#
PORT              = 5678
CMD_PORT          = 8765
BUFSIZE           = 1024
#------------------------------------------------------------#
ENABLE_MQTT       = 1
ENABLE_LOG        = 1
#------------------------------------------------------------#
DEBUG_PRINT_JSON  = 1
#------------------------------------------------------------#
# If using a client based on the Z1 mote, then enable by equal to 1, else if
# using the RE-Mote equal to 0
EXAMPLE_WITH_Z1   = 0
#------------------------------------------------------------#
MQTT_URL          = "iot.eclipse.org"
MQTT_PORT         = 1883
MQTT_KEEPALIVE    = 60
MQTT_URL_PUB      = "DACA/info/"
MQTT_URL_TOPIC    = "/cmd"
#------------------------------------------------------------#
# Message structure
#------------------------------------------------------------#
if EXAMPLE_WITH_Z1:
  var1 = "temperature"
  var2 = "x_axis"
  var3 = "y_axis"
  var4 = "z_axis"
else:
  var1 = "core_temp"
  var2 = "ADC1"
  var3 = "ADC2"
  var4 = "ADC3"

class SENSOR(BigEndianStructure):
    _pack_   = 1
    _fields_ = [
                 ("id",                         c_uint8),
                 ("id2",                         c_uint8),
                 ("counter",                    c_uint16),
                 (var1,                         c_uint16),
                 (var2,                         c_uint16),
                 (var3,                         c_uint16),
                 (var4,                         c_uint16),
                 ("battery",                    c_uint16)
               ]
    def __new__(self, socket_buffer):
        return self.from_buffer_copy(socket_buffer)

    def __init__(self, socket_buffer):
        pass
fieldnames=["IP","id","id2","counter",var1,var2,var3,var4,"battery"]
#------------------------------------------------------------#
# Helper functions
#------------------------------------------------------------#
def print_recv_data(addr,msg):

  print "***"
  #Create an array that will be added to one of the rows of the csv file
  value_list=[]
  #Add the IP of the sender first to the array
  value_list.append(addr)

  #This for reads the whole msg received and stores it one by one according to the class SENSOR
  #and adds it to the array
  for f_name, f_type in msg._fields_:
    #some prints to check what we are going to write
    print "{0}:{1} ".format(f_name, getattr(msg, f_name)),
    value_list.append(getattr(msg,f_name))
  print
  print "***"
  with open('Node_info.csv', 'a') as csvfile:
    writer = csv.writer(csvfile, delimiter=',',
                            quotechar='|', quoting=csv.QUOTE_MINIMAL)
    writer.writerow(value_list)
# -----------------------------------------------------------#
def publish_recv_data(data, pubid, conn, addr):
  try:
    res, mid = conn.publish(MQTT_URL_PUB + str(pubid), payload=data, qos=1)
    print "MQTT: Publishing to {0}... " + "{1} ({2})".format(mid, res, str(pubid))
  except Exception as error:
    print error
  
# -----------------------------------------------------------#
def jsonify(keyval, val):
  return json.dumps(dict(value=val, key=keyval))
# -----------------------------------------------------------#
def jsonify_recv_data(msg):
  sensordata = '{"values":['
  for f_name, f_type in msg._fields_:
    sensordata += jsonify(f_name, getattr(msg, f_name)) + ","
  sensordata = sensordata[:-1]
  sensordata += ']}'
  
  # Paho MQTT client doesn't support sending JSON objects
  json_parsed = json.loads(sensordata)
  if DEBUG_PRINT_JSON:
    print json.dumps(json_parsed, indent=2)

  return sensordata
# -----------------------------------------------------------#
def send_udp_cmd(addr):
  client = socket(AF_INET6, SOCK_DGRAM)
  print "Sending reply to " + addr

  try:
    client.sendto("Hello from the server", (addr, CMD_PORT))
  except Exception as error:
    print error

  client.close()
# -----------------------------------------------------------#
# MQTT related functions
# -----------------------------------------------------------#
def on_connect(client, userdata, rc):
  print("MQTT: Connected ({0}) ").format(str(rc))
  client.subscribe(MQTT_URL_PUB + MQTT_URL_TOPIC)
#------------------------------------------------------------#
def on_message(client, userdata, msg):
  print("MQTT: RX: " + msg.topic + " : " + str(msg.payload))
#------------------------------------------------------------#
def on_publish(client, packet, mid):
  print("MQTT: Published {0}").format(mid)
#------------------------------------------------------------#
# UDP6 and MQTT client session
#------------------------------------------------------------#
def start_client():
  now = datetime.datetime.now()
  print "UDP6-MQTT server side application "  + ID_STRING
  print "Started " + str(now)
  try:
    s = socket(AF_INET6, SOCK_DGRAM)
    s.setsockopt(SOL_SOCKET, SO_REUSEADDR, 1)

    # Replace address below with "aaaa::1" if tunslip6 has created a tunnel
    # interface with this address
    s.bind(('', PORT))

  except Exception:
    print "ERROR: Server Port Binding Failed"
    return
  print 'UDP6-MQTT server ready: %s'% PORT
  print "msg structure size: ", sizeof(SENSOR)
  print

  if ENABLE_MQTT:
    # Initialize MQTT connection
    try:
      client = mqtt.Client()
    except Exception as error:
      print error
      raise

    client.on_connect = on_connect
    client.on_message = on_message
    client.on_publish = on_publish

    try:
      client.connect(MQTT_URL, MQTT_PORT, MQTT_KEEPALIVE)
    except Exception as error:
      print error
      raise

    # Start the MQTT thread and handle reconnections, also ensures the callbacks
    # being triggered
    client.loop_start()

  with open('Node_info.csv', 'w') as csvfile:
    writer = csv.writer(csvfile, delimiter=',',
                            quotechar='|', quoting=csv.QUOTE_MINIMAL)
    writer.writerow(fieldnames)
  while True:
    data, addr = s.recvfrom(BUFSIZE)
    now = datetime.datetime.now()
    print str(now)[:19] + " -> " + str(addr[0]) + ":" + str(addr[1]) + " " + str(len(data))

    #copy data on their byte form and print it
    #datacp=" ".join("{:02x}".format(ord(c)) for c in data)
    #print datacp

    msg_recv = SENSOR(data)
    if ENABLE_LOG:
      print_recv_data(addr[0],msg_recv)
    sensordata = jsonify_recv_data(msg_recv)

    if ENABLE_MQTT:
      publish_recv_data(sensordata, msg_recv.id, client, addr[0])

    send_udp_cmd(addr[0])

  client.loop_stop()

#------------------------------------------------------------#
# MAIN APP
#------------------------------------------------------------#
if __name__ == "__main__":
  start_client()

