/*
 * log_stream.h
 */

#ifndef INC_LOG_STREAM_H_
#define INC_LOG_STREAM_H_

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdbool.h>

#define LOG_STREAM_FRAME_MAX           256u
#define LOG_STREAM_PROTO_VER           2u

#define LOG_OPCODE_PING                0x01u
#define LOG_OPCODE_GET_INFO            0x02u
#define LOG_OPCODE_START_DUMP          0x10u
#define LOG_OPCODE_STOP_DUMP           0x11u
#define LOG_OPCODE_GET_DUMP_STATUS     0x12u

#define LOG_STATUS_OK                  0u
#define LOG_STATUS_NOT_INIT            1u
#define LOG_STATUS_BAD_PARAM           2u
#define LOG_STATUS_OUT_OF_RANGE        3u
#define LOG_STATUS_BUSY                4u
#define LOG_STATUS_FORBIDDEN           5u
#define LOG_STATUS_INTERNAL            6u

#define LOG_TIER_CRITICAL              0u
#define LOG_TIER_GENERAL               1u
#define LOG_TIER_BOTH                  2u

#define LOG_STREAM_FLAG_FIRST          0x01u
#define LOG_STREAM_FLAG_LAST           0x02u
#define LOG_STREAM_FLAG_TIER_CHANGE    0x04u
#define LOG_STREAM_FLAG_ERROR          0x80u

typedef uint8_t LogTransportPort_t;
#define LOG_PORT_UART2                 0u
#define LOG_PORT_UART4                 1u

typedef void (*LogStream_SendFn)(LogTransportPort_t port,
                                 uint16_t pkt_type,
                                 uint16_t seq,
                                 const uint8_t *payload,
                                 uint16_t payload_len);

void LogStream_Init(LogStream_SendFn send_fn);
void LogStream_HandleRequest(LogTransportPort_t port,
                             uint16_t seq,
                             const uint8_t *payload,
                             uint16_t payload_len);
void LogStream_Process(void);
bool LogStream_IsDumpActive(void);

#ifdef __cplusplus
}
#endif

#endif /* INC_LOG_STREAM_H_ */
