#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SMS_PDU_MAX_SENDER_LEN 24
#define SMS_PDU_MAX_DATE_LEN   20
#define SMS_PDU_MAX_REF_LEN    8

typedef struct {
    char sender[SMS_PDU_MAX_SENDER_LEN];
    char date[SMS_PDU_MAX_DATE_LEN];
    char ref[SMS_PDU_MAX_REF_LEN];   /* concatenation reference, "" if single-part */
    uint8_t part;                    /* 1-based, 1 if single-part */
    uint8_t total_parts;             /* 1 if single-part */
    char *message;                   /* heap-allocated UTF-8 text of THIS part; caller frees */
} sms_pdu_t;

/*
 * Parses the text that follows "+CMGR:" (or one entry of a "+CMGL:" listing)
 * — i.e. the params line, a newline, then the hex-encoded PDU line.
 * Returns false on malformed input. On success, out->message is
 * heap-allocated and must be freed by the caller.
 */
bool sms_pdu_parse(const char *cmgr_body, sms_pdu_t *out);

#ifdef __cplusplus
}
#endif
