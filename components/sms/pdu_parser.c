#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "pdu_parser.h"

/*
 * NOTE ON VALIDATION: this is a port of string-based PDU parsing logic that
 * was previously exercised against real hardware in Arduino/TinyGSM form.
 * The bit-level packing (GSM7 septets, UDH offsets) is easy to get subtly
 * wrong and hard to unit test without a live modem. Log the raw hex PDU
 * (see the sms component's read path) and compare against parsed output the
 * first few times you receive real SMS on real hardware.
 */

static const char *const GSM7_LOOKUP[128] = {
    "@", "£", "$", "¥", "è", "é", "ù", "ì", "ò", "Ç", "\n", "Ø", "ø", "\r", "Å", "å",
    "Δ", "_", "Φ", "Γ", "Λ", "Ω", "Π", "Ψ", "Σ", "Θ", "Ξ", "", "Æ", "æ", "ß", "É",
    " ", "!", "\"", "#", "¤", "%", "&", "'", "(", ")", "*", "+", ",", "-", ".", "/",
    "0", "1", "2", "3", "4", "5", "6", "7", "8", "9", ":", ";", "<", "=", ">", "?",
    "¡", "A", "B", "C", "D", "E", "F", "G", "H", "I", "J", "K", "L", "M", "N", "O",
    "P", "Q", "R", "S", "T", "U", "V", "W", "X", "Y", "Z", "Ä", "Ö", "Ñ", "Ü", "§",
    "¿", "a", "b", "c", "d", "e", "f", "g", "h", "i", "j", "k", "l", "m", "n", "o",
    "p", "q", "r", "s", "t", "u", "v", "w", "x", "y", "z", "ä", "ö", "ñ", "ü", "à",
};

static const char *gsm7_escape(uint8_t code)
{
    switch (code) {
        case 0x0A: return "";
        case 0x14: return "^";
        case 0x28: return "{";
        case 0x29: return "}";
        case 0x2F: return "\\";
        case 0x3C: return "[";
        case 0x3D: return "~";
        case 0x3E: return "]";
        case 0x40: return "|";
        case 0x65: return "€";
        default:   return "";
    }
}

static inline uint8_t hex_byte(const char *p)
{
    char buf[3] = { p[0], p[1], 0 };
    return (uint8_t)strtol(buf, NULL, 16);
}

static uint32_t hex_val(const char *p, int nchars)
{
    char buf[9] = { 0 };
    if (nchars > 8) {
        nchars = 8;
    }
    memcpy(buf, p, nchars);
    return (uint32_t)strtoul(buf, NULL, 16);
}

/* GSM 7-bit packed hex -> UTF-8. Returns a heap buffer the caller must free. */
static char *parse_gsm7(const char *data, uint8_t padding_bits, uint16_t septet_count)
{
    size_t data_len = strlen(data);
    char *out = malloc((size_t)septet_count * 3 + 1);
    if (out == NULL) {
        return NULL;
    }
    size_t out_len = 0;

    bool escape = false;
    char temp = 0;
    int j = (7 - padding_bits) % 7;
    uint16_t count = 0;

    for (size_t i = 0; i + 1 < data_len && count < septet_count; i += 2) {
        uint8_t o = hex_byte(data + i);
        char s = (char)(((o << j) & 127) | temp);
        temp = (char)(o >> (7 - j));

        if (padding_bits == 0 || i != 0) {
            const char *piece;
            if (escape) {
                piece = gsm7_escape((uint8_t)s);
                escape = false;
            } else if ((uint8_t)s == 0x1B) {
                escape = true;
                piece = NULL;
            } else {
                piece = GSM7_LOOKUP[(uint8_t)s & 0x7F];
            }
            if (piece) {
                size_t plen = strlen(piece);
                memcpy(out + out_len, piece, plen);
                out_len += plen;
            }
            if (++count == septet_count) {
                break;
            }
        }
        if (++j == 7) {
            const char *piece;
            if (escape) {
                piece = gsm7_escape((uint8_t)temp);
                escape = false;
            } else if ((uint8_t)temp == 0x1B) {
                escape = true;
                piece = NULL;
            } else {
                piece = GSM7_LOOKUP[(uint8_t)temp & 0x7F];
            }
            if (piece) {
                size_t plen = strlen(piece);
                memcpy(out + out_len, piece, plen);
                out_len += plen;
            }
            temp = 0;
            j = 0;
            if (++count == septet_count) {
                break;
            }
        }
    }
    out[out_len] = '\0';
    char *shrunk = realloc(out, out_len + 1);
    return shrunk ? shrunk : out;
}

/* Hex-encoded UCS2 (with surrogate pairs) -> UTF-8. Caller must free(). */
static char *decode_unicode(const char *data)
{
    size_t data_len = strlen(data);
    char *out = malloc((data_len / 4) * 4 + 4 + 1);
    if (out == NULL) {
        return NULL;
    }
    size_t out_len = 0;
    uint16_t high_surrogate = 0;

    for (size_t i = 0; i + 3 < data_len; i += 4) {
        uint16_t u = (uint16_t)hex_val(data + i, 4);
        uint32_t codepoint;

        if (u >= 0xD800 && u < 0xDC00) {
            high_surrogate = u & 0x3FF;
            continue;
        } else if (u >= 0xDC00 && u < 0xE000) {
            codepoint = 0x10000 + (((uint32_t)high_surrogate << 10) | (u & 0x3FF));
        } else {
            codepoint = u;
        }

        uint8_t buf[4];
        int n = 0;
        if (codepoint < 0x80) {
            buf[n++] = (uint8_t)codepoint;
        } else if (codepoint < 0x800) {
            buf[n++] = 0xC0 | (codepoint >> 6);
            buf[n++] = 0x80 | (codepoint & 0x3F);
        } else if (codepoint < 0x10000) {
            buf[n++] = 0xE0 | (codepoint >> 12);
            buf[n++] = 0x80 | ((codepoint >> 6) & 0x3F);
            buf[n++] = 0x80 | (codepoint & 0x3F);
        } else {
            buf[n++] = 0xF0 | (codepoint >> 18);
            buf[n++] = 0x80 | ((codepoint >> 12) & 0x3F);
            buf[n++] = 0x80 | ((codepoint >> 6) & 0x3F);
            buf[n++] = 0x80 | (codepoint & 0x3F);
        }
        memcpy(out + out_len, buf, (size_t)n);
        out_len += (size_t)n;
    }
    out[out_len] = '\0';
    char *shrunk = realloc(out, out_len + 1);
    return shrunk ? shrunk : out;
}

/* Parses the address field at *cursor, advances *cursor past it. */
static void parse_number(const char **cursor, char *out, size_t out_size)
{
    const char *p = *cursor;
    uint8_t digit_count = hex_byte(p);
    bool is_odd = digit_count % 2;
    uint8_t byte_pairs = is_odd ? (uint8_t)(digit_count + 1) : digit_count;

    uint8_t address_type = hex_byte(p + 2) >> 4;
    const char *digits_hex = p + 4;

    *cursor = p + 4 + byte_pairs;

    if (address_type == 0x0D) {
        /* Alphanumeric sender, GSM7-packed. digit_count here counts
         * semi-octets of packed 7-bit data; recover septet count. */
        char hex_copy[64];
        size_t copy_len = byte_pairs < sizeof(hex_copy) - 1 ? byte_pairs : sizeof(hex_copy) - 1;
        memcpy(hex_copy, digits_hex, copy_len);
        hex_copy[copy_len] = '\0';
        uint16_t septets = (uint16_t)((digit_count * 4) / 7);
        char *decoded = parse_gsm7(hex_copy, 0, septets);
        if (decoded) {
            strncpy(out, decoded, out_size - 1);
            out[out_size - 1] = '\0';
            free(decoded);
        } else {
            out[0] = '\0';
        }
    } else {
        char swapped[32];
        size_t n = byte_pairs < sizeof(swapped) - 1 ? byte_pairs : sizeof(swapped) - 1;
        for (size_t i = 0; i + 1 < n; i += 2) {
            swapped[i] = digits_hex[i + 1];
            swapped[i + 1] = digits_hex[i];
        }
        swapped[n] = '\0';
        if (is_odd && n > 0 && swapped[n - 1] == 'F') {
            swapped[n - 1] = '\0';
        }
        snprintf(out, out_size, "+%s", swapped);
    }
}

static void swap_nibble_pairs(char *s, size_t len)
{
    for (size_t i = 0; i + 1 < len; i += 2) {
        char t = s[i];
        s[i] = s[i + 1];
        s[i + 1] = t;
    }
}

/* data14 = 14 hex chars = 7 octets: YY MM DD HH MM SS TZ (semi-octet swapped).
 * Timezone offset octet is parsed but currently discarded (matches prior
 * behavior) - local module time is not adjusted for it. */
static void parse_date(const char *data14, char *out, size_t out_size)
{
    char buf[15];
    memcpy(buf, data14, 14);
    buf[14] = '\0';
    swap_nibble_pairs(buf, 14);
    snprintf(out, out_size, "20%.2s/%.2s/%.2s %.2s:%.2s:%.2s",
             buf, buf + 2, buf + 4, buf + 6, buf + 8, buf + 10);
}

bool sms_pdu_parse(const char *cmgr_body, sms_pdu_t *out)
{
    if (cmgr_body == NULL || out == NULL) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    out->part = 1;
    out->total_parts = 1;

    const char *nl = strchr(cmgr_body, '\n');
    if (nl == NULL) {
        return false;
    }
    const char *p = nl + 1;
    /* trim a trailing \r if present at the end of the pdu line */
    const char *line_end = strchr(p, '\n');
    size_t pdu_len = line_end ? (size_t)(line_end - p) : strlen(p);
    if (pdu_len > 0 && p[pdu_len - 1] == '\r') {
        pdu_len--;
    }
    char pdu[600];
    if (pdu_len >= sizeof(pdu)) {
        pdu_len = sizeof(pdu) - 1;
    }
    memcpy(pdu, p, pdu_len);
    pdu[pdu_len] = '\0';

    const char *cur = pdu;

    uint8_t smsc_len = hex_byte(cur);
    cur += 2 + (size_t)smsc_len * 2;

    uint8_t tpdu = hex_byte(cur);
    bool has_header = (tpdu >> 6) & 0x01; /* TP-UDHI */
    cur += 2;

    parse_number(&cur, out->sender, sizeof(out->sender));

    uint8_t dcs = (uint8_t)hex_val(cur + 2, 2); /* TP-DCS byte */
    bool is_unicode;
    if ((dcs & 0xC0) == 0x00) {
        /* General Data Coding group: alphabet lives in bits 3-2 */
        uint8_t alphabet = (dcs >> 2) & 0x03;
        is_unicode = (alphabet == 0x02);
    } else {
        /* Other coding groups (message waiting indication, etc.) - default
         * to GSM7. Extend here if a specific carrier relies on one of
         * these less-common groups for unicode SMS. */
        is_unicode = false;
    }
    cur += 4; /* skip TP-PID + TP-DCS */

    parse_date(cur, out->date, sizeof(out->date));
    cur += 14;

    uint16_t message_septets = (uint16_t)hex_byte(cur);
    cur += 2;

    uint8_t padding = 0;
    if (has_header) {
        uint8_t header_octets = hex_byte(cur); /* UDHL, excludes itself */
        const char *udh_end = cur + 2 + (size_t)header_octets * 2;
        const char *ie = cur + 2;

        while (ie + 4 <= udh_end) {
            uint8_t iei = hex_byte(ie);
            uint8_t iedl = hex_byte(ie + 2);
            const char *ie_data = ie + 4;

            if (iei == 0x00 && iedl == 3) {
                memcpy(out->ref, ie_data, 2);
                out->ref[2] = '\0';
                out->total_parts = hex_byte(ie_data + 2);
                out->part = hex_byte(ie_data + 4);
            } else if (iei == 0x08 && iedl == 4) {
                memcpy(out->ref, ie_data, 4);
                out->ref[4] = '\0';
                out->total_parts = hex_byte(ie_data + 4);
                out->part = hex_byte(ie_data + 6);
            }
            /* Unknown/unhandled IEs: skip and continue - per spec, a
             * receiving entity ignores IEs it doesn't support and moves
             * on to the next one. */
            ie += 4 + (size_t)iedl * 2;
        }
        cur = udh_end;

        if (!is_unicode) {
            uint16_t udh_bits = (uint16_t)((header_octets + 1) * 8);
            padding = (uint8_t)((7 - (udh_bits % 7)) % 7);
            message_septets -= (udh_bits / 7) + (padding > 0 ? 1 : 0);
        }
    }

    out->message = is_unicode ? decode_unicode(cur) : parse_gsm7(cur, padding, message_septets);
    return out->message != NULL;
}
