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

#endif /* ZXBC_CONFIG_FILE_H */
