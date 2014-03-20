#include "contiki.h"
#include "lib/random.h"
#include "sys/ctimer.h"
#include "sys/etimer.h"
#include "net/uip.h"
#include "net/uip-ds6.h"
//#include "net/uip-debug.h"
#include "mqtt-sn.h"

#include "net/rime.h"

#include "simple-udp.h"
//#include "servreg-hack.h"

#include <stdio.h>
#include <string.h>

#define UDP_PORT 1884

#define SEND_INTERVAL		(10 * CLOCK_SECOND)
#define SEND_TIME		(random_rand() % (SEND_INTERVAL))
#define REPLY_TIMEOUT (3 * CLOCK_SECOND)

static struct mqtt_sn_connection mqtt_sn_c;
static char *mqtt_client_id="sensor";
static char ctrl_topic[21] = "0000000000000000/ctrl";//of form "0011223344556677/ctrl" it is not null terminated, and is 21 charactes
static char pub_topic[20] = "0000000000000000/msg";
static uint16_t ctrl_topic_id;
static uint16_t publisher_topic_id;
static publish_packet_t incoming_packet;
static uint16_t ctrl_topic_msg_id;
static uint16_t reg_topic_msg_id;
static uint16_t mqtt_keep_alive=20;
static int8_t qos = 1;
static uint8_t retain = FALSE;
static char device_id[17];
//uint8_t debug = FALSE;

//enum mqttsn_connection_status
//{
//  MQTTSN_DISCONNECTED =0,
//  MQTTSN_WAITING_CONNACK,
//  MQTTSN_CONNECTION_FAILED,
//  MQTTSN_CONNECTED
//};

enum topic_registration_status
{
  MQTTSN_UNREGISTERED = 0,
  MQTTSN_WAITING_REGACK,
  MQTTSN_WAITING_TOPIC_ID,
  MQTTSN_REGISTER_FAILED,
  MQTTSN_REGISTERED
};

enum ctrl_subscription_status
{
  CTRL_UNSUBSCRIBED = 0,
  CTRL_WAITING_SUBACK,
  CTRL_SUBSCRIBE_FAILED,
  CTRL_SUBSCRIBED
};

static enum mqttsn_connection_status connection_state = MQTTSN_DISCONNECTED;
static enum topic_registration_status registration_state = MQTTSN_UNREGISTERED;
static enum ctrl_subscription_status ctrl_subscription_state = CTRL_UNSUBSCRIBED;

/*A few events for managing device state*/
static process_event_t mqttsn_connack_event;
static process_event_t mqttsn_regack_event;
static process_event_t ctrl_suback_event;

PROCESS(example_mqttsn_process, "Configure Connection and Topic Registration");
PROCESS(publish_process, "register topic and publish data");
PROCESS(ctrl_subscription_process, "subscribe to a device control channel");


AUTOSTART_PROCESSES(&example_mqttsn_process);

/*---------------------------------------------------------------------------*/
static void
puback_receiver(struct mqtt_sn_connection *mqc, const uip_ipaddr_t *source_addr, const uint8_t *data, uint16_t datalen)
{
  printf("Puback received\n");
}
/*---------------------------------------------------------------------------*/
static void
connack_receiver(struct mqtt_sn_connection *mqc, const uip_ipaddr_t *source_addr, const uint8_t *data, uint16_t datalen)
{
  uint8_t connack_return_code;
  connack_return_code = *(data + 3);
  printf("Connack received\n");
  if (connack_return_code == ACCEPTED) {
    process_post(&example_mqttsn_process, mqttsn_connack_event, NULL);
  } else {
    printf("Connack error: %s\n", mqtt_sn_return_code_string(connack_return_code));
  }
}
/*---------------------------------------------------------------------------*/
static void
regack_receiver(struct mqtt_sn_connection *mqc, const uip_ipaddr_t *source_addr, const uint8_t *data, uint16_t datalen)
{
  regack_packet_t incoming_regack;
  memcpy(&incoming_regack, data, datalen);
  printf("Regack received\n");
  if (incoming_regack.message_id == reg_topic_msg_id) {
    if (incoming_regack.return_code == ACCEPTED) {
      publisher_topic_id = incoming_regack.topic_id;
      process_post(&publish_process,mqttsn_regack_event, NULL);
    } else {
      printf("Regack error: %s\n", mqtt_sn_return_code_string(incoming_regack.return_code));
    }
  }
}
/*---------------------------------------------------------------------------*/
static void
suback_receiver(struct mqtt_sn_connection *mqc, const uip_ipaddr_t *source_addr, const uint8_t *data, uint16_t datalen)
{
  suback_packet_t incoming_suback;
  memcpy(&incoming_suback, data, datalen);
  printf("Suback received\n");
  if (incoming_suback.message_id == ctrl_topic_msg_id) {
    if (incoming_suback.return_code == ACCEPTED) {
      ctrl_topic_id = incoming_suback.topic_id;
      process_post(&ctrl_subscription_process,ctrl_suback_event, NULL);
    } else {
      printf("Suback error: %s\n", mqtt_sn_return_code_string(incoming_suback.return_code));
    }
  }
}
/*---------------------------------------------------------------------------*/
static void
publish_receiver(struct mqtt_sn_connection *mqc, const uip_ipaddr_t *source_addr, const uint8_t *data, uint16_t datalen)
{
  memcpy(&incoming_packet, data, datalen);
  printf("Published message received\n");
  //see if this message corresponds to ctrl channel subscription request
  if (incoming_packet.topic_id == ctrl_topic_id) {
    //do something with incoming data here
  } else {
    printf("unknown publication received\n");
  }

}
/*---------------------------------------------------------------------------*/
/*Add callbacks here if we make them*/
static const struct mqtt_sn_callbacks mqtt_sn_call = {
  publish_receiver,
  NULL,
  NULL,
  connack_receiver,
  regack_receiver,
  puback_receiver,
  suback_receiver,
  NULL,
  NULL
  };

/*---------------------------------------------------------------------------*/
/*this process will publish data at regular intervals*/
static struct ctimer registration_timer;
static process_event_t registration_timeout_event;

static void registration_timer_callback(void *mqc)
{
  process_post(&publish_process, registration_timeout_event, NULL);
}

PROCESS_THREAD(publish_process, ev, data)
{
  static uint8_t registration_tries;
  static struct etimer send_timer;
  static uint8_t buf_len;
  static uint8_t message_number;
  static char buf[20];

  PROCESS_BEGIN();
  memcpy(pub_topic,device_id,16);
  mqttsn_regack_event = process_alloc_event();
  printf("registering topic\n");
  registration_tries =0;
  registration_timeout_event = process_alloc_event();
  ctimer_set( &registration_timer, REPLY_TIMEOUT, registration_timer_callback, NULL);
  while (registration_tries < 4)
  {
    reg_topic_msg_id = mqtt_sn_send_register(&mqtt_sn_c, pub_topic);
    PROCESS_WAIT_EVENT();
    if (ev == registration_timeout_event)
    {
      registration_state = MQTTSN_REGISTER_FAILED;
      registration_tries++;
      printf("registration timeout\n");
      ctimer_restart(&registration_timer);
    }
    else if (ev == mqttsn_regack_event)
    {
      //if success
      printf("registration acked\n");
      ctimer_stop(&registration_timer);
      registration_state = MQTTSN_REGISTERED;
      registration_tries = 4;//using break here may mess up switch statement of process
    }
  }
  ctimer_stop(&registration_timer);
  if (registration_state == MQTTSN_REGISTERED){
    //start topic publishing to topic at regular intervals
    while(1)
    {
      PROCESS_WAIT_EVENT_UNTIL(etimer_expired(&send_timer));
      printf("publishing \n ");
      sprintf(buf, "Message %d", message_number);
      message_number++;
      buf_len = strlen(buf);
      mqtt_sn_send_publish(&mqtt_sn_c, publisher_topic_id,MQTT_SN_TOPIC_TYPE_NORMAL,buf, buf_len,qos,retain);
      etimer_reset(&send_timer);
    }
  } else {
    printf("unable to register topic\n");
  }

  PROCESS_END();
}

/*---------------------------------------------------------------------------*/
/*this process will create a subscription and monitor for incoming traffic*/
static struct ctimer subscription_timer;
static process_event_t subscription_timeout_event;

static void subscription_timer_callback(void *mqc)
{
  process_post(&ctrl_subscription_process, subscription_timeout_event, NULL);
}

PROCESS_THREAD(ctrl_subscription_process, ev, data)
{
  static uint8_t subscription_tries;
  PROCESS_BEGIN();
  ctrl_suback_event = process_alloc_event();
  subscription_tries = 0;
  memcpy(ctrl_topic,device_id,16);
  subscription_timeout_event = process_alloc_event();
  ctimer_set( &subscription_timer, REPLY_TIMEOUT, subscription_timer_callback, NULL);
  while(subscription_tries < 10) {
    //request subscription
    ctrl_topic_msg_id = mqtt_sn_send_subscribe(&mqtt_sn_c,ctrl_topic,0);//QOS 1 currently unsupported on client
    ctrl_subscription_state = CTRL_WAITING_SUBACK;
    PROCESS_WAIT_EVENT();
    if (ev == subscription_timeout_event)
    {
      ctrl_subscription_state = CTRL_SUBSCRIBE_FAILED;
      subscription_tries++;
      printf("subscription timeout\n");
      ctimer_restart(&subscription_timer);
    }
    else if (ev == ctrl_suback_event)
    {
      //if success
      printf("subscription to control topic acked\n");
      ctimer_stop(&subscription_timer);
      ctrl_subscription_state = CTRL_SUBSCRIBED;
      subscription_tries = 10;//using break here may mess up switch statement of process
    }
  }
  if (ctrl_subscription_state != CTRL_SUBSCRIBED) {
    printf("subscription to control topic failed\n");
  }
  ctimer_stop(&subscription_timer);

  PROCESS_END();
}

/*---------------------------------------------------------------------------*/
/*this main process will create connection and register topics*/
/*---------------------------------------------------------------------------*/


static struct ctimer connection_timer;
static process_event_t connection_timeout_event;

static void connection_timer_callback(void *mqc)
{
  process_post(&ctrl_subscription_process, connection_timeout_event, NULL);
}

PROCESS_THREAD(example_mqttsn_process, ev, data)
{
  static struct etimer periodic_timer;
  static uip_ipaddr_t broker_addr;
  static uint8_t connection_retries = 0;

  PROCESS_BEGIN();

  mqttsn_connack_event = process_alloc_event();

  mqtt_sn_set_debug(1);
  //uip_ip6addr(&broker_addr, 0xaaaa, 0, 0, 0, 0, 0, 0, 1);
  //uip_ip6addr(&broker_addr, 0x2001, 0x0db8, 1, 0xffff, 0, 0, 0xc0a8, 0xd480);//192.168.212.128 with tayga
  //uip_ip6addr(&broker_addr, 0xaaaa, 0, 2, 0xeeee, 0, 0, 0xc0a8, 0xd480);//192.168.212.128 with tayga
  //uip_ip6addr(&broker_addr, 0xaaaa, 0, 2, 0xeeee, 0, 0, 0xac10, 0xdc01);//172.16.220.1 with tayga
  uip_ip6addr(&broker_addr, 0xaaaa, 0, 2, 0xeeee, 0, 0, 0xac10, 0xdc80);//172.16.220.128 with tayga
  mqtt_sn_create_socket(&mqtt_sn_c,UDP_PORT, &broker_addr, UDP_PORT);
  (&mqtt_sn_c)->mc = &mqtt_sn_call;

  sprintf(device_id,"%02X%02X%02X%02X%02X%02X%02X%02X",rimeaddr_node_addr.u8[0],
          rimeaddr_node_addr.u8[1],rimeaddr_node_addr.u8[2],rimeaddr_node_addr.u8[3],
          rimeaddr_node_addr.u8[4],rimeaddr_node_addr.u8[5],rimeaddr_node_addr.u8[6],
          rimeaddr_node_addr.u8[7]);

  /*Wait a little to let system get set*/
  etimer_set(&periodic_timer, 10*CLOCK_SECOND);
  PROCESS_WAIT_EVENT_UNTIL(etimer_expired(&periodic_timer));

  /*Request a connection and wait for connack*/
  printf("requesting connection \n ");
  connection_timeout_event = process_alloc_event();
  ctimer_set( &connection_timer, REPLY_TIMEOUT, connection_timer_callback, NULL);
  while (connection_retries < 4)
  {
    mqtt_sn_send_connect(&mqtt_sn_c,mqtt_client_id,mqtt_keep_alive);
    PROCESS_WAIT_EVENT();
    if (ev == mqttsn_connack_event) {
      //if success
      printf("connection acked\n");
      ctimer_stop(&connection_timer);
      connection_state = CTRL_SUBSCRIBED;
      connection_retries = 4;//using break here may mess up switch statement of proces
    }
    if (ev == connection_timeout_event) {
      connection_state = MQTTSN_CONNECTION_FAILED;
      connection_retries++;
      printf("connection timeout\n");
      ctimer_restart(&connection_timer);
    }
  }
  ctimer_stop(&subscription_timer);
  if (connection_state == MQTTSN_CONNECTED){
    process_start(&ctrl_subscription_process, 0);
    process_start(&publish_process, 0);
    //monitor connection
    while(1)
    {
      PROCESS_WAIT_EVENT();
    }
  } else {
    printf("unable to connect\n");
  }




  PROCESS_END();
}
/*---------------------------------------------------------------------------*/

