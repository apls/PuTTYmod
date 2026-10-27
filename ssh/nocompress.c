/*
 * SSH compression vtable for not using compression at all.
 */

#include "putty.h"
#include "ssh.h"

static ssh_compressor *ssh_comp_none_init(void)
{
    return NULL;
}
static void ssh_comp_none_cleanup(ssh_compressor *handle)
{
}
static ssh_decompressor *ssh_decomp_none_init(void)
{
    return NULL;
}
static void ssh_decomp_none_cleanup(ssh_decompressor *handle)
{
}
static void ssh_comp_none_block(ssh_compressor *handle,
                                const unsigned char *block, int len,
                                unsigned char **outblock, int *outlen,
                                int minlen)
{
}
static bool ssh_decomp_none_block(ssh_decompressor *handle,
                                  const unsigned char *block, int len,
                                  unsigned char **outblock, int *outlen)
{
    return false;
}
const ssh_compression_alg ssh_comp_none = { "none",  NULL,  ssh_comp_none_init,  ssh_comp_none_cleanup,  ssh_comp_none_block,  ssh_decomp_none_init,  ssh_decomp_none_cleanup,  ssh_decomp_none_block,  NULL};
