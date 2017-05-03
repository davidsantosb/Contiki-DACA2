/*
 * Copyright (c) 2016, Antonio Lignan - antonio.lignan@gmail.com
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the Institute nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE INSTITUTE AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE INSTITUTE OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * This file is part of the Contiki operating system.
 *
 */
/*---------------------------------------------------------------------------*/
#include "contiki.h"
#include "lib/random.h"
#include "net/rpl/rpl.h"
#include "net/ip/uip.h"
#include "dev/leds.h"
#include "net/ipv6/uip-icmp6.h"
#include "net/ipv6/sicslowpan.h"
#include "sys/etimer.h"
#include "dev/sys-ctrl.h"
#include "mqtt-client.h"
#include "mqtt-sensors.h"
#include "thingsio.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>
/*---------------------------------------------------------------------------*/
#define DEBUG 1
#if DEBUG
#define PRINTF(...) printf(__VA_ARGS__)
#else
#define PRINTF(...)
#endif
/*---------------------------------------------------------------------------*/
#define SENSORS_NAME_EXPAND(x, y) x##y
#define SENSORS_NAME(x, y) SENSORS_NAME_EXPAND(x, y)
/*---------------------------------------------------------------------------*/
/* Payload length of ICMPv6 echo requests used to measure RSSI with def rt */
#define ECHO_REQ_PAYLOAD_LEN   20
/*---------------------------------------------------------------------------*/
#define APP_BUFFER_SIZE 512
static char *buf_ptr;
static char app_buffer[APP_BUFFER_SIZE];
/*---------------------------------------------------------------------------*/
/* Topic placeholders */
static char data_topic[CONFIG_PUB_TOPIC_LEN];
static char cmd_topic[CONFIG_SUB_CMD_TOPIC_LEN];
/*---------------------------------------------------------------------------*/
PROCESS(thingsio_process, "The Things.io MQTT process");
/*---------------------------------------------------------------------------*/
/* Include there the sensors processes to include */
PROCESS_NAME(SENSORS_NAME(MQTT_SENSORS, _sensors_process));
/*---------------------------------------------------------------------------*/
static struct etimer alarm_expired;
/*---------------------------------------------------------------------------*/
static uint16_t seq_nr_value;
/*---------------------------------------------------------------------------*/
/* Parent RSSI functionality */
static struct uip_icmp6_echo_reply_notification echo_reply_notification;
static int def_rt_rssi = 0;
/*---------------------------------------------------------------------------*/
/* Converts the IPv6 address to string */
static int
ipaddr_sprintf(char *buf, uint8_t buf_len, const uip_ipaddr_t *addr)
{
  uint16_t a;
  uint8_t len = 0;
  int i, f;
  for(i = 0, f = 0; i < sizeof(uip_ipaddr_t); i += 2) {
    a = (addr->u8[i] << 8) + addr->u8[i + 1];
    if(a == 0 && f >= 0) {
      if(f++ == 0) {
        len += snprintf(&buf[len], buf_len - len, "::");
      }
    } else {
      if(f > 0) {
        f = -1;
      } else if(i > 0) {
        len += snprintf(&buf[len], buf_len - len, ":");
      }
      len += snprintf(&buf[len], buf_len - len, "%x", a);
    }
  }

  return len;
}
/*---------------------------------------------------------------------------*/
/* Handles the ping response and updates the RSSI value */
static void
echo_reply_handler(uip_ipaddr_t *source, uint8_t ttl, uint8_t *data,
                   uint16_t datalen)
{
  if(uip_ip6addr_cmp(source, uip_ds6_defrt_choose())) {
    def_rt_rssi = sicslowpan_get_last_rssi();
  }
}
/*---------------------------------------------------------------------------*/
static void
ping_parent(void)
{
  if(uip_ds6_get_global(ADDR_PREFERRED) == NULL) {
    PRINTF("Things.io: Parent not available\n");
    return;
  }

  uip_icmp6_send(uip_ds6_defrt_choose(), ICMP6_ECHO_REQUEST, 0,
                 ECHO_REQ_PAYLOAD_LEN);
}
/*---------------------------------------------------------------------------*/
void
activate_sensors(uint8_t state)
{
  if(state) {
    process_start(&SENSORS_NAME(MQTT_SENSORS, _sensors_process), NULL);
  } else {
    process_exit(&SENSORS_NAME(MQTT_SENSORS, _sensors_process));
  }
}
/*---------------------------------------------------------------------------*/
static int
add_pub_topic(uint16_t length, char *meaning, char *value,
              uint8_t first, uint8_t more)
{
  int len = 0;
  int pos = 0;

  if((buf_ptr == NULL) || (length <= 0)){
    PRINTF("Things.io: null buffer or lenght less than zero\n");
    return -1;
  }

  if(first) {
    len = snprintf(buf_ptr, length, "%s", "{\"values\":[");
    pos = len;
    buf_ptr += len;
  }

  len = snprintf(buf_ptr, (length - pos),
                 "{\"key\":\"%s\",\"value\":\"%s\"}",
                 meaning, value);
 
  if(len < 0 || pos >= length) {
    PRINTF("Things.io: Buffer too short. Have %d, need %d + \\0\n", length, len);
    return -1;
  }

  pos += len;
  buf_ptr += len;

  if(more) {
    len = snprintf(buf_ptr, (length - pos), "%s", ",");
  } else {
    len = snprintf(buf_ptr, (length - pos), "%s", "]}");
  }

  pos += len;
  buf_ptr += len;

  return pos;
}
/*---------------------------------------------------------------------------*/
static void
publish_alarm(sensor_val_t *sensor)
{
  uint16_t aux_int, aux_res;

  if(etimer_expired(&alarm_expired)) {

    /* Clear buffer */
    memset(app_buffer, 0, APP_BUFFER_SIZE);

    PRINTF("Things.io: Alarm! %s --> %u\n", sensor->alarm_name, sensor->value);
    aux_int = sensor->value;
    aux_res = sensor->value;

    if(sensor->pres > 0) {
      aux_int /= sensor->pres;
      aux_res %= sensor->pres;
    } else {
      aux_res = 0;
    }

    snprintf(app_buffer, APP_BUFFER_SIZE,
             "{\"values\":[{\"key\":\"%s\",\"value\":%d.%02u}]}",
             sensor->alarm_name, aux_int, aux_res);

    publish((uint8_t *)app_buffer, data_topic, strlen(app_buffer));

    /* Schedule the timer to prevent flooding the broker with the same event */
    etimer_set(&alarm_expired, (CLOCK_SECOND * DEFAULT_ALARM_TIME));
  }
}
/*---------------------------------------------------------------------------*/
static void
publish_event(sensor_values_t *msg)
{
  char aux[64];
  int len = 0;
  uint8_t i;
  uint16_t aux_int, aux_res;
  int remain = APP_BUFFER_SIZE;

  /* Clear buffer */
  memset(app_buffer, 0, APP_BUFFER_SIZE);

  /* Use the buf_ptr as pointer to the actual application buffer */
  buf_ptr = app_buffer;

  /* Retrieve our own IPv6 address
   * This is the starting value to be sent, the `first` argument should be 1,
   * and the `more` argument 1 as well, as we want to add more values to our
   * list
   */
  memset(aux, 0, sizeof(aux));

  len = add_pub_topic(remain, DEFAULT_PUBLISH_EVENT_ID, DEVICE_ID, 1, 1);
  remain =- len;

  /* Include the sensor values, if `sensor_name` is empty */
  for(i=0; i < msg->num; i++) {
    if(strlen(msg->sensor[i].sensor_name)) {
      memset(aux, 0, sizeof(aux));

      aux_int = msg->sensor[i].value;
      aux_res = msg->sensor[i].value;

      if(msg->sensor[i].pres > 0) {
        aux_int /= msg->sensor[i].pres;
        aux_res %= msg->sensor[i].pres;
      } else {
        aux_res = 0;
      }

      snprintf(aux, sizeof(aux), "%d.%02u", aux_int, aux_res);
      len = add_pub_topic(remain, msg->sensor[i].sensor_name, aux, 0, 1);
      remain =- len;
    }
  }

  memset(aux, 0, sizeof(aux));
  snprintf(aux, sizeof(aux), "%lu", clock_seconds());
  len = add_pub_topic(remain, DEFAULT_PUBLISH_EVENT_UPTIME, aux, 0, 1);
  remain =- len;

  memset(aux, 0, sizeof(aux));
  ipaddr_sprintf(aux, sizeof(aux), uip_ds6_defrt_choose());
  len = add_pub_topic(remain, DEFAULT_PUBLISH_EVENT_PARENT, aux, 0, 1);
  remain =- len;

  /* The last value to be sent, the `more` argument should be zero */
  memset(aux, 0, sizeof(aux));
  snprintf(aux, sizeof(aux), "%d", def_rt_rssi);
  len = add_pub_topic(remain, DEFAULT_PUBLISH_EVENT_RSSI, aux, 0, 0);

  PRINTF("Things.io: publish %s (%u)\n", app_buffer, strlen(app_buffer));
  publish((uint8_t *)app_buffer, data_topic, strlen(app_buffer));
}
/*---------------------------------------------------------------------------*/
/* This function handler receives publications to which we are subscribed */
static void
thingsio_pub_handler(const char *topic, uint16_t topic_len, const uint8_t *chunk,
            uint16_t chunk_len)
{
  uint8_t i;
  uint16_t aux;

  PRINTF("Things.io: Pub Handler, topic='%s' (len=%u), chunk='%s', chunk_len=%u\n",
         topic, topic_len, chunk, chunk_len);

  /* Most of the commands follow a boolean-logic at least */
  if(chunk_len <= 0) {
    PRINTF("Relay: Chunk should be at least a single digit integer or string\n");
    return;
  }

  /* This is a command event, it uses "true" and "false" strings
   * We expect commands to have the following syntax:
   * {"key":"enable_sensor","value":false}
   * That is why we use an index of "8" to search for the command string
   */
  if(strncmp(topic, cmd_topic, CONFIG_SUB_CMD_TOPIC_LEN) == 0) {

    /* Toggle a given LED */
    if(strncmp((const char *)&chunk[8], DEFAULT_SUBSCRIBE_CMD_LEDS,
               strlen(DEFAULT_SUBSCRIBE_CMD_LEDS)) == 0) {
      PRINTF("Things.io: Command received --> toggle LED\n");

      if(strncmp((const char *)&chunk[strlen(DEFAULT_SUBSCRIBE_CMD_LEDS) + 18],
        "true", 4) == 0) {
        leds_on(CMD_LED);
      } else if(strncmp((const char *)&chunk[strlen(DEFAULT_SUBSCRIBE_CMD_LEDS) + 18],
        "false", 5) == 0) {
        leds_off(CMD_LED);
      } else {
        PRINTF("Things.io: invalid command argument (expected boolean)!\n");
      }

      return;

    /* Restart the device */
    } else if(strncmp((const char *)&chunk[8], DEFAULT_SUBSCRIBE_CMD_REBOOT,
               strlen(DEFAULT_SUBSCRIBE_CMD_REBOOT)) == 0) {
      PRINTF("Things.io: Command received --> reboot\n");

      /* This is fixed to check only "true" arguments */
      if(strncmp((const char *)&chunk[strlen(DEFAULT_SUBSCRIBE_CMD_REBOOT) + 18],
        "true", 4) == 0) {
        sys_ctrl_reset();
      } else {
        PRINTF("Things.io: invalid command argument (expected only 'true')!\n");
      }

      return;

    /* Enable or disable external sensors */
    } else if(strncmp((const char *)&chunk[8], DEFAULT_SUBSCRIBE_CMD_SENSOR,
               strlen(DEFAULT_SUBSCRIBE_CMD_SENSOR)) == 0) {
      PRINTF("Things.io: Command received --> enable/disable sensor\n");

      if(strncmp((const char *)&chunk[strlen(DEFAULT_SUBSCRIBE_CMD_SENSOR) + 18],
        "true", 4) == 0) {
        activate_sensors(0x01);
      } else if(strncmp((const char *)&chunk[strlen(DEFAULT_SUBSCRIBE_CMD_SENSOR) + 18],
        "false", 5) == 0) {
        activate_sensors(0x00);
      } else {
        PRINTF("Things.io: invalid command argument (expected boolean)!\n");
      }

      return;

    /* This is a configuration event
     * As currently Contiki's MQTT driver does not support more than one SUBSCRIBE
     * we are handling both commands and configurations in the same "cmd" topic
     * We expect the configuration payload to follow the next syntax:
     * {"name":"update_period","value":61}
     */

    /* Change the update period */
   } else if(strncmp((const char *)&chunk[8], DEFAULT_SUBSCRIBE_CMD_EVENT,
               strlen(DEFAULT_SUBSCRIBE_CMD_EVENT)) == 0) {

      /* Take integers as configuration value */
      aux = atoi((const char*) &chunk[strlen(DEFAULT_SUBSCRIBE_CMD_EVENT) + 18]);

      /* Check for allowed values */
      if((aux < DEFAULT_UPDATE_PERIOD_MIN) || (aux > DEFAULT_UPDATE_PERIOD_MAX)) {
        PRINTF("Things.io: update interval should be between %u and %u\n", 
                DEFAULT_UPDATE_PERIOD_MIN, DEFAULT_UPDATE_PERIOD_MAX);
        return;
      }

      conf.pub_interval_check = aux;
      PRINTF("Things.io: New update interval --> %u secs\n", conf.pub_interval_check);

      // FIXME: write_config_to_flash();
      return;
    }

    /* Change a sensor's threshold, skip is `sensor_config` is empty */
    for(i=0; i<SENSORS_NAME(MQTT_SENSORS, _sensors.num); i++) {

      if((strlen(SENSORS_NAME(MQTT_SENSORS,_sensors.sensor[i].sensor_config))) &&
        (strncmp((const char *)&chunk[8], SENSORS_NAME(MQTT_SENSORS, _sensors.sensor[i].sensor_config),
                      strlen(SENSORS_NAME(MQTT_SENSORS, _sensors.sensor[i].sensor_config))) == 0)) {

        /* Take integers as configuration value */
        aux = atoi((const char*) &chunk[strlen(SENSORS_NAME(MQTT_SENSORS,_sensors.sensor[i].sensor_config)) + 18]);

        if((aux < SENSORS_NAME(MQTT_SENSORS,_sensors.sensor[i].min)) || 
          (aux > SENSORS_NAME(MQTT_SENSORS,_sensors.sensor[i].max))) {
          PRINTF("Things.io: %s threshold should be between %d and %d\n",
                 SENSORS_NAME(MQTT_SENSORS,_sensors.sensor[i].sensor_name),
                 SENSORS_NAME(MQTT_SENSORS,_sensors.sensor[i].min),
                 SENSORS_NAME(MQTT_SENSORS,_sensors.sensor[i].max));
          return;
        }

        /* We have now a valid threshold value, the logic is simple: each
         * variable has a `thresh` to configure a limit over the given value,
         * and `thresl` which respectively checks for values below this limit.
         * As a convention we are expecting `sensor_config` strings ending in
         * `_thresh` or `_thresl`.  The check below "should" be "safe" as we are
         * sure it matches an expected string.
         */

        if(strstr((const char *)&chunk[8], "_thresh") != NULL) {
          SENSORS_NAME(MQTT_SENSORS,_sensors.sensor[i].over_threshold) = aux;
          PRINTF("Things.io: New %s over threshold --> %u\n",
                 SENSORS_NAME(MQTT_SENSORS,_sensors.sensor[i].sensor_name),
                 SENSORS_NAME(MQTT_SENSORS,_sensors.sensor[i].over_threshold));
        } else if(strstr((const char *)&chunk[8], "_thresl") != NULL) {
          SENSORS_NAME(MQTT_SENSORS,_sensors.sensor[i].below_threshold) = aux;
          PRINTF("Things.io: New %s below threshold --> %u\n",
                 SENSORS_NAME(MQTT_SENSORS,_sensors.sensor[i].sensor_name),
                 SENSORS_NAME(MQTT_SENSORS,_sensors.sensor[i].below_threshold));
        } else {
          PRINTF("Things.io: Expected threshold configuration name to end ");
          PRINTF("either in thresh or thresl\n");
          /* Exit earlier to avoid writting in flash */
          return;
        }

        // FIXME: write_config_to_flash();
        return;
      }
    }

    /* We are now checking for any string command expected by the subscribed
     * sensor module
     */
#if DEFAULT_COMMANDS_NUM
    for(i=0; i<SENSORS_NAME(MQTT_SENSORS, _commands.num); i++) {

      if((strncmp((const char *)&chunk[8],
          SENSORS_NAME(MQTT_SENSORS, _commands.command[i].command_name),
          strlen(SENSORS_NAME(MQTT_SENSORS, _commands.command[i].command_name))) == 0)) {

        /* Take integers as argument value */
        aux = atoi((const char*) &chunk[strlen(SENSORS_NAME(MQTT_SENSORS,_commands.command[i].command_name)) + 18]);

        /* Invoke the command handler */
        SENSORS_NAME(MQTT_SENSORS,_commands.command[i].cmd(aux));
        return;
      }
    }
#endif /* DEFAULT_COMMANDS_NUM */

    /* Invalid configuration topic, we should have returned before */
    PRINTF("Things.io: Configuration/Command parameter not recognized\n");

  } else {
    PRINTF("Things.io: Incorrect topic or chunk len. Ignored\n");
  }
}
/*---------------------------------------------------------------------------*/
static void
init_platform(void)
{
  /* Register the publish callback handler */
  MQTT_PUB_REGISTER_HANDLER(thingsio_pub_handler);

  /* Configures a callback for a ping request to our parent node, to retrieve
   * the RSSI value
   */
  def_rt_rssi = 0x8000000;
  uip_icmp6_echo_reply_callback_add(&echo_reply_notification,
                                    echo_reply_handler);

  /* Create the client id */
  snprintf(conf.client_id, DEFAULT_IP_ADDR_STR_LEN, "%02x%02x%02x%02x%02x%02x",
           linkaddr_node_addr.u8[0], linkaddr_node_addr.u8[1],
           linkaddr_node_addr.u8[2], linkaddr_node_addr.u8[5],
           linkaddr_node_addr.u8[6], linkaddr_node_addr.u8[7]);

  /* Create topics */
  if(strlen(DEFAULT_CONF_AUTH_USER)) {
    snprintf(data_topic, CONFIG_PUB_TOPIC_LEN, "%s%s", DEFAULT_TOPIC_LONG,
             DEFAULT_PUB_STRING);
    snprintf(cmd_topic, CONFIG_SUB_CMD_TOPIC_LEN, "%s%s", DEFAULT_TOPIC_LONG,
             DEFAULT_CMD_STRING);
  } else {
    /* If we are here it means the mqtt_client has already check credentials */
    snprintf(data_topic, CONFIG_PUB_TOPIC_LEN, "%s%s%s", DEFAULT_TOPIC_STR,
             conf.auth_user, DEFAULT_PUB_STRING);
    snprintf(cmd_topic, CONFIG_SUB_CMD_TOPIC_LEN, "%s%s%s", DEFAULT_TOPIC_STR,
             conf.auth_user, DEFAULT_CMD_STRING);
  }
}
/*---------------------------------------------------------------------------*/
PROCESS_THREAD(thingsio_process, ev, data)
{
  PROCESS_BEGIN();

  /* Initialize platform-specific */
  init_platform();

  printf("\nThe Things.io process started\n");
  printf("  Client ID:    %s\n", conf.client_id);
  printf("  Data topic:   %s\n", data_topic);
  printf("  Cmd topic:    %s\n\n", cmd_topic);

  while(1) {

    PROCESS_YIELD();

    if(ev == mqtt_client_event_connected) {
      seq_nr_value = 0;

      /* Ping our current parent to retrieve the RSSI signal level */
      ping_parent();

      /* Subscribe to topics (MQTT driver only supports 1 topic at the moment */
      subscribe(cmd_topic);
      // subscribe((char *) DEFAULT_SUBSCRIBE_CFG);

      /* Enable the sensor */
      activate_sensors(0x01);
    }

    if(ev == mqtt_client_event_disconnected) {
      /* We are not connected, disable the sensors */
      activate_sensors(0x00);
    }

    /* Check for periodic publish events */
    if(ev == SENSORS_NAME(MQTT_SENSORS,_sensors_data_event)) {
      seq_nr_value++;

      /* The `pub_interval_check` is an external struct defined in mqtt-client */
      if(!(seq_nr_value % conf.pub_interval_check)) {
        sensor_values_t *msgPtr = (sensor_values_t *) data;
        publish_event(msgPtr);
      }
    }

    /* Check for alarms */
    if(ev == SENSORS_NAME(MQTT_SENSORS,_sensors_alarm_event)) {
      sensor_val_t *sensorPtr = (sensor_val_t *) data;
      publish_alarm(sensorPtr);
    }
  }

  PROCESS_END();
}
/*---------------------------------------------------------------------------*/
/** @} */
