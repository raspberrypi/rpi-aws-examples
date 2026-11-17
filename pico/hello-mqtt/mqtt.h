/* MQTT 3.1.1 to AWS IoT Core: coreMQTT over lwIP's TLS (altcp_tls and mbedTLS), polled from the main loop. */
#ifndef MQTT_H
#define MQTT_H

#include <stdbool.h>
#include <stddef.h>

/* Called for each message received on a subscribed topic. */
typedef void (*mqtt_message_fn)(const char *topic, size_t topic_len, const char *payload, size_t len);

/* Connects as client_id (the thing name) to endpoint:8883, then subscribes to subscribe_topic.
 * Returns 0, or a negative step code: -1 DNS, -2 TLS, -3 MQTT CONNECT, -4 SUBSCRIBE. */
int mqtt_connect(const char *endpoint, const char *client_id, const char *cert_pem, const char *subscribe_topic,
                 mqtt_message_fn on_message);
bool mqtt_publish(const char *topic, const char *payload, size_t len);
/* Receives and keeps the connection alive. Returns false once it has been lost. */
bool mqtt_poll(void);
void mqtt_disconnect(void);

#endif
