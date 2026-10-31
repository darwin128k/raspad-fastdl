#include <fastdl/config.h>
#include <authum/char.h>
#include <authum/cstr.h>
#include <authum/file.h>
#include <authum/memory.h>

/* The config is read whole, then walked line by line. */
#define FASTDL_CONFIG_CAPACITY 65536

/* The schema names the field it fills: the compiler builtin gives the offset
 * of a field without <stddef.h>, so the module still names no libc. */
#define FASTDL_FIELD(type, field) __builtin_offsetof(type, field)

typedef enum fastdl_config_read_result
{
    fastdl_config_read_ok,
    fastdl_config_read_open_failed,
    fastdl_config_read_failed
} fastdl_config_read_result_t;

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
    {"bind", fastdl_setting_text, FASTDL_FIELD(fastdl_config_t, bind), 64, 0, 0},
    {"root", fastdl_setting_text, FASTDL_FIELD(fastdl_config_t, root), AUTHUM_HTTP_PATH_CAPACITY, 0,
     0},
    {"allow_dirs", fastdl_setting_text, FASTDL_FIELD(fastdl_config_t, allow_dirs), 512, 0, 0},
    {"allow_extensions", fastdl_setting_text, FASTDL_FIELD(fastdl_config_t, allow_extensions), 512,
     0, 0},
    {"deny_prefixes", fastdl_setting_text, FASTDL_FIELD(fastdl_config_t, deny_prefixes), 512, 0, 0},
    {"port", fastdl_setting_integer, FASTDL_FIELD(fastdl_config_t, port), 0, 1, 65535},
    {"max_clients", fastdl_setting_integer, FASTDL_FIELD(fastdl_config_t, max_clients), 0, 1, 256},
    {"per_ip", fastdl_setting_integer, FASTDL_FIELD(fastdl_config_t, per_ip), 0, 1, 64},
    {"requests_per_second", fastdl_setting_integer,
     FASTDL_FIELD(fastdl_config_t, requests_per_second), 0, 1, 1000},
    {"timeout_seconds", fastdl_setting_integer, FASTDL_FIELD(fastdl_config_t, timeout_seconds), 0,
     1, 120},
    {"cpu", fastdl_setting_integer, FASTDL_FIELD(fastdl_config_t, cpu), 0, -1, 1023},
    {"access_log", fastdl_setting_integer, FASTDL_FIELD(fastdl_config_t, access_log), 0, 0, 1},
    {"bytes_per_second", fastdl_setting_bytes, FASTDL_FIELD(fastdl_config_t, bytes_per_second), 0,
     1024, 1073741824},
    {"max_file_bytes", fastdl_setting_bytes, FASTDL_FIELD(fastdl_config_t, max_file_bytes), 0, 1,
     1073741824}};

void
fastdl_config_defaults(fastdl_config_t *config)
{
    authum_memory_set(config, 0, sizeof *config);
    authum_cstr_copy(config->bind, sizeof config->bind, "127.0.0.1");
    authum_cstr_copy(config->root, sizeof config->root, "{game}");
    authum_cstr_copy(config->allow_dirs, sizeof config->allow_dirs,
                     "maps models sound sprites gfx overviews");
    authum_cstr_copy(config->allow_extensions, sizeof config->allow_extensions,
                     "bsp mdl wav mp3 spr tga bmp txt res wad");
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
    while (authum_char_is_space(*text))
    {
        ++text;
    }
    authum_char_t *end = text + authum_cstr_len(text);
    while (end > text && authum_char_is_space(end[-1]))
    {
        --end;
    }
    *end = 0;
    return text;
}

/* The number a setting holds: its kind says how the text is read, its range
 * says what is accepted. */
static authum_bool_t
apply_number(const fastdl_setting_t *setting, authum_byte_t *destination,
             const authum_char_t *value)
{
    if (setting->kind == fastdl_setting_bytes)
    {
        authum_u32_t number = 0;
        if (!authum_cstr_parse_u32(value, &number) || (authum_i64_t)number < setting->minimum ||
            (authum_i64_t)number > setting->maximum)
        {
            return authum_bool_false;
        }
        *(authum_u64_t *)destination = (authum_u64_t)number;
        return authum_bool_true;
    }
    authum_i32_t number = 0;
    if (!authum_cstr_parse_i32(value, &number) || (authum_i64_t)number < setting->minimum ||
        (authum_i64_t)number > setting->maximum)
    {
        return authum_bool_false;
    }
    *(authum_i32_t *)destination = number;
    return authum_bool_true;
}

static authum_bool_t
apply_setting(fastdl_config_t *config, const authum_char_t *name, const authum_char_t *value)
{
    if (authum_cstr_is_equal(name, "source", authum_bool_false))
    {
        if (!*value || config->source_count >= AUTHUM_HTTP_MAX_SOURCES ||
            authum_cstr_len(value) >= sizeof config->sources[0])
        {
            return authum_bool_false;
        }
        authum_cstr_copy(config->sources[config->source_count++], sizeof config->sources[0], value);
        return authum_bool_true;
    }

    for (authum_usize_t index = 0; index < sizeof settings / sizeof settings[0]; ++index)
    {
        const fastdl_setting_t *setting = &settings[index];
        if (!authum_cstr_is_equal(setting->name, name, authum_bool_false))
        {
            continue;
        }
        authum_byte_t *destination = (authum_byte_t *)config + setting->offset;
        if (setting->kind == fastdl_setting_text)
        {
            if (authum_cstr_len(value) >= setting->capacity)
            {
                return authum_bool_false;
            }
            authum_cstr_copy((authum_char_t *)destination, setting->capacity, value);
            return authum_bool_true;
        }
        return apply_number(setting, destination, value);
    }
    return authum_bool_false;
}

/* Read the whole config through the Authum file facade: it reads bytes, not lines. */
static fastdl_config_read_result_t
fastdl_config_read(const authum_char_t *path, authum_char_t *text, authum_usize_t capacity,
                   authum_usize_t *size)
{
    authum_file_t file;
    authum_usize_t total = 0;

    authum_file_init(&file);
    if (!authum_file_open_cstr(&file, path, authum_file_mode_read))
    {
        return fastdl_config_read_open_failed;
    }
    if (!authum_file_read_all(&file, text, capacity, &total))
    {
        authum_file_close(&file);
        return fastdl_config_read_failed;
    }
    authum_file_close(&file);
    *size = total;
    return fastdl_config_read_ok;
}

authum_bool_t
fastdl_config_load(fastdl_config_t *config, const authum_char_t *path, authum_char_t *error,
                   authum_usize_t error_size)
{
    authum_char_t text[FASTDL_CONFIG_CAPACITY + 1];
    authum_usize_t size = 0;

    switch (fastdl_config_read(path, text, FASTDL_CONFIG_CAPACITY, &size))
    {
    case fastdl_config_read_open_failed:
        authum_cstr_format(error, error_size, "cannot open config '%s'", path);
        return authum_bool_false;
    case fastdl_config_read_failed:
        authum_cstr_format(error, error_size, "config '%s' does not fit in %u bytes", path,
                           (authum_u32_t)FASTDL_CONFIG_CAPACITY);
        return authum_bool_false;
    case fastdl_config_read_ok:
        break;
    }
    text[size] = 0;
    authum_usize_t offset = 0;
    authum_usize_t line_number = 0;
    while (offset < size)
    {
        authum_char_t *line = text + offset;
        authum_usize_t length = 0;
        ++line_number;
        while (offset + length < size && line[length] != '\n')
        {
            ++length;
        }
        line[length] = 0;
        offset += length + 1;
        authum_char_t *name = trim(line);
        if (!*name || *name == '#' || *name == ';')
        {
            continue;
        }
        authum_char_t *value = (authum_char_t *)authum_cstr_find_char(name, '=');
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
    if (!*config->allow_extensions)
    {
        authum_cstr_format(error, error_size, "config '%s': allow_extensions cannot be empty",
                           path);
        return authum_bool_false;
    }
    if (!config->source_count)
    {
        authum_cstr_copy(config->sources[0], sizeof config->sources[0],
                         *config->root ? config->root : "{game}");
        config->source_count = 1;
    }
    return authum_bool_true;

invalid:
    authum_cstr_format(error, error_size, "config '%s': invalid setting at line %u", path,
                       line_number);
    return authum_bool_false;
}
