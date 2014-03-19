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
#define SERVICE_ID 190

#define SEND_INTERVAL		(10 * CLOCK_SECOND)
#define SEND_TIME		(random_rand() % (SEND_INTERVAL))
#define REPLY_TIMEOUT (3 * CLOCK_SECOND)

static struct mqtt_sn_connection mqtt_sn_c;
static char *mqtt_client_id="sensor";
static char topic_name[]="AR";
static char ctrl_topic[21] = "0000000000000000/ctrl";//of form "0011223344556677/ctrl" it is not null terminated, and is 21 charactes
static uint16_t ctrl_topic_id;
static uint16_t publisher_topic_id;
static publish_packet_t incoming_packet;
static uint16_t ctrl_topic_msg_id;
static uint16_t reg_topic_msg_id;
static uint16_t mqtt_keep_alive=20;
const char *message_data = NULL;
static uint16_t topic_id = 0;
static uint8_t topic_id_type = MQTT_SN_TOPIC_TYPE_SHORT;
static int8_t qos = 1;
uint8_t retain = FALSE;
//uint8_t debug = FALSE;

enum mqttsn_connection_status
{
  MQTTSN_DISCONNECTED =0,
  MQTTSN_WAITING_CONNACK,
  MQTTSN_CONNECTION_FAILED,
  MQTTSN_CONNECTED
};

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
  CTRL_UNSUBSCRIBED,
  CTRL_WAITING_SUBACK,
  CTRL_SUBSCRIBE_FAILED,
  CTRL_SUBSCRIBED
};

static enum mqttsn_connection_status connection_state = DISCONNECTED;
static enum topic_registration_status registration_state = UNREGISTERED;
static enum ctrl_subscription_status ctrl_subscription_state = CTRL_UNSUBSCRIBED;

/*A few events for managing device state*/
static process_event_t mqttsn_connack_event;
static process_event_t mqttsn_regack_event;
static process_event_t ctrl_suback_event;

/*---------------------------------------------------------------------------*/
PROCESS(example_mqttsn_process, "Configure Connection and Topic Registration");

PROCESS(publish_process, "publish data to the broker");
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

  printf("Connack received\n");
}
/*---------------------------------------------------------------------------*/
static void
regack_receiver(struct mqtt_sn_connection *mqc, const uip_ipaddr_t *source_addr, const uint8_t *data, uint16_t datalen)
{
  uint16_t incoming_message_id;
  regack_packet_t incoming_regack;
  memcpy(&incoming_regack, data, datalen);
  printf("Regack received\n");
  if (incoming_regack.message_id == reg_topic_msg_id) {
    if (incoming_regack.return_code == ACCEPTED) {
      publisher_topic_id = incoming_regack.topic_id;
      process_post(&publish_process,mqttsn_regack_event, NULL);
    } else {
      printf("Regack error: %s\n", mqtt_sn_return_code_string(packet->return_code));
    }
  }
}
/*---------------------------------------------------------------------------*/
static void
suback_receiver(struct mqtt_sn_connection *mqc, const uip_ipaddr_t *source_addr, const uint8_t *data, uint16_t datalen)
{
  uint16_t incoming_message_id;
  suback_packet_t incoming_suback;
  memcpy(&incoming_suback, data, datalen);
  printf("Suback received\n");
  if (incoming_suback.message_id == ctrl_topic_msg_id) {
    if (incoming_suback.return_code == ACCEPTED) {
      ctrl_topic_id = incoming_suback.topic_id;
      process_post(&ctrl_subscription_process,ctrl_suback_event, NULL);
    } else {
      printf("Suback error: %s\n", mqtt_sn_return_code_string(packet->return_code));
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
    printf("unknown publication received\n")'
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
PROCESS(publish_process, "register topic and publish data");
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
  static char buf[20];
  PROCESS_BEGIN();
  mqttsn_regack_event = process_alloc_event();
  //register topic
  printf("registering topic\n");
  registration_tries =0;
  registration_timeout_event = process_alloc_event();
  ctimer_set( &registration_timer, REPLY_TIMEOUT, registration_timer_callback, NULL);
  while (registration_tries < 4)
  {
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
      registration_tries = 0;
      printf("registration acked\n");
      ctimer_stop(&registration_timer);
      registration_state = MQTTSN_REGISTERED;
      registration tries = 4;//using break here may mess up switch statement of process
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
      topic_id = (topic_name[0] << 8) + topic_name[1];
      mqtt_sn_send_publish(&mqtt_sn_c, topic_id,topic_id_type,buf, buf_len,qos,retain);
      etimer_reset(&send_timer);
    }
  } else {
    printf("unable to register topic\n")
  }

  PROCESS_END();
}

/*---------------------------------------------------------------------------*/
/*this process will create a subscription and monitor for incoming traffic*/
PROCESS(ctrl_subsciption_process, "subscribe to a device control channel");
static struct ctimer subscription_timer;
static process_event_t subscription_timeout_event;

static void subscription_timer_callback(void *mqc)
{
  process_post(&ctrl_subsciption_process, subscription_timeout_event, NULL);
}

PROCESS_THREAD(ctrl_subsciption_process, ev, data)
{
  char device_id[17];
  static uint8_t subscription_tries;
  PROCESS_BEGIN();
  ctrl_suback_event = process_alloc_event();
  subscription_tries = 0;
  sprintf(device_id,"%02X%02X%02X%02X%02X%02X%02X%02X",rimeaddr_node_addr);
  memcpy(ctrl_topic,device_id,16);
  subscription_timeout_event = process_alloc_event();
  ctimer_set( &subscription_timer, REPLY_TIMEOUT, subscription_timer_callback, NULL);
  while(subscription_tries < 10) {
    //request subscription
    ctrl_topic_msg_id = mqtt_sn_send_subscribe(mqtt_sn_c,ctrl_topic,0);//QOS 1 currently unsupported on client
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
      subscription_tries = 0;
      printf("subscription to control topic acked\n");
      ctimer_stop(&subscription_timer);
      ctrl_subscription_state = CTRL_SUBSCRIBED;
      subscription tries = 10;//using break here may mess up switch statement of process
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
static struct ctimer connection_timer;
static process_event_t connection_timeout_event;

static void connection_timer_callback(void *mqc)
{
  process_post(&ctrl_subsciption_process, connection_timeout_event, NULL);
}

PROCESS_THREAD(example_mqttsn_process, ev, data)
{
  static struct etimer periodic_timer;
  static uip_ipaddr_t broker_addr;


  static char ctrl_topic[25];
  static uint8_t ctrl_channel_status = 0;
  static uint8_t mqttsn_retries = 0;



  PROCESS_BEGIN();

  mqttsn_connack_event = process_alloc_event();

  mqtt_sn_set_debug(1);
  //uip_ip6addr(&broker_addr, 0xaaaa, 0, 0, 0, 0, 0, 0, 1);
  //uip_ip6addr(&broker_addr, 0x2001, 0x0db8, 1, 0xffff, 0, 0, 0xc0a8, 0xd480);//192.168.212.128 with tayga
  //uip_ip6addr(&broker_addr, 0xaaaa, 0, 2, 0xeeee, 0, 0, 0xc0a8, 0xd480);//192.168.212.128 with tayga
  //uip_ip6addr(&broker_addr, 0xaaaa, 0, 2, 0xeeee, 0, 0, 0xac10, 0xdc01);//172.16.220.1 with tayga
  uip_ip6addr(&broker_addr, 0xaaaa, 0, 2, 0xeeee, 0, 0, 0xac10, 0xdc80);//172.16.220.128 with tayga
  mqtt_sn_create_socket(&mqtt_sn_c,35555, &broker_addr, UDP_PORT);
  (&mqtt_sn_c)->mc = &mqtt_sn_call;

  /*Wait a little to let system get set*/
  etimer_set(&periodic_timer, 10*CLOCK_SECOND);
  PROCESS_WAIT_EVENT_UNTIL(etimer_expired(&periodic_timer));

  /*Request a connection and wait for connack*/
  printf("requesting connection \n ");
  while (mqttsn_retries < 4)
  {
    mqtt_sn_send_connect(&mqtt_sn_c,mqtt_client_id,mqtt_keep_alive);
    PROCESS_WAIT_EVENT();
    if (ev == connack_event) {

    }
    if (ev == timeout_event) {

    }
  }
  if (connection_state == MQTTSN_CONNECTED){
    //start topic subscription process
    //start topic publish process
    //monitor connection
    while(1)
    {
      PROCESS_WAIT_EVENT();
    }
  } else {
    printf("unable to connect\n")
  }




  PROCESS_END();
}
/*---------------------------------------------------------------------------*/

