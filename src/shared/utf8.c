/*-*- Mode: C; c-basic-offset: 8; indent-tabs-mode: nil -*-*/

/***
  This file is part of systemd.

  Copyright 2012 Lennart Poettering

  systemd is free software; you can redistribute it and/or modify it
  under the terms of the GNU Lesser General Public License as published by
  the Free Software Foundation; either version 2.1 of the License, or
  (at your option) any later version.

  systemd is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public License
  along with systemd; If not, see <http://www.gnu.org/licenses/>.
***/

/* This file is based on the GLIB utf8 validation functions. The
 * original license text follows. */

/* gutf8.c - Operations on UTF-8 strings.
 *
 * Copyright (C) 1999 Tom Tromey
 * Copyright (C) 2000 Red Hat, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

#include <errno.h>
#include <stdlib.h>
#include <inttypes.h>
#include <string.h>
#include <stdbool.h>

#include "utf8.h"
#include "util.h"

#define FILTER_CHAR '_'

static inline bool is_unicode_valid(uint32_t ch) {

        if (ch >= 0x110000) /* End of unicode space */
                return false;
        if ((ch & 0xFFFFF800) == 0xD800) /* Reserved area for UTF-16 */
                return false;
        if ((ch >= 0xFDD0) && (ch <= 0xFDEF)) /* Reserved */
                return false;
        if ((ch & 0xFFFE) == 0xFFFE) /* BOM (Byte Order Mark) */
                return false;

        return true;
}

static inline bool is_continuation_char(uint8_t ch) {
        if ((ch & 0xc0) != 0x80) /* 10xxxxxx */
                return false;
        return true;
}

static inline void merge_continuation_char(uint32_t *u_ch, uint8_t ch) {
        *u_ch <<= 6;
        *u_ch |= ch & 0x3f;
}

static bool is_unicode_control(uint32_t ch) {

        /*
          0 to ' '-1 is the C0 range.
          DEL=0x7F, and DEL+1 to 0x9F is C1 range.
          '\t' is in C0 range, but more or less harmless and commonly used.
        */

        return (ch < ' ' && ch != '\t' && ch != '\n') ||
                (0x7F <= ch && ch <= 0x9F);
}

bool utf8_is_printable(const char* str, size_t length) {
        uint32_t val = 0;
        uint32_t min = 0;
        const uint8_t *p;

        assert(str);

        for (p = (const uint8_t*) str; length; p++, length--) {
                if (*p < 128) {
                        val = *p;
                } else {
                        if ((*p & 0xe0) == 0xc0) { /* 110xxxxx two-char seq. */
                                min = 128;
                                val = (uint32_t) (*p & 0x1e);
                                goto ONE_REMAINING;
                        } else if ((*p & 0xf0) == 0xe0) { /* 1110xxxx three-char seq.*/
                                min = (1 << 11);
                                val = (uint32_t) (*p & 0x0f);
                                goto TWO_REMAINING;
                        } else if ((*p & 0xf8) == 0xf0) { /* 11110xxx four-char seq */
                                min = (1 << 16);
                                val = (uint32_t) (*p & 0x07);
                        } else
                                return false;

                        p++;
                        length--;
                        if (!length || !is_continuation_char(*p))
                                return false;
                        merge_continuation_char(&val, *p);

                TWO_REMAINING:
                        p++;
                        length--;
                        if (!is_continuation_char(*p))
                                return false;
                        merge_continuation_char(&val, *p);

                ONE_REMAINING:
                        p++;
                        length--;
                        if (!is_continuation_char(*p))
                                return false;
                        merge_continuation_char(&val, *p);

                        if (val < min)
                                return false;
                }

                if (is_unicode_control(val))
                        return false;
        }

        return true;
}

static char* utf8_validate(const char *str, char *output) {
        uint32_t val = 0;
        uint32_t min = 0;
        const uint8_t *p, *last;
        int size;
        uint8_t *o;

        assert(str);

        o = (uint8_t*) output;
        for (p = (const uint8_t*) str; *p; p++) {
                if (*p < 128) {
                        if (o)
                                *o = *p;
                } else {
                        last = p;

                        if ((*p & 0xe0) == 0xc0) { /* 110xxxxx two-char seq. */
                                size = 2;
                                min = 128;
                                val = (uint32_t) (*p & 0x1e);
                                goto ONE_REMAINING;
                        } else if ((*p & 0xf0) == 0xe0) { /* 1110xxxx three-char seq.*/
                                size = 3;
                                min = (1 << 11);
                                val = (uint32_t) (*p & 0x0f);
                                goto TWO_REMAINING;
                        } else if ((*p & 0xf8) == 0xf0) { /* 11110xxx four-char seq */
                                size = 4;
                                min = (1 << 16);
                                val = (uint32_t) (*p & 0x07);
                        } else
                                goto error;

                        p++;
                        if (!is_continuation_char(*p))
                                goto error;
                        merge_continuation_char(&val, *p);

                TWO_REMAINING:
                        p++;
                        if (!is_continuation_char(*p))
                                goto error;
                        merge_continuation_char(&val, *p);

                ONE_REMAINING:
                        p++;
                        if (!is_continuation_char(*p))
                                goto error;
                        merge_continuation_char(&val, *p);

                        if (val < min)
                                goto error;

                        if (!is_unicode_valid(val))
                                goto error;

                        if (o) {
                                memcpy(o, last, (size_t) size);
                                o += size;
                        }

                        continue;

                error:
                        if (o) {
                                *o = FILTER_CHAR;
                                p = last; /* We retry at the next character */
                        } else
                                goto failure;
                }

                if (o)
                        o++;
        }

        if (o) {
                *o = '\0';
                return output;
        }

        return (char*) str;

failure:
        return NULL;
}

char* utf8_is_valid (const char *str) {
        return utf8_validate(str, NULL);
}

char* utf8_filter (const char *str) {
        char *new_str;

        assert(str);

        new_str = malloc(strlen(str) + 1);
        if (!new_str)
                return NULL;

        return utf8_validate(str, new_str);
}

char *ascii_is_valid(const char *str) {
        const char *p;

        assert(str);

        for (p = str; *p; p++)
                if ((unsigned char) *p >= 128)
                        return NULL;

        return (char*) str;
}

char *ascii_filter(const char *str) {
        const char *s;
        char *r, *d;
        size_t l;

        assert(str);

        l = strlen(str);
        r = malloc(l + 1);
        if (!r)
                return NULL;

        for (s = str, d = r; *s; s++)
                if ((unsigned char) *s < 128)
                        *(d++) = *s;

        *d = 0;

        return r;
}

char *utf16_to_utf8(const void *s, size_t length) {
        char *r;
        const uint8_t *f;
        uint8_t *t;

        r = new(char, (length*3+1)/2 + 1);
        if (!r)
                return NULL;

        t = (uint8_t*) r;

        for (f = s; f < (const uint8_t*) s + length; f += 2) {
                uint16_t c;

                c = (f[1] << 8) | f[0];

                if (c == 0) {
                        *t = 0;
                        return r;
                } else if (c < 0x80) {
                        *(t++) = (uint8_t) c;
                } else if (c < 0x800) {
                        *(t++) = (uint8_t) (0xc0 | (c >> 6));
                        *(t++) = (uint8_t) (0x80 | (c & 0x3f));
                } else {
                        *(t++) = (uint8_t) (0xe0 | (c >> 12));
                        *(t++) = (uint8_t) (0x80 | ((c >> 6) & 0x3f));
                        *(t++) = (uint8_t) (0x80 | (c & 0x3f));
                }
        }

        *t = 0;

        return r;
}

/* count of characters used to encode one unicode char */
static int utf8_encoded_expected_len(const char *str) {
        unsigned char c = (unsigned char)str[0];

        if (c < 0x80)
                return 1;
        if ((c & 0xe0) == 0xc0)
                return 2;
        if ((c & 0xf0) == 0xe0)
                return 3;
        if ((c & 0xf8) == 0xf0)
                return 4;
        if ((c & 0xfc) == 0xf8)
                return 5;
        if ((c & 0xfe) == 0xfc)
                return 6;
        return 0;
}

/* decode one unicode char */
static int utf8_encoded_to_unichar(const char *str) {
        int unichar;
        int len;
        int i;

        len = utf8_encoded_expected_len(str);
        switch (len) {
        case 1:
                return (int)str[0];
        case 2:
                unichar = str[0] & 0x1f;
                break;
        case 3:
                unichar = (int)str[0] & 0x0f;
                break;
        case 4:
                unichar = (int)str[0] & 0x07;
                break;
        case 5:
                unichar = (int)str[0] & 0x03;
                break;
        case 6:
                unichar = (int)str[0] & 0x01;
                break;
        default:
                return -1;
        }

        for (i = 1; i < len; i++) {
                if (((int)str[i] & 0xc0) != 0x80)
                        return -1;
                unichar <<= 6;
                unichar |= (int)str[i] & 0x3f;
        }

        return unichar;
}

/* expected size used to encode one unicode char */
static int utf8_unichar_to_encoded_len(int unichar) {
        if (unichar < 0x80)
                return 1;
        if (unichar < 0x800)
                return 2;
        if (unichar < 0x10000)
                return 3;
        if (unichar < 0x200000)
                return 4;
        if (unichar < 0x4000000)
                return 5;
        return 6;
}

/* validate one encoded unicode char and return its length */
int utf8_encoded_valid_unichar(const char *str) {
        int len;
        int unichar;
        int i;

        len = utf8_encoded_expected_len(str);
        if (len == 0)
                return -1;

        /* ascii is valid */
        if (len == 1)
                return 1;

        /* check if expected encoded chars are available */
        for (i = 0; i < len; i++)
                if ((str[i] & 0x80) != 0x80)
                        return -1;

        unichar = utf8_encoded_to_unichar(str);

        /* check if encoded length matches encoded value */
        if (utf8_unichar_to_encoded_len(unichar) != len)
                return -1;

        /* check if value has valid range */
        if (!is_unicode_valid(unichar))
                return -1;

        return len;
}

int is_utf8_encoding_whitelisted(char c, const char *white) {
        if ((c >= '0' && c <= '9') ||
            (c >= 'A' && c <= 'Z') ||
            (c >= 'a' && c <= 'z') ||
            strchr("#+-.:=@_", c) != NULL ||
            (white != NULL && strchr(white, c) != NULL))
                return 1;
        return 0;
}

int udev_encode_string(const char *str, char *str_enc, size_t len) {
        size_t i, j;

        if (str == NULL || str_enc == NULL)
                return -1;

        for (i = 0, j = 0; str[i] != '\0'; i++) {
                int seqlen;

                seqlen = utf8_encoded_valid_unichar(&str[i]);
                if (seqlen > 1) {
                        if (len-j < (size_t)seqlen)
                                goto err;
                        memcpy(&str_enc[j], &str[i], seqlen);
                        j += seqlen;
                        i += (seqlen-1);
                } else if (str[i] == '\\' || !is_utf8_encoding_whitelisted(str[i], NULL)) {
                        if (len-j < 4)
                                goto err;
                        sprintf(&str_enc[j], "\\x%02x", (unsigned char) str[i]);
                        j += 4;
                } else {
                        if (len-j < 1)
                                goto err;
                        str_enc[j] = str[i];
                        j++;
                }
        }
        if (len-j < 1)
                goto err;
        str_enc[j] = '\0';
        return 0;
err:
        return -1;
}
