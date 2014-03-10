#include "contiki.h"
#include "lib/random.h"
#include "sys/ctimer.h"
#include "sys/etimer.h"
#include "net/uip.h"
#include "net/uip-ds6.h"
//#include "net/uip-debug.h"
#include "mqtt-sn.h"

#include "node-id.h"

#include "simple-udp.h"
//#include "servreg-hack.h"

#include <stdio.h>
#include <string.h>

#define UDP_PORT 1884
#define SERVICE_ID 190

#define SEND_INTERVAL		(10 * CLOCK_SECOND)
#define SEND_TIME		(random_rand() % (SEND_INTERVAL))

static struct mqtt_sn_connection mqtt_sn_c;
static char *mqtt_client_id="sensor";
static char topic_name[]="AR";
static uint16_t mqtt_keep_alive=20;
const char *message_data = NULL;
static uint16_t topic_id = 0;
static uint8_t topic_id_type = MQTT_SN_TOPIC_TYPE_SHORT;
static int8_t qos = 1;
uint8_t retain = FALSE;
//uint8_t debug = FALSE;

enum mqttsn_connection_status
{
  DISCONNECTED =0,
  WAITING_CONNACK,
  CONNECTION_FAILED,
  CONNECTED
};

enum topic_registration_status
{
  UNREGISTERED = 0,
  WAITING_REGACK,
  WAITING_TOPIC_ID,
  REGISTER_FAILED,
  REGISTERED
};

enum ctrl_subscription_status
{
  CTRL_UNSUBSCRIBED,
  CTRL_WAITING_SUBACK,
  CTRL_SUBSCRIBE_FAILED,
  CTRL_SUBSCRIBED
};


/*A few events for managing device state*/
static process_event_t mqttsn_connack_event;
static process_event_t mqttsn_regack_event;
static process_event_t mqttsn_suback_event;


/*---------------------------------------------------------------------------*/
PROCESS(unicast_sender_process, "Unicast sender example process");
PROCESS(ctrl_subsciption_process, "subscribe to a device control channel");
AUTOSTART_PROCESSES(&unicast_sender_process);
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
set_global_address(void)
{
  uip_ipaddr_t ipaddr;
  int i;
  uint8_t state;

  uip_ip6addr(&ipaddr, 0xaaaa, 0, 0, 0, 0, 0, 0, 0);
  uip_ds6_set_addr_iid(&ipaddr, &uip_lladdr);
  uip_ds6_addr_add(&ipaddr, 0, ADDR_AUTOCONF);

  printf("IPv6 addresses: ");
  for(i = 0; i < UIP_DS6_ADDR_NB; i++) {
    state = uip_ds6_if.addr_list[i].state;
    if(uip_ds6_if.addr_list[i].isused &&
       (state == ADDR_TENTATIVE || state == ADDR_PREFERRED)) {
      //uip_debug_ipaddr_print(&uip_ds6_if.addr_list[i].ipaddr);
      printf("\n");
    }
  }
}
/*---------------------------------------------------------------------------*/
static void
sprintf_eui(void)
{
  sprintf(macs48,"%02X-%02X-%02X-%02X-%02X-%02X",
                            dev_eth_addr.addr[0],
                            dev_eth_addr.addr[1],
                            dev_eth_addr.addr[2],
                            dev_eth_addr.addr[3],
                            dev_eth_addr.addr[4],
                            dev_eth_addr.addr[5]);
}


/*---------------------------------------------------------------------------*/
/*Add callbacks here if we make them*/
static const struct mqtt_sn_callbacks mqtt_sn_call = {NULL,NULL,connack_receiver,NULL,puback_receiver,NULL};


/*---------------------------------------------------------------------------*/
PROCESS_THREAD(mqttsn_subscription_process, ev, data)
{

  static struct etimer subscription_timeout;
  static char ctrl_topic[21];//of form "0011223344556677/ctrl" it is not null terminated, and is 21 charactes

  PROCESS_BEGIN();
  etimer_set(&subscription_timeout, SEND_TIME);
  while(1) {


    PROCESS_WAIT_EVENT_UNTIL(etimer_expired(&subscription_timeout));

  }

  PROCESS_END();
}

/*---------------------------------------------------------------------------*/
PROCESS_THREAD(unicast_sender_process, ev, data)
{
  static struct etimer periodic_timer;
  static struct etimer send_timer;
  static struct ctimer mqttsn_timeout;
  static uip_ipaddr_t addr;

  static uint8_t buf_len;
  static unsigned int message_number;
  static char buf[20];
  static char ctrl_topic[25];
  static uint8_t ctrl_channel_status = 0;
  static uint8_t mqttsn_retries = 0;

  static enum mqttsn_connection_status connection_state = DISCONNECTED;
  static enum topic_registration_status registration_state = UNREGISTERED;
  static enum ctrl_subscription_status ctrl_subscription_state = CTRL_UNSUBSCRIBED;

  PROCESS_BEGIN();

  mqttsn_connack_event = process_alloc_event();
  mqttsn_regack_event = process_alloc_event();
  mqttsn_suback_event = process_alloc_event();

  //servreg_hack_init();

  set_global_address();
  mqtt_sn_set_debug(1);
  //send to TUN interface for cooja simulation
  //uip_ip6addr(&addr, 0xaaaa, 0, 0, 0, 0, 0, 0, 1);
  //uip_ip6addr(&addr, 0x2001, 0x0db8, 1, 0xffff, 0, 0, 0xc0a8, 0xd480);//192.168.212.128 with tayga
  //uip_ip6addr(&addr, 0xaaaa, 0, 2, 0xeeee, 0, 0, 0xc0a8, 0xd480);//192.168.212.128 with tayga
  //uip_ip6addr(&addr, 0xaaaa, 0, 2, 0xeeee, 0, 0, 0xac10, 0xdc01);//172.16.220.1 with tayga
  uip_ip6addr(&addr, 0xaaaa, 0, 2, 0xeeee, 0, 0, 0xac10, 0xdc80);//172.16.220.128 with tayga
  mqtt_sn_create_socket(&mqtt_sn_c,35555, &addr, UDP_PORT);
  (&mqtt_sn_c)->mc = &mqtt_sn_call;



  //connect, presume ack comes before timer fires
  //mqtt_sn_send_connect(&unicast_connection,mqtt_client_id,mqtt_keep_alive);


  /*Wait a little to let system get set*/
  etimer_set(&periodic_timer, 10*CLOCK_SECOND);
  PROCESS_WAIT_EVENT_UNTIL(etimer_expired(&periodic_timer));

  /*Request a connection and wait for connack*/
  printf("requesting connection \n ");
  for(mqttsn_retries =0; mqttsn_retries++
  mqtt_sn_send_connect(&mqtt_sn_c,mqtt_client_id,mqtt_keep_alive);

  etimer_set(&periodic_timer, 4*CLOCK_SECOND);
  PROCESS_WAIT_EVENT_UNTIL(etimer_expired(&periodic_timer));
  printf("subscribing to control channel \n ");
  while (ctrl_channel_status=0){
   mqtt_sn_send_subscribe(&mqtt_sn_c,ctrl_topic,1);
  }

//  etimer_set(&periodic_timer, 4*CLOCK_SECOND);
//  PROCESS_WAIT_EVENT_UNTIL(etimer_expired(&periodic_timer));
//  printf("registering topic\n ");

  //etimer_reset(&periodic_timer);
  etimer_set(&send_timer, SEND_TIME);
  while(1) {


    PROCESS_WAIT_EVENT_UNTIL(etimer_expired(&send_timer));
    printf("publishing \n ");
    //addr = servreg_hack_lookup(SERVICE_ID);
    //if(addr != NULL) {


//      printf("Sending unicast to ");
//      uip_debug_ipaddr_print(&addr);
//      printf("\n");
      sprintf(buf, "Message %d", message_number);
      message_number++;
      buf_len = strlen(buf);

      topic_id = (topic_name[0] << 8) + topic_name[1];

      mqtt_sn_send_publish(&mqtt_sn_c, topic_id,topic_id_type,buf, buf_len,qos,retain);
      etimer_reset(&send_timer);
      //simple_udp_sendto(&unicast_connection, buf, strlen(buf) + 1, addr);
    //} else {
     // printf("Service %d not found\n", SERVICE_ID);
   // }
  }

  PROCESS_END();
}
/*---------------------------------------------------------------------------*/

