/* coreMQTT for hello-mqtt: errors and warnings to USB serial. */
#ifndef CORE_MQTT_CONFIG_H
#define CORE_MQTT_CONFIG_H

#include <stdio.h>

#define LogError(message) do { printf("coreMQTT error: "); printf message; printf("\n"); } while (0)
#define LogWarn(message) do { printf("coreMQTT warning: "); printf message; printf("\n"); } while (0)
#define LogInfo(message)
#define LogDebug(message)

#define MQTT_RECV_POLLING_TIMEOUT_MS 2000U
#define MQTT_SEND_TIMEOUT_MS 5000U
#define MQTT_PINGRESP_TIMEOUT_MS 10000U

#endif
