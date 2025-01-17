// #############################################################################
// #                        ESP8266 WiFi Weather Sensor                        #
// #############################################################################
// # push.c - Push data to HTTP service or MQTT server                         #
// #############################################################################
// #            Version: 1.2 - Compiler: esp-open-sdk 2.1.0 (Linux)            #
// #  (c) 2015-2019 by Malte Pöggel - www.MALTEPOEGGEL.de - malte@poeggel.de   #
// #############################################################################
// #  This program is free software; you can redistribute it and/or modify it  #
// #   under the terms of the GNU General Public License as published by the   #
// #        Free Software Foundation; either version 3 of the License,         #
// #                  or (at your option) any later version.                   #
// #                                                                           #
// #      This program is distributed in the hope that it will be useful,      #
// #      but WITHOUT ANY WARRANTY; without even the implied warranty of       #
// #           MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.            #
// #           See the GNU General Public License for more details.            #
// #                                                                           #
// #  You should have received a copy of the GNU General Public License along  #
// #      with this program; if not, see <http://www.gnu.org/licenses/>.       #
// #############################################################################

#include <esp8266.h>
#include "push.h"
#include "sensors.h"
#include "httpclient.h"
#include "mqtt.h"
#include "queue.h"
#include "config.h"
#include "strreplace.h"

enum push_states pushState;
uint8_t pushErrorDelay;
uint8_t pushRetries;
uint8_t pushRedirect;
MQTT_Client mqttClient;


// Start sensor data push
void ICACHE_FLASH_ATTR pushSensorData( void )
 {
  pushState = PUSH_INIT;
  pushErrorDelay = 0;
  pushRetries = 0;
  pushRedirect = 0;
  os_printf("Push: Start\n");
  statusLed(LED_FLASH1);
  pushTimer();
  pushTimeout();
 }


// Start push timer
void ICACHE_FLASH_ATTR pushTimer( void )
 {
  static ETSTimer pushTimer;
  os_timer_disarm(&pushTimer);
  os_timer_setfn(&pushTimer, pushTimerCb, NULL);
  os_timer_arm(&pushTimer, PUSH_CHECK_INTERVAL_MSEC, 0);
 }


// Callback to check push state machine
void ICACHE_FLASH_ATTR pushTimerCb( void *arg )
 {
  uint8_t tmp;
  switch(pushState)
   {
    case PUSH_IDLE:
     break;
    case PUSH_INIT:
     // Check if push neccesary
     if(!configGet()->http_enabled&&!configGet()->mqtt_enabled)
      {
       pushState = PUSH_FINISH;
       break;
      }
     statusLed(LED_FLASH1);
     pushRetries = 0;
     ++pushState;
     break;
    case PUSH_READ:
     if(sensorsRead(pushRetries)<0)
      {
       // At least one sensor value failed, retry five times then continue anyway.
       if(++pushRetries>=5) ++pushState;
      } else {
       // Reading done
       ++pushState;
      }
     break;
    case PUSH_IPWAIT:
     // IP address available?
     tmp = wifi_station_get_connect_status();
     if(tmp==STATION_GOT_IP)
      {
       ++pushState;
      }
     break;
    case PUSH_HTTPSTART:
     // Initialize HTTP
     pushRetries = 0;
     ++pushState;
     break;
    case PUSH_HTTP:
     // Push data via HTTP
     statusLed(LED_FLASH2);
     pushRedirect = PUSH_REDIRECT_MAX;
     if(httpPush()==0&&pushRetries<(PUSH_RETRIES_MAX-1))
      {
       ++pushState;
      } else {
       pushState+=2;
      }
     break;
    case PUSH_HTTPWAIT:
     // Wait for status update
     if(pushErrorDelay>0)
      {
       statusLed(LED_FLASH3);
       if(--pushErrorDelay==0) pushState = PUSH_HTTP;
      }
     break;
    case PUSH_MQTTSTART:
     // Initialize MQTT
     pushRetries = 0;
     ++pushState;
     break;
    case PUSH_MQTT:
     // Push data via MQTT
     statusLed(LED_FLASH2);
     if(mqttPush()==0&&pushRetries<(PUSH_RETRIES_MAX-1))
      {
       ++pushState;
      } else {
       pushState+=2;
      }
     break;
    case PUSH_MQTTWAIT:
     // Wait for status update
     if(pushErrorDelay>0)
      {
       statusLed(LED_FLASH3);
       if(--pushErrorDelay==0) pushState = PUSH_MQTT;
      }
     break;
    case PUSH_FINISH:
     // Finish sending data
     statusLed(LED_OFF);
     os_printf("Push: Done\n");
     if(configIsFirstStart())
      {
       statusLed(LED_FLASH4);
       os_printf("Push: First start, user may press button to reconfigure. Waiting...\n");
      } else {
       sleepmode();
      }
     pushState = PUSH_IDLE;
     break;
    default:
     // Should not happen
     pushState = PUSH_IDLE;
     break;
   }
  if(pushState>PUSH_IDLE)
   {
    pushTimer();
   }
 }


// Push via HTTP
uint8_t ICACHE_FLASH_ATTR httpPush( void )
 {
  char buff[256];
  uint8_t mode = configGet()->http_enabled;
  if(mode==0) return 1;
  os_printf("Push: HTTP\n");
  buff[0]='\0';
  if(mode==1)
   {
    // thingspeak.com
    os_sprintf(buff, HTTP_THINGSPEAK, configGet()->http_key, temperatureToString(), humidityToString(), pressureToString(), batteryVoltageToString(), rssiToString());
   } else
    if(mode==2)
     {
      // adafruit.io
      os_sprintf(buff, HTTP_ADAFRUIT, configGet()->http_grp, configGet()->http_key, temperatureToString(), humidityToString(), pressureToString(), batteryVoltageToString(), rssiToString());
     } else {
      // Custom URL
      os_strcpy(buff, configGet()->http_url);
      strreplace(buff, "%t", temperatureToString(), sizeof(buff));
      strreplace(buff, "%h", humidityToString(), sizeof(buff));
      strreplace(buff, "%p", pressureToString(), sizeof(buff));
      strreplace(buff, "%v", batteryVoltageToString(), sizeof(buff));
      strreplace(buff, "%r", rssiToString(), sizeof(buff));
     }
  if(!http_get(buff, "", httpPushCb)) return 1;
  //os_printf("Push: URL=%s\n", buff);
  return 0;
 }


// Callback for HTTP push
void ICACHE_FLASH_ATTR httpPushCb( char *response, int http_status, char *full_response )
 {
  char* tmp;
  int value;
  char buff[256];
  // Request successful?
  if(http_status==200)
   {
    tmp = os_strstr(full_response, "X-Interval-Override: ");
    if(tmp!=NULL)
     {
      // Set temporary interval (min 30s / max 6h)
      tmp += 21;
      strtok(tmp, "\r\n");
      value = atoi(tmp);
      if(value>=30&&value<=21600)
       {
        configGet()->sensor_interval = value;
       }
     }
    ++pushState;
   } else
    if(http_status==301||http_status==302)
    {
     if(--pushRedirect==0)
      {
       os_printf("Push: Too many redirects\n");
       ++pushState;
      } else {
       tmp = os_strstr(full_response, "Location: ");
       if(tmp!=NULL)
        {
         tmp += 10;
         strtok(tmp, "\r\n");
         os_strncpy(buff, tmp, sizeof(buff));
         buff[(sizeof(buff)-1)] = '\0';
         //os_printf("Push: HTTP redirect\n");
         if(!http_get(buff, "", httpPushCb))
          {
           pushErrorDelay = PUSH_ERROR_DELAY+1;
           ++pushRetries;
          }
        } else {
         pushErrorDelay = PUSH_ERROR_DELAY+1;
         ++pushRetries;
        }
      }
    } else {
     pushErrorDelay = PUSH_ERROR_DELAY+1;
     ++pushRetries;
    }
 }


// Push via MQTT
uint8_t ICACHE_FLASH_ATTR mqttPush( void )
 {
  uint32_t chip_id;
  char id_str[20];
  if(configGet()->mqtt_enabled==0) return 1;
  os_printf("Push: MQTT\n");
  os_sprintf(id_str, "ESP_Weather%06X", system_get_chip_id());
  MQTT_InitConnection(&mqttClient, configGet()->mqtt_host, configGet()->mqtt_port, (configGet()->mqtt_enabled==1?0:1));
  MQTT_InitClient(&mqttClient, id_str, configGet()->mqtt_user, configGet()->mqtt_pass, 120, 1);
  MQTT_OnConnected(&mqttClient, mqttPushCb);
  MQTT_OnDisconnected(&mqttClient, NULL);
  MQTT_OnData(&mqttClient, NULL);
  MQTT_OnPublished(&mqttClient, mqttPublishCb);
  MQTT_OnTimeout(&mqttClient, mqttErrorCb);
  MQTT_Connect(&mqttClient);
  return 0;
 }


// Callback for MQTT push, which is called after connect
void ICACHE_FLASH_ATTR mqttPushCb(uint32_t *args)
 {
  MQTT_Client* client = (MQTT_Client*)args;
  mqttPublish(client, configGet()->mqtt_topic, "temperature", temperatureToString());
  mqttPublish(client, configGet()->mqtt_topic, "humidity", humidityToString());
  mqttPublish(client, configGet()->mqtt_topic, "pressure", pressureToString());
  mqttPublish(client, configGet()->mqtt_topic, "battery", batteryVoltageToString());
  mqttPublish(client, configGet()->mqtt_topic, "rssi", rssiToString());
 }


// Publish data to MQTT topic
void ICACHE_FLASH_ATTR mqttPublish(MQTT_Client* client, char* topic, char* sub, char* msg)
 {
  char tmp_topic[128];
  os_strncpy(tmp_topic, topic, sizeof(tmp_topic));
  if(tmp_topic[strlen(tmp_topic)]!='/') os_strcat(tmp_topic, "/");
  os_strcat(tmp_topic, sub);
  MQTT_Publish(client, tmp_topic, msg, strlen(msg), 0, 0);
 }


// Called if a message was published successful
void ICACHE_FLASH_ATTR mqttPublishCb(uint32_t *args)
 {
  if(QUEUE_IsEmpty(&mqttClient.msgQueue))
   {
    MQTT_Disconnect(&mqttClient);
    MQTT_DeleteClient(&mqttClient);
    ++pushState;
   }
 }


// Called if MQTT push fails
void ICACHE_FLASH_ATTR mqttErrorCb(uint32_t *args)
 {
  os_printf("Push: MQTT failed\n");
  MQTT_Disconnect(&mqttClient);
  MQTT_DeleteClient(&mqttClient);
  pushErrorDelay = PUSH_ERROR_DELAY+1;
  ++pushRetries;
 }


// Initialize push timeout after which the sensor goes to sleep
void ICACHE_FLASH_ATTR pushTimeout( void ) 
 {
  // Shut down after 30 seconds
  static ETSTimer pushTimeoutTimer;
  os_timer_disarm(&pushTimeoutTimer);
  os_timer_setfn(&pushTimeoutTimer, pushTimeoutCb, NULL);
  os_timer_arm(&pushTimeoutTimer, PUSH_TIMEOUT_SEC*1000, 0);
 }


// Callback for timeout
static void ICACHE_FLASH_ATTR pushTimeoutCb( void *arg )
 {
  os_printf("Push: Timeout :-/\n");
  sleepmode();
 }


// Sleep defined interval
void ICACHE_FLASH_ATTR sleepmode( void )
 {
  uint16_t interval = configGet()->sensor_interval;
  os_printf("Sleep %d sec, goodnight.\n", interval);
  system_deep_sleep(interval*1000*1000);
 }
