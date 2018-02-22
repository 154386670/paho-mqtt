#ifndef __PAHO_MQTT_H__
#define __PAHO_MQTT_H__

#include <stdint.h>

#include <MQTTPacket.h>

#ifndef PKG_PAHOMQTT_SUBSCRIBE_HANDLERS 
#define MAX_MESSAGE_HANDLERS    1 /* redefinable - how many subscriptions do you want? */
#else
#define MAX_MESSAGE_HANDLERS    PKG_PAHOMQTT_SUBSCRIBE_HANDLERS
#endif

#define MAX_PACKET_ID           65535 /* according to the MQTT specification - do not change! */

enum QoS { QOS0, QOS1, QOS2 };

/* all failure return codes must be negative */
enum returnCode { PAHO_BUFFER_OVERFLOW = -2, PAHO_FAILURE = -1, PAHO_SUCCESS = 0 };

typedef struct MQTTMessage
{
    enum QoS qos;
    unsigned char retained;
    unsigned char dup;
    unsigned short id;
    void *payload;
    size_t payloadlen;
} MQTTMessage;

typedef struct MessageData
{
    MQTTMessage* message;
    MQTTString* topicName;
} MessageData;

typedef struct MQTTClient MQTTClient;

struct MQTTClient
{
    const char *host;
    int port;
    int sock;

    MQTTPacket_connectData condata;

    unsigned int next_packetid, command_timeout_ms;
    size_t buf_size, readbuf_size;
    unsigned char *buf, *readbuf;
    unsigned int keepAliveInterval;
    int isconnected;
    uint32_t tick_ping;

    void (*connect_callback) (MQTTClient*);
    void (*online_callback) (MQTTClient*);
    void (*offline_callback) (MQTTClient*);

    struct MessageHandlers
    {
        const char* topicFilter;
        void (*callback) (MQTTClient*, MessageData*);
    } messageHandlers[MAX_MESSAGE_HANDLERS]; /* Message handlers are indexed by subscription topic */

    void (*defaultMessageHandler) (MQTTClient*, MessageData*);

    /* publish interface */
#if defined(RT_USING_POSIX) && defined(RT_USING_DFS_NET)
    int pub_pipe[2];
#else
    int pub_sock;
    int pub_port;
#endif /* RT_USING_POSIX && RT_USING_DFS_NET */
};

extern int paho_mqtt_start(MQTTClient *client);
extern int MQTTPublish(MQTTClient* c, const char* topicName, MQTTMessage* message); /* copy */

#endif /* __PAHO_MQTT_H__ */
