#include <fastdl/config.h>
#include <ctype.h>
#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum fastdl_setting_kind
{
    fastdl_setting_text,
    fastdl_setting_integer,
    fastdl_setting_bytes
} fastdl_setting_kind_t;

typedef struct fastdl_setting
{
    const authum_char_t *name;
    fastdl_setting_kind_t kind;
    authum_usize_t offset;
    authum_usize_t capacity;
    authum_i64_t minimum;
    authum_i64_t maximum;
} fastdl_setting_t;

/* Adding a setting extends the schema, not parser control flow. */
static const fastdl_setting_t settings[] = {
    {"bind", fastdl_setting_text, offsetof(fastdl_config_t, bind), 64, 0, 0},
    {"root", fastdl_setting_text, offsetof(fastdl_config_t, root), AUTHUM_HTTP_PATH_CAPACITY, 0, 0},
    {"allow_dirs", fastdl_setting_text, offsetof(fastdl_config_t, allow_dirs), 512, 0, 0},
    {"allow_extensions", fastdl_setting_text, offsetof(fastdl_config_t, allow_extensions), 512, 0,
     0},
    {"deny_prefixes", fastdl_setting_text, offsetof(fastdl_config_t, deny_prefixes), 512, 0, 0},
    {"port", fastdl_setting_integer, offsetof(fastdl_config_t, port), 0, 1, 65535},
    {"max_clients", fastdl_setting_integer, offsetof(fastdl_config_t, max_clients), 0, 1, 256},
    {"per_ip", fastdl_setting_integer, offsetof(fastdl_config_t, per_ip), 0, 1, 64},
    {"requests_per_second", fastdl_setting_integer, offsetof(fastdl_config_t, requests_per_second),
     0, 1, 1000},
    {"timeout_seconds", fastdl_setting_integer, offsetof(fastdl_config_t, timeout_seconds), 0, 1,
     120},
    {"cpu", fastdl_setting_integer, offsetof(fastdl_config_t, cpu), 0, -1, 1023},
    {"access_log", fastdl_setting_integer, offsetof(fastdl_config_t, access_log), 0, 0, 1},
    {"bytes_per_second", fastdl_setting_bytes, offsetof(fastdl_config_t, bytes_per_second), 0, 1024,
     1073741824},
    {"max_file_bytes", fastdl_setting_bytes, offsetof(fastdl_config_t, max_file_bytes), 0, 1,
     1073741824}};

void
fastdl_config_defaults(fastdl_config_t *config)
{
    memset(config, 0, sizeof *config);
    strcpy(config->bind, "127.0.0.1");
    strcpy(config->root, "{game}");
    strcpy(config->allow_dirs, "maps models sound sprites gfx overviews");
    strcpy(config->allow_extensions, "bsp mdl wav mp3 spr tga bmp txt res wad");
    config->port = 17015;
    config->max_clients = 64;
    config->per_ip = 4;
    config->requests_per_second = 20;
    config->timeout_seconds = 15;
    config->cpu = -1;
    config->bytes_per_second = 8 * 1024 * 1024;
    config->access_log = 1;
    config->max_file_bytes = 128 * 1024 * 1024;
}

static authum_char_t *
trim(authum_char_t *text)
{
    while (isspace((authum_byte_t)*text))
    {
        ++text;
    }
    authum_char_t *end = text + strlen(text);
    while (end > text && isspace((authum_byte_t)end[-1]))
    {
        --end;
    }
    *end = 0;
    return text;
}

static authum_bool_t
apply_setting(fastdl_config_t *config, const authum_char_t *name, const authum_char_t *value)
{
    if (!strcmp(name, "source"))
    {
        if (!*value || config->source_count >= AUTHUM_HTTP_MAX_SOURCES ||
            strlen(value) >= sizeof config->sources[0])
        {
            return authum_bool_false;
        }
        strcpy(config->sources[config->source_count++], value);
        return authum_bool_true;
    }

    for (authum_usize_t index = 0; index < sizeof settings / sizeof settings[0]; ++index)
    {
        const fastdl_setting_t *setting = &settings[index];
        if (strcmp(setting->name, name))
        {
            continue;
        }
        authum_byte_t *destination = (authum_byte_t *)config + setting->offset;
        if (setting->kind == fastdl_setting_text)
        {
            if (strlen(value) >= setting->capacity)
            {
                return authum_bool_false;
            }
            strcpy((authum_char_t *)destination, value);
            return authum_bool_true;
        }

        authum_char_t *end;
        errno = 0;
        authum_i64_t number = strtoll(value, &end, 10);
        if (errno || end == value || *end || number < setting->minimum || number > setting->maximum)
        {
            return authum_bool_false;
        }
        if (setting->kind == fastdl_setting_integer)
        {
            *(authum_i32_t *)destination = (authum_i32_t)number;
        }
        else
        {
            *(authum_u64_t *)destination = (authum_u64_t)number;
        }
        return authum_bool_true;
    }
    return authum_bool_false;
}

authum_bool_t
fastdl_config_load(fastdl_config_t *config, const authum_char_t *path, authum_char_t *error,
                   authum_usize_t error_size)
{
    FILE *file = fopen(path, "r");
    if (!file)
    {
        snprintf(error, error_size, "cannot open config '%s': %s", path, strerror(errno));
        return authum_bool_false;
    }

    authum_usize_t line_number = 0;
    authum_char_t line[8192];
    while (fgets(line, sizeof line, file))
    {
        ++line_number;
        if (!strchr(line, '\n') && !feof(file))
        {
            goto invalid;
        }
        authum_char_t *name = trim(line);
        if (!*name || *name == '#' || *name == ';')
        {
            continue;
        }
        authum_char_t *value = strchr(name, '=');
        if (!value)
        {
            goto invalid;
        }
        *value++ = 0;
        if (!apply_setting(config, trim(name), trim(value)))
        {
            goto invalid;
        }
    }
    if (ferror(file))
    {
        goto invalid;
    }
    fclose(file);
    if (!*config->allow_extensions)
    {
        snprintf(error, error_size, "config '%s': allow_extensions cannot be empty", path);
        return authum_bool_false;
    }
    if (!config->source_count)
    {
        strcpy(config->sources[0], *config->root ? config->root : "{game}");
        config->source_count = 1;
    }
    return authum_bool_true;

invalid:
    snprintf(error, error_size, "config '%s': invalid setting at line %u", path, line_number);
    fclose(file);
    return authum_bool_false;
}
