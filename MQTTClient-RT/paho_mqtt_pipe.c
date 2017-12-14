#include <string.h>
#include <stdint.h>

#include <rtthread.h>
#include <rtdevice.h>
#include <dfs_posix.h>

#include <netdb.h>
#include <sys/socket.h>
#include <sys/select.h>

#include "MQTTPacket.h"
#include "paho_mqtt.h"

#define  debug_printf     rt_kprintf("[MQTT] ");rt_kprintf
//#define  debug_printf(...)

#define malloc      rt_malloc
#define free        rt_free

static int net_connect(MQTTClient *c)
{
    struct hostent *host = 0;
    struct sockaddr_in sockaddr;
    int rc = -1;

    c->sock = -1;
    c->next_packetid = 0;

    host = gethostbyname(c->host);
    if (host == 0)
    {
        debug_printf("gethostbyname(%s) error!\n", c->host);
        goto _exit;
    }

    sockaddr.sin_family = AF_INET;
    sockaddr.sin_port = htons(c->port);
    sockaddr.sin_addr = *((struct in_addr *)host->h_addr);
    memset(&(sockaddr.sin_zero), 0, sizeof(sockaddr.sin_zero));

    if ((c->sock = socket(AF_INET, SOCK_STREAM, 0)) == -1)
    {
        debug_printf("create socket error!\n");
        goto _exit;
    }

    if ((rc = connect(c->sock, (struct sockaddr *)&sockaddr, sizeof(struct sockaddr))) == -1)
    {
        debug_printf("connect %s:%d error!\n", c->host, c->port);
        closesocket(c->sock);
        return -2;
    }

_exit:
    return rc;
}

static int net_disconnect(MQTTClient *c)
{
    if (c->sock >= 0)
    {
        closesocket(c->sock);
        c->sock = -1;
    }

    return 0;
}

static int sendPacket(MQTTClient *c, int length)
{
    int rc;
    struct timeval tv;

    tv.tv_sec = 2000;
    tv.tv_usec = 0;

    setsockopt(c->sock, SOL_SOCKET, SO_SNDTIMEO, (char *)&tv, sizeof(struct timeval));
    rc = send(c->sock, c->buf, length, 0);
    if (rc == length)
    {
        rc = 0;
    }
    else
    {
        rc = -1;
    }

    return rc;
}

static int net_read(MQTTClient *c, unsigned char *buf,  int len, int timeout)
{
    int bytes = 0;
    int rc;

    while (bytes < len)
    {
        rc = recv(c->sock, &buf[bytes], (size_t)(len - bytes), MSG_DONTWAIT);

        if (rc == -1)
        {
            if (errno != ENOTCONN && errno != ECONNRESET)
            {
                bytes = -1;
                break;
            }
        }
        else
            bytes += rc;

        if (bytes >= len)
        {
            break;
        }

        if (timeout > 0)
        {
            fd_set readset;
            struct timeval interval;

            debug_printf("net_read %d:%d, timeout:%d\n", bytes, len, timeout);
            timeout  = 0;

            interval.tv_sec = 1;
            interval.tv_usec = 0;

            FD_ZERO(&readset);
            FD_SET(c->sock, &readset);

            select(c->sock + 1, &readset, RT_NULL, RT_NULL, &interval);
        }
        else
        {
            debug_printf("net_read %d:%d, break!\n", bytes, len);
            break;
        }
    }

    return bytes;
}

static int decodePacket(MQTTClient *c, int *value, int timeout)
{
    unsigned char i;
    int multiplier = 1;
    int len = 0;
    const int MAX_NO_OF_REMAINING_LENGTH_BYTES = 4;

    *value = 0;
    do
    {
        int rc = MQTTPACKET_READ_ERROR;

        if (++len > MAX_NO_OF_REMAINING_LENGTH_BYTES)
        {
            rc = MQTTPACKET_READ_ERROR; /* bad data */
            goto exit;
        }
        rc = net_read(c, &i, 1, timeout);
        if (rc != 1)
            goto exit;
        *value += (i & 127) * multiplier;
        multiplier *= 128;
    }
    while ((i & 128) != 0);
exit:
    return len;
}

static int MQTTPacket_readPacket(MQTTClient *c)
{
    int rc = FAILURE;
    MQTTHeader header = {0};
    int len = 0;
    int rem_len = 0;

    /* 1. read the header byte.  This has the packet type in it */
    if (net_read(c, c->readbuf, 1, 0) != 1)
        goto exit;

    len = 1;
    /* 2. read the remaining length.  This is variable in itself */
    decodePacket(c, &rem_len, 50);
    len += MQTTPacket_encode(c->readbuf + 1, rem_len); /* put the original remaining length back into the buffer */

    /* 3. read the rest of the buffer using a callback to supply the rest of the data */
    if (rem_len > 0 && (net_read(c, c->readbuf + len, rem_len, 300) != rem_len))
        goto exit;

    header.byte = c->readbuf[0];
    rc = header.bits.type;

exit:
    return rc;
}

static int getNextPacketId(MQTTClient *c)
{
    return c->next_packetid = (c->next_packetid == MAX_PACKET_ID) ? 1 : c->next_packetid + 1;
}

static int MQTTConnect(MQTTClient *c)
{
    int rc = -1, len;
    MQTTPacket_connectData *options = &c->condata;

    if (c->isconnected) /* don't send connect packet again if we are already connected */
        goto _exit;

    c->keepAliveInterval = options->keepAliveInterval;

    if ((len = MQTTSerialize_connect(c->buf, c->buf_size, options)) <= 0)
        goto _exit;

    if ((rc = sendPacket(c, len)) != 0)  // send the connect packet
        goto _exit; // there was a problem

    {
        int res;
        fd_set readset;
        struct timeval timeout;

        timeout.tv_sec = 5;
        timeout.tv_usec = 0;

        FD_ZERO(&readset);
        FD_SET(c->sock, &readset);

        res = select(c->sock + 1, &readset, RT_NULL, RT_NULL, &timeout);

        if (res <= 0)
        {
            debug_printf("%s wait resp fail, res:%d errno:%d\n", __FUNCTION__, res, errno);
            rc = -1;
            goto _exit;
        }
    }

    rc = MQTTPacket_readPacket(c);
    if (rc < 0)
    {
        debug_printf("%s MQTTPacket_readPacket fail\n", __FUNCTION__);
        goto _exit;
    }

    if (rc == CONNACK)
    {
        unsigned char sessionPresent, connack_rc;

        if (MQTTDeserialize_connack(&sessionPresent, &connack_rc, c->readbuf, c->readbuf_size) == 1)
        {
            rc = connack_rc;
        }
        else
        {
            rc = -1;
        }
    }
    else
        rc = -1;

_exit:
    if (rc == 0)
        c->isconnected = 1;

    return rc;
}

static int MQTTDisconnect(MQTTClient *c)
{
    int rc = FAILURE;
    int len = 0;

    len = MQTTSerialize_disconnect(c->buf, c->buf_size);
    if (len > 0)
        rc = sendPacket(c, len);            // send the disconnect packet

    c->isconnected = 0;

    return rc;
}

static int MQTTSubscribe(MQTTClient *c, const char *topicFilter, enum QoS qos)
{
    int rc = FAILURE;
    int len = 0;
    MQTTString topic = MQTTString_initializer;
    topic.cstring = (char *)topicFilter;

    if (!c->isconnected)
        goto _exit;

    len = MQTTSerialize_subscribe(c->buf, c->buf_size, 0, getNextPacketId(c), 1, &topic, (int *)&qos);
    if (len <= 0)
        goto _exit;
    if ((rc = sendPacket(c, len)) != SUCCESS) // send the subscribe packet
        goto _exit;             // there was a problem

    {
        int res;
        fd_set readset;
        struct timeval timeout;

        timeout.tv_sec = 5;
        timeout.tv_usec = 0;

        FD_ZERO(&readset);
        FD_SET(c->sock, &readset);

        res = select(c->sock + 1, &readset, RT_NULL, RT_NULL, &timeout);

        if (res <= 0)
        {
            debug_printf("%s wait resp fail, res:%d errno:%d\n", __FUNCTION__, res, errno);
            rc = -1;
            goto _exit;
        }
    }

    rc = MQTTPacket_readPacket(c);
    if (rc < 0)
    {
        debug_printf("MQTTPacket_readPacket MQTTConnect fail\n");
        goto _exit;
    }

    if (rc == SUBACK)      // wait for suback
    {
        int count = 0, grantedQoS = -1;
        unsigned short mypacketid;

        if (MQTTDeserialize_suback(&mypacketid, 1, &count, &grantedQoS, c->readbuf, c->readbuf_size) == 1)
            rc = grantedQoS; // 0, 1, 2 or 0x80

        if (rc != 0x80)
        {
            rc = 0;
        }
    }
    else
        rc = FAILURE;

_exit:
    return rc;
}

static void NewMessageData(MessageData *md, MQTTString *aTopicName, MQTTMessage *aMessage)
{
    md->topicName = aTopicName;
    md->message = aMessage;
}

// assume topic filter and name is in correct format
// # can only be at end
// + and # can only be next to separator
static char isTopicMatched(char *topicFilter, MQTTString *topicName)
{
    char *curf = topicFilter;
    char *curn = topicName->lenstring.data;
    char *curn_end = curn + topicName->lenstring.len;

    while (*curf && curn < curn_end)
    {
        if (*curn == '/' && *curf != '/')
            break;
        if (*curf != '+' && *curf != '#' && *curf != *curn)
            break;
        if (*curf == '+')
        {
            // skip until we meet the next separator, or end of string
            char *nextpos = curn + 1;
            while (nextpos < curn_end && *nextpos != '/')
                nextpos = ++curn + 1;
        }
        else if (*curf == '#')
            curn = curn_end - 1;    // skip until end of string
        curf++;
        curn++;
    };

    return (curn == curn_end) && (*curf == '\0');
}

static int deliverMessage(MQTTClient *c, MQTTString *topicName, MQTTMessage *message)
{
    int i;
    int rc = FAILURE;

    // we have to find the right message handler - indexed by topic
    for (i = 0; i < MAX_MESSAGE_HANDLERS; ++i)
    {
        if (c->messageHandlers[i].topicFilter != 0 && (MQTTPacket_equals(topicName, (char *)c->messageHandlers[i].topicFilter) ||
                isTopicMatched((char *)c->messageHandlers[i].topicFilter, topicName)))
        {
            if (c->messageHandlers[i].callback != NULL)
            {
                MessageData md;
                NewMessageData(&md, topicName, message);
                c->messageHandlers[i].callback(c, &md);
                rc = SUCCESS;
            }
        }
    }

    if (rc == FAILURE && c->defaultMessageHandler != NULL)
    {
        MessageData md;
        NewMessageData(&md, topicName, message);
        c->defaultMessageHandler(c, &md);
        rc = SUCCESS;
    }

    return rc;
}

static int MQTT_cycle(MQTTClient *c)
{
    // read the socket, see what work is due
    int packet_type = MQTTPacket_readPacket(c);

    int len = 0,
        rc = SUCCESS;


    if (packet_type == -1)
    {
        rc = FAILURE;
        goto exit;
    }

    switch (packet_type)
    {
    case CONNACK:
    case PUBACK:
    case SUBACK:
        break;
    case PUBLISH:
    {
        MQTTString topicName;
        MQTTMessage msg;
        int intQoS;
        if (MQTTDeserialize_publish(&msg.dup, &intQoS, &msg.retained, &msg.id, &topicName,
                                    (unsigned char **)&msg.payload, (int *)&msg.payloadlen, c->readbuf, c->readbuf_size) != 1)
            goto exit;
        msg.qos = (enum QoS)intQoS;
        deliverMessage(c, &topicName, &msg);
        if (msg.qos != QOS0)
        {
            if (msg.qos == QOS1)
                len = MQTTSerialize_ack(c->buf, c->buf_size, PUBACK, 0, msg.id);
            else if (msg.qos == QOS2)
                len = MQTTSerialize_ack(c->buf, c->buf_size, PUBREC, 0, msg.id);
            if (len <= 0)
                rc = FAILURE;
            else
                rc = sendPacket(c, len);
            if (rc == FAILURE)
                goto exit; // there was a problem
        }
        break;
    }
    case PUBREC:
    {
        unsigned short mypacketid;
        unsigned char dup, type;
        if (MQTTDeserialize_ack(&type, &dup, &mypacketid, c->readbuf, c->readbuf_size) != 1)
            rc = FAILURE;
        else if ((len = MQTTSerialize_ack(c->buf, c->buf_size, PUBREL, 0, mypacketid)) <= 0)
            rc = FAILURE;
        else if ((rc = sendPacket(c, len)) != SUCCESS) // send the PUBREL packet
            rc = FAILURE; // there was a problem
        if (rc == FAILURE)
            goto exit; // there was a problem
        break;
    }
    case PUBCOMP:
        break;
    case PINGRESP:
        c->tick_ping = rt_tick_get();
        break;
    }

exit:
    return rc;
}

static void paho_mqtt_thread(void *param)
{
    MQTTClient *c = (MQTTClient *)param;
    int i, rc, len;
    int rc_t = 0;

    /* create publish pipe. */
    if (pipe(c->pub_pipe) != 0)
    {
        debug_printf("creat pipe err\n");
        goto _mqtt_exit;
    }

_mqtt_start:
    if (c->connect_callback)
    {
        c->connect_callback(c);
    }

    rc = net_connect(c);
    if (rc != 0)
    {
        goto _mqtt_restart;
    }

    rc = MQTTConnect(c);
    if (rc != 0)
    {
        goto _mqtt_restart;
    }

    for (i = 0; i < MAX_MESSAGE_HANDLERS; i++)
    {
        const char *topic = c->messageHandlers[i].topicFilter;

        rc = MQTTSubscribe(c, topic, QOS2);
        debug_printf("Subscribe #%d %s %s!\n", i, topic, (rc < 0) ? ("fail") : ("OK"));

        if (rc != 0)
        {
            goto _mqtt_disconnect;
        }
    }

    if (c->online_callback)
    {
        c->online_callback(c);
    }

    c->tick_ping = rt_tick_get();
    while (1)
    {
        int res;
        rt_tick_t tick_now;
        fd_set readset;
        struct timeval timeout;

        tick_now = rt_tick_get();
        if (((tick_now - c->tick_ping) / RT_TICK_PER_SECOND) > (c->keepAliveInterval - 5))
        {
            timeout.tv_sec = 1;
            //debug_printf("tick close to ping.\n");
        }
        else
        {
            timeout.tv_sec = c->keepAliveInterval - 10 - (tick_now - c->tick_ping) / RT_TICK_PER_SECOND;
            //debug_printf("timeount for ping: %d\n", timeout.tv_sec);
        }
        timeout.tv_usec = 0;

        FD_ZERO(&readset);
        FD_SET(c->sock, &readset);
        FD_SET(c->pub_pipe[0], &readset);

        /* int select(maxfdp1, readset, writeset, exceptset, timeout); */
        res = select(((c->pub_pipe[0] > c->sock) ? c->pub_pipe[0] : c->sock) + 1,
                     &readset, RT_NULL, RT_NULL, &timeout);
        if (res == 0)
        {
            len = MQTTSerialize_pingreq(c->buf, c->buf_size);
            rc = sendPacket(c, len);
            if (rc != 0)
            {
                debug_printf("[%d] send ping rc: %d \n", rt_tick_get(), rc);
                goto _mqtt_disconnect;
            }

            /* wait Ping Response. */
            timeout.tv_sec = 5;
            timeout.tv_usec = 0;

            FD_ZERO(&readset);
            FD_SET(c->sock, &readset);

            res = select(c->sock + 1, &readset, RT_NULL, RT_NULL, &timeout);
            if (res <= 0)
            {
                debug_printf("[%d] wait Ping Response res: %d\n", rt_tick_get(), res);
                goto _mqtt_disconnect;
            }
        } /* res == 0: timeount for ping. */

        if (res < 0)
        {
            debug_printf("select res: %d\n", res);
            goto _mqtt_disconnect;
        }

        if (FD_ISSET(c->sock, &readset))
        {
            //debug_printf("sock FD_ISSET\n");
            rc_t = MQTT_cycle(c);
            //debug_printf("sock FD_ISSET rc_t : %d\n", rc_t);
            if (rc_t < 0)    goto _mqtt_disconnect;

            continue;
        }

        if (FD_ISSET(c->pub_pipe[0], &readset))
        {
            MQTTMessage *message;
            MQTTString topic = MQTTString_initializer;

            //debug_printf("pub_sock FD_ISSET\n");

            len = read(c->pub_pipe[0], c->readbuf, c->readbuf_size);

            if (len < sizeof(MQTTMessage))
            {
                c->readbuf[len] = '\0';
                debug_printf("pub_sock recv %d byte: %s\n", len, c->readbuf);

                if (strcmp((const char *)c->readbuf, "DISCONNECT") == 0)
                {
                    debug_printf("DISCONNECT\n");
                    goto _mqtt_disconnect_exit;
                }

                continue;
            }

            message = (MQTTMessage *)c->readbuf;
            message->payload = c->readbuf + sizeof(MQTTMessage);
            topic.cstring = (char *)c->readbuf + sizeof(MQTTMessage) + message->payloadlen;
            //debug_printf("pub_sock topic:%s, payloadlen:%d\n", topic.cstring, message->payloadlen);

            len = MQTTSerialize_publish(c->buf, c->buf_size, 0, message->qos, message->retained, message->id,
                                        topic, (unsigned char *)message->payload, message->payloadlen);
            if (len <= 0)
            {
                debug_printf("MQTTSerialize_publish len: %d\n", len);
                goto _mqtt_disconnect;
            }

            if ((rc = sendPacket(c, len)) != SUCCESS) // send the subscribe packet
            {
                debug_printf("MQTTSerialize_publish sendPacket rc: %d\n", rc);
                goto _mqtt_disconnect;
            }
        } /* pbulish sock handler. */
    } /* while (1) */

_mqtt_disconnect:
    MQTTDisconnect(c);
_mqtt_restart:
    if (c->offline_callback)
    {
        c->offline_callback(c);
    }

    net_disconnect(c);
    rt_thread_delay(RT_TICK_PER_SECOND * 5);
    debug_printf("restart!\n");
    goto _mqtt_start;

_mqtt_disconnect_exit:
    MQTTDisconnect(c);
    net_disconnect(c);

_mqtt_exit:
    debug_printf("thread exit\n");

    return;
}

int paho_mqtt_start(MQTTClient *client)
{
    rt_err_t result;
    rt_thread_t tid;
    int stack_size = 1024 * 4;
    int priority = RT_THREAD_PRIORITY_MAX / 3;
    char *stack;

    tid = malloc(RT_ALIGN(sizeof(struct rt_thread), 8) + stack_size);
    if (!tid)
    {
        debug_printf("no memory for thread: MQTT\n");
        return -1;
    }

    stack = (char *)tid + RT_ALIGN(sizeof(struct rt_thread), 8);
    result = rt_thread_init(tid,
                            "MQTT",
                            paho_mqtt_thread, client, // fun, parameter
                            stack, stack_size,   // stack, size
                            priority, 2          //priority, tick
                           );

    if (result == RT_EOK)
    {
        rt_thread_startup(tid);
    }

    return 0;
}

static int MQTT_local_send(MQTTClient *c, const void *data, int len)
{
    int send_len;

    send_len = write(c->pub_pipe[1], data, len);

    return send_len;
}

/*
MQTT_CMD:
"DISCONNECT"
*/
int MQTT_CMD(MQTTClient *c, const char *cmd)
{
    char *data = 0;
    int cmd_len, len;
    int rc = FAILURE;

    if (!c->isconnected)
        goto _exit;

    cmd_len = strlen(cmd) + 1;
    if (cmd_len >= sizeof(MQTTMessage))
    {
        debug_printf("cmd too loog %d:\n", cmd_len, sizeof(MQTTMessage));
        goto _exit;
    }

    data = malloc(cmd_len);
    if (!data)
        goto _exit;

    strcpy(data, cmd);
    len = MQTT_local_send(c, data, cmd_len);
    if (len == cmd_len)
    {
        rc = 0;
    }

_exit:
    if (data)
        free(data);

    return rc;
}

/*
[MQTTMessage] + [payload] + [topic] + '\0'
*/
int MQTTPublish(MQTTClient *c, const char *topicName, MQTTMessage *message)
{
    int rc = FAILURE;
    int len, msg_len;
    char *data = 0;

    if (!c->isconnected)
        goto exit;

    msg_len = sizeof(MQTTMessage) + message->payloadlen + strlen(topicName) + 1;
    data = malloc(msg_len);
    if (!data)
        goto exit;

    memcpy(data, message, sizeof(MQTTMessage));
    memcpy(data + sizeof(MQTTMessage), message->payload, message->payloadlen);
    strcpy(data + sizeof(MQTTMessage) + message->payloadlen, topicName);

    len = MQTT_local_send(c, data, msg_len);
    if (len == msg_len)
    {
        rc = 0;
    }
    //debug_printf("MQTTPublish sendto %d\n", len);

exit:
    if (data)
        free(data);

    return rc;
}
