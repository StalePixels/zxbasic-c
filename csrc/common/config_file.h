/*
 * config_file.h — Simple .ini config file reader
 *
 * Ported from Python's configparser usage in src/api/config.py.
 * Supports [section] headers and key = value pairs.
 */
#ifndef ZXBC_CONFIG_FILE_H
#define ZXBC_CONFIG_FILE_H

#include <stdbool.h>

/* Callback invoked for each key=value pair found in the target section.
 * Return true to continue, false to abort parsing. */
typedef bool (*config_callback)(const char *key, const char *value, void *userdata);

/*
 * Load a config file and invoke callback for each key=value in the given section.
 *
 * Returns:
 *   1 on success (section found and parsed)
 *   0 if section not found
 *  -1 on file open error
 *  -2 on parse error (e.g. duplicate keys)
 */
int config_load_section(const char *filename, const char *section, config_callback cb, void *userdata);

/* ----------------------------------------------------------------
 * Config writer — faithful port of Python configparser.write() as used
 * by src/api/config.py:143-186 save_config_into_file().
 * ---------------------------------------------------------------- */

/* Opaque builder for the target section's ordered key=value list.
 * The caller appends keys in Python OPTIONS()-iteration order; the
 * writer serializes them as `<key> = <value>\n` (configparser delimiter
 * is space-equals-space). */
typedef struct ConfigWriter ConfigWriter;

/* Create an empty section builder. Caller must config_writer_free(). */
ConfigWriter *config_writer_new(void);
void config_writer_free(ConfigWriter *w);

/* Append one `key = value` line to the section being built (order
 * preserved). value is copied. */
void config_writer_add(ConfigWriter *w, const char *key, const char *value);

/*
 * Write `w`'s key/value list into `filename` as section `[section]`,
 * faithfully reproducing Python save_config_into_file():
 *
 *   - if filename exists: parse it (simple-INI: [section] headers,
 *     `key = value` / `key=value`, whole-line ';'/'#' comments, blank
 *     lines). On a duplicate SECTION or a duplicate OPTION within one
 *     section -> return -2 (caller emits the
 *     "Invalid config file '<f>': it has duplicated fields" message and
 *     exits 1, matching config.py:160-164).
 *   - the [section] section is RESET (any pre-existing one is wiped);
 *     OTHER sections are preserved, re-serialized normalized
 *     (`key = value`, comments/spacing/blank-line layout lost — exactly
 *     configparser's behavior). Section order = original read order;
 *     [section] keeps its original position if it pre-existed, else is
 *     appended last.
 *   - each section is emitted `[name]\n` then its `key = value\n` lines
 *     then ONE blank line `\n`; the file therefore ends `...value\n\n`.
 *   - on write/open failure -> return -1 (caller emits
 *     "Can't write config file '<f>'" and exits 1, config.py:180-184).
 *
 * Returns 0 on success, -1 on IO/open error, -2 on duplicate fields.
 */
int config_writer_save(ConfigWriter *w, const char *filename, const char *section);

#endif /* ZXBC_CONFIG_FILE_H */
