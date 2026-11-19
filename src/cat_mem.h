/**
 * \file            cat_mem.h
 * \brief           Safe memory/string helpers used by cAT
 *
 * \note            This project does not use libc memory/string routines
 *                  directly. All helpers below are re-entrant by design.
 */

#ifndef CAT_MEM_HDR_H
#define CAT_MEM_HDR_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

static inline void cat_memcpy(void *dst, const void *src, size_t len)
{
        uint8_t *d = (uint8_t *)dst;
        const uint8_t *s = (const uint8_t *)src;
        size_t i;

        for (i = 0U; i < len; i++) {
                d[i] = s[i];
        }
}

static inline void cat_memset(void *dst, int val, size_t len)
{
        uint8_t *d = (uint8_t *)dst;
        size_t i;

        for (i = 0U; i < len; i++) {
                d[i] = (uint8_t)val;
        }
}

static inline size_t cat_strlen(const char *str)
{
        size_t len = 0U;

        if (str != NULL) {
                while (str[len] != '\0') {
                        len++;
                }
        }
        return len;
}

static inline void cat_strcpy(char *dst, const char *src)
{
        size_t i = 0U;

        if ((dst == NULL) || (src == NULL)) {
                return;
        }

        while (src[i] != '\0') {
                dst[i] = src[i];
                i++;
        }
        dst[i] = '\0';
}

static inline void cat_strncpy(char *dst, const char *src, size_t len)
{
        size_t i = 0U;

        if ((dst == NULL) || (src == NULL)) {
                return;
        }

        while ((i < len) && (src[i] != '\0')) {
                dst[i] = src[i];
                i++;
        }
        while (i < len) {
                dst[i] = '\0';
                i++;
        }
}

static inline int cat_strcmp(const char *a, const char *b)
{
        size_t i = 0U;

        if ((a == NULL) || (b == NULL)) {
                return (a == b) ? 0 : 1;
        }

        while ((a[i] != '\0') && (a[i] == b[i])) {
                i++;
        }
        return (int)((unsigned char)a[i] - (unsigned char)b[i]);
}

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif /* CAT_MEM_HDR_H */
