#include "putty.h"

static const char *const conf_debug_identifiers[] = {
    #include "generated/conf_names.h"
};

const char *conf_id(int key)
{
    size_t i = key;
    if (i < lenof(conf_debug_identifiers))
        return conf_debug_identifiers[i];
    return "CONF_!outofrange!";
}
