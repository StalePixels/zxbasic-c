/*
 * config_file.c — Simple .ini config file reader
 *
 * Ported from Python's configparser usage in src/api/config.py.
 */
#include "config_file.h"
#include "hashmap.h"
#include "compat.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

/* Trim leading and trailing whitespace in-place, return pointer to trimmed start */
static char *trim(char *s) {
    while (*s && isspace((unsigned char)*s)) s++;
    size_t len = strlen(s);
    while (len > 0 && isspace((unsigned char)s[len - 1])) s[--len] = '\0';
    return s;
}

int config_load_section(const char *filename, const char *section, config_callback cb, void *userdata) {
    FILE *f = fopen(filename, "r");
    if (!f) return -1;

    char line[1024];
    bool in_section = false;
    bool section_found = false;
    HashMap seen_keys;
    hashmap_init(&seen_keys);

    int result = 0;

    while (fgets(line, sizeof(line), f)) {
        char *p = trim(line);

        /* Skip empty lines and comments */
        if (!*p || *p == '#' || *p == ';') continue;

        /* Section header */
        if (*p == '[') {
            char *end = strchr(p, ']');
            if (!end) continue;
            *end = '\0';
            char *sec_name = trim(p + 1);

            if (strcasecmp(sec_name, section) == 0) {
                in_section = true;
                section_found = true;
            } else {
                if (in_section) break;  /* left our section */
            }
            continue;
        }

        if (!in_section) continue;

        /* key = value */
        char *eq = strchr(p, '=');
        if (!eq) continue;
        *eq = '\0';
        char *key = trim(p);
        char *value = trim(eq + 1);

        /* Check for duplicate keys (Python configparser raises on duplicates) */
        if (hashmap_get(&seen_keys, key)) {
            result = -2;
            break;
        }
        hashmap_set(&seen_keys, key, (void *)1);

        if (!cb(key, value, userdata)) {
            break;
        }
    }

    hashmap_free(&seen_keys);
    fclose(f);

    if (result < 0) return result;
    return section_found ? 1 : 0;
}
