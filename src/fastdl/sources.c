#include <fastdl/config.h>
#include <string.h>

/* Placeholders are config-only substitutions, never expanded from URLs. */
authum_bool_t
fastdl_config_expand(const authum_char_t *source, const authum_char_t *module,
                     const authum_char_t *game, authum_char_t *out, authum_usize_t size)
{
    authum_usize_t used = 0;
    while (*source)
    {
        const authum_char_t *value = NULL;
        authum_usize_t skip = 0;
        if (!strncmp(source, "{game}", 6))
        {
            value = game;
            skip = 6;
        }
        else if (!strncmp(source, "{module}", 8))
        {
            value = module;
            skip = 8;
        }
        else if (!strncmp(source, "{data}", 6))
        {
            value = module;
            skip = 6;
        }
        else if (*source == '{' || *source == '}')
        {
            return 0;
        }
        if (value)
        {
            size_t n = strlen(value);
            if (n >= size - used)
            {
                return 0;
            }
            memcpy(out + used, value, n);
            used += (authum_usize_t)n;
            if (skip == 6 && !strncmp(source, "{data}", 6))
            {
                if (5 >= size - used)
                {
                    return 0;
                }
                memcpy(out + used, "/data", 5);
                used += 5;
            }
            source += skip;
        }
        else
        {
            if (used + 1 >= size)
            {
                return 0;
            }
            out[used++] = *source++;
        }
    }
    if (used >= size)
    {
        return 0;
    }
    out[used] = 0;
    return 1;
}
