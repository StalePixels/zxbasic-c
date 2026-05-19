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
#include <stdlib.h>
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

/* ================================================================
 * Config writer — faithful port of configparser.write() as used by
 * src/api/config.py:143-186 save_config_into_file().
 *
 * The merge model is a faithful SIMPLE-INI parser: [section] headers,
 * `key = value` / `key=value` (also `:` delimiter — configparser
 * splits on the FIRST of '=' or ':' whichever appears first),
 * whole-line ';'/'#' comments, blank lines. Duplicate sections and
 * duplicate options-within-a-section are detected. configparser-only
 * features (multi-line continuation values, %-interpolation, inline
 * comments — note configparser's DEFAULT keeps `; ` inside a value)
 * are NOT reproduced; see the sprint report for that boundary.
 * ================================================================ */

typedef struct CfgKV {
    char *key;
    char *value;
    struct CfgKV *next;
} CfgKV;

typedef struct CfgSection {
    char *name;
    CfgKV *head;
    CfgKV *tail;
    struct CfgSection *next;
} CfgSection;

struct ConfigWriter {
    CfgKV *head;   /* target section's ordered key/value list */
    CfgKV *tail;
};

static char *cf_strdup(const char *s) {
    size_t n = strlen(s) + 1;
    char *p = (char *)malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}

ConfigWriter *config_writer_new(void) {
    ConfigWriter *w = (ConfigWriter *)calloc(1, sizeof(*w));
    return w;
}

static void cf_kv_free(CfgKV *kv) {
    while (kv) {
        CfgKV *n = kv->next;
        free(kv->key);
        free(kv->value);
        free(kv);
        kv = n;
    }
}

void config_writer_free(ConfigWriter *w) {
    if (!w) return;
    cf_kv_free(w->head);
    free(w);
}

void config_writer_add(ConfigWriter *w, const char *key, const char *value) {
    CfgKV *kv = (CfgKV *)calloc(1, sizeof(*kv));
    if (!kv) return;
    kv->key = cf_strdup(key);
    kv->value = cf_strdup(value);
    if (w->tail) w->tail->next = kv;
    else w->head = kv;
    w->tail = kv;
}

/* Split a `key <delim> value` line on the FIRST of '=' or ':',
 * whichever occurs first (configparser default delimiters). Returns
 * pointer to the delimiter, or NULL if neither present. */
static char *cf_find_delim(char *p) {
    char *eq = strchr(p, '=');
    char *co = strchr(p, ':');
    if (!eq) return co;
    if (!co) return eq;
    return (eq < co) ? eq : co;
}

static void cf_section_add(CfgSection *sec, const char *key, const char *value) {
    CfgKV *kv = (CfgKV *)calloc(1, sizeof(*kv));
    if (!kv) return;
    kv->key = cf_strdup(key);
    kv->value = cf_strdup(value);
    if (sec->tail) sec->tail->next = kv;
    else sec->head = kv;
    sec->tail = kv;
}

static void cf_sections_free(CfgSection *s) {
    while (s) {
        CfgSection *n = s->next;
        cf_kv_free(s->head);
        free(s->name);
        free(s);
        s = n;
    }
}

int config_writer_save(ConfigWriter *w, const char *filename, const char *section) {
    CfgSection *sections = NULL, *sec_tail = NULL;
    int rc = 0;

    /* --- existing-file read (config.py:157-164) --- */
    FILE *f = fopen(filename, "r");
    if (f) {
        char line[4096];
        CfgSection *cur = NULL;
        HashMap seen_sections;
        hashmap_init(&seen_sections);

        while (fgets(line, sizeof(line), f)) {
            char *p = trim(line);
            if (!*p || *p == '#' || *p == ';') continue;  /* whole-line comment / blank */

            if (*p == '[') {
                char *end = strchr(p, ']');
                if (!end) continue;
                *end = '\0';
                char *sec_name = trim(p + 1);

                /* configparser: a duplicated section -> DuplicateSectionError */
                if (hashmap_get(&seen_sections, sec_name)) {
                    rc = -2;
                    break;
                }
                hashmap_set(&seen_sections, sec_name, (void *)1);

                CfgSection *ns = (CfgSection *)calloc(1, sizeof(*ns));
                ns->name = cf_strdup(sec_name);
                if (sec_tail) sec_tail->next = ns;
                else sections = ns;
                sec_tail = ns;
                cur = ns;
                continue;
            }

            if (!cur) continue;  /* key before any section header — configparser
                                    would MissingSectionHeaderError; our seam only
                                    needs faithful behavior for well-formed files,
                                    and a key with no section is dropped here. */

            char *delim = cf_find_delim(p);
            if (!delim) continue;
            *delim = '\0';
            char *key = trim(p);
            char *value = trim(delim + 1);

            /* duplicate option within the SAME section ->
             * DuplicateOptionError (config.py:160) */
            bool dup = false;
            for (CfgKV *it = cur->head; it; it = it->next) {
                if (strcmp(it->key, key) == 0) { dup = true; break; }
            }
            if (dup) { rc = -2; break; }

            CfgKV *kv = (CfgKV *)calloc(1, sizeof(*kv));
            kv->key = cf_strdup(key);
            kv->value = cf_strdup(value);
            if (cur->tail) cur->tail->next = kv;
            else cur->head = kv;
            cur->tail = kv;
        }

        hashmap_free(&seen_sections);
        fclose(f);

        if (rc == -2) {
            cf_sections_free(sections);
            return -2;
        }
    }

    /* --- reset the target section (config.py:166 cfg[section_] = {}) ---
     * Find an existing same-named section: wipe its key list, keep its
     * position. Otherwise append a new section at the end. */
    CfgSection *target = NULL;
    for (CfgSection *s = sections; s; s = s->next) {
        if (strcmp(s->name, section) == 0) { target = s; break; }
    }
    if (target) {
        cf_kv_free(target->head);
        target->head = target->tail = NULL;
    } else {
        CfgSection *ns = (CfgSection *)calloc(1, sizeof(*ns));
        ns->name = cf_strdup(section);
        if (sec_tail) sec_tail->next = ns;
        else sections = ns;
        sec_tail = ns;
        target = ns;
    }
    /* populate target with the builder's ordered key/value list */
    for (CfgKV *src = w->head; src; src = src->next)
        cf_section_add(target, src->key, src->value);

    /* --- write all sections (config.py:177-184; configparser.write) ---
     * Each section: `[name]\n` then `key = value\n` lines then ONE
     * blank line `\n`. File therefore ends `...value\n\n`. */
    FILE *out = fopen(filename, "wb");
    if (!out) {
        cf_sections_free(sections);
        return -1;
    }
    for (CfgSection *s = sections; s; s = s->next) {
        if (fprintf(out, "[%s]\n", s->name) < 0) { fclose(out); cf_sections_free(sections); return -1; }
        for (CfgKV *kv = s->head; kv; kv = kv->next) {
            if (fprintf(out, "%s = %s\n", kv->key, kv->value) < 0) {
                fclose(out); cf_sections_free(sections); return -1;
            }
        }
        if (fprintf(out, "\n") < 0) { fclose(out); cf_sections_free(sections); return -1; }
    }
    if (fclose(out) != 0) {
        cf_sections_free(sections);
        return -1;
    }

    cf_sections_free(sections);
    return 0;
}
