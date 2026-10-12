/*
 * Definitions likely to be helpful to multiple AES implementations.
 */

/*
 * The 'extra' structure used by AES implementations is used to
 * include information about how to check if a given implementation is
 * available at run time, and whether we've already checked.
 */
struct aes_extra_mutable;
struct aes_extra {
    /* Function to check availability. Might be expensive, so we don't
     * want to call it more than once. */
    bool (*check_available)(void);

    /* Point to a writable substructure. */
    struct aes_extra_mutable *mut;

    /* Extra API function specific to AES, to encrypt a single block
     * in ECB mode without touching the IV. Used by AES-GCM MAC
     * setup. */
    void (*encrypt_ecb_block)(ssh_cipher *, void *);
};
struct aes_extra_mutable {
    bool checked_availability;
    bool is_available;
};
static inline bool check_availability(const struct aes_extra *extra)
{
    if (!extra->mut->checked_availability) {
        extra->mut->is_available = extra->check_available();
        extra->mut->checked_availability = true;
    }

    return extra->mut->is_available;
}

/* Shared stub function for all the AES-GCM vtables. */
void aesgcm_cipher_crypt_length(
    ssh_cipher *cipher, void *blk, int len, unsigned long seq);

/* External entry point for the encrypt_ecb_block function. */
static inline void aes_encrypt_ecb_block(ssh_cipher *ciph, void *blk)
{
    const struct aes_extra *extra = (const struct aes_extra *)ciph->vt->extra;
    extra->encrypt_ecb_block(ciph, blk);
}

/*
 * Macros to define vtables for AES variants. There are a lot of
 * these, because of the cross product between cipher modes, key
 * sizes, and assorted HW/SW implementations, so it's worth spending
 * some effort here to reduce the boilerplate in the sub-files.
 */

#define AES_EXTRA_BITS(impl_c, bits)                                    \
    static struct aes_extra_mutable aes ## bits ## impl_c ## _extra_mut; \
    static const struct aes_extra aes ## bits ## impl_c ## _extra = {   \
        aes ## impl_c ## _available,                                    \
        &aes ## bits ## impl_c ## _extra_mut,                           \
        &aes ## bits ## impl_c ## _encrypt_ecb_block,                   \
    }

#define AES_EXTRA(impl_c)                       \
    AES_EXTRA_BITS(impl_c, 128);                \
    AES_EXTRA_BITS(impl_c, 192);                \
    AES_EXTRA_BITS(impl_c, 256)

#define AES_CBC_VTABLE(impl_c, impl_display, bits)                      \
    const ssh_cipheralg ssh_aes ## bits ## _cbc ## impl_c = {           \
        aes ## impl_c ## _new,                                          \
        aes ## impl_c ## _free,                                         \
        aes ## impl_c ## _setiv_cbc,                                    \
        aes ## impl_c ## _setkey,                                       \
        aes ## bits ## impl_c ## _cbc_encrypt,                          \
        aes ## bits ## impl_c ## _cbc_decrypt,                          \
        0,                                                              \
        0,                                                              \
        nullcipher_next_message,                                        \
        "aes" #bits "-cbc",                                             \
        16,                                                             \
        bits,                                                           \
        bits/8,                                                         \
        SSH_CIPHER_IS_CBC,                                              \
        "AES-" #bits " CBC (" impl_display ")",                         \
        0,                                                              \
        &aes ## bits ## impl_c ## _extra,                               \
    }

#define AES_SDCTR_VTABLE(impl_c, impl_display, bits)                    \
    const ssh_cipheralg ssh_aes ## bits ## _sdctr ## impl_c = {         \
        aes ## impl_c ## _new,                                          \
        aes ## impl_c ## _free,                                         \
        aes ## impl_c ## _setiv_sdctr,                                  \
        aes ## impl_c ## _setkey,                                       \
        aes ## bits ## impl_c ## _sdctr,                                \
        aes ## bits ## impl_c ## _sdctr,                                \
        0,                                                              \
        0,                                                              \
        nullcipher_next_message,                                        \
        "aes" #bits "-ctr",                                             \
        16,                                                             \
        bits,                                                           \
        bits/8,                                                         \
        0,                                                              \
        "AES-" #bits " SDCTR (" impl_display ")",                       \
        0,                                                              \
        &aes ## bits ## impl_c ## _extra,                               \
    }

#define AES_GCM_VTABLE(impl_c, impl_display, bits)                      \
    const ssh_cipheralg ssh_aes ## bits ## _gcm ## impl_c = {           \
        aes ## impl_c ## _new,                                          \
        aes ## impl_c ## _free,                                         \
        aes ## impl_c ## _setiv_gcm,                                    \
        aes ## impl_c ## _setkey,                                       \
        aes ## bits ## impl_c ## _gcm,                                  \
        aes ## bits ## impl_c ## _gcm,                                  \
        aesgcm_cipher_crypt_length,                                     \
        aesgcm_cipher_crypt_length,                                     \
        aes ## impl_c ## _next_message_gcm,                             \
        /* 192-bit AES-GCM is included only so that testcrypt can run   \
         * standard test vectors against it. OpenSSH doesn't define a   \
         * protocol id for it. So we set its ssh2_id to NULL. */        \
        bits==192 ? NULL : "aes" #bits "-gcm@openssh.com",              \
        16,                                                             \
        bits,                                                           \
        bits/8,                                                         \
        SSH_CIPHER_SEPARATE_LENGTH,                                     \
        "AES-" #bits " GCM (" impl_display ")",                         \
        &ssh2_aesgcm_mac,                                               \
        &aes ## bits ## impl_c ## _extra,                               \
    }

#define AES_ALL_VTABLES(impl_c, impl_display)           \
    AES_CBC_VTABLE(impl_c, impl_display, 128);          \
    AES_CBC_VTABLE(impl_c, impl_display, 192);          \
    AES_CBC_VTABLE(impl_c, impl_display, 256);          \
    AES_SDCTR_VTABLE(impl_c, impl_display, 128);        \
    AES_SDCTR_VTABLE(impl_c, impl_display, 192);        \
    AES_SDCTR_VTABLE(impl_c, impl_display, 256);        \
    AES_GCM_VTABLE(impl_c, impl_display, 128);          \
    AES_GCM_VTABLE(impl_c, impl_display, 192);          \
    AES_GCM_VTABLE(impl_c, impl_display, 256)

/*
 * Macros to repeat a piece of code particular numbers of times that
 * correspond to 1 fewer than the number of AES rounds. (Because the
 * last round is different.)
 */
#define REP2(x) x x
#define REP4(x) REP2(REP2(x))
#define REP8(x) REP2(REP4(x))
#define REP9(x) REP8(x) x
#define REP11(x) REP8(x) REP2(x) x
#define REP13(x) REP8(x) REP4(x) x

/*
 * The round constants used in key schedule expansion.
 */
extern const uint8_t aes_key_setup_round_constants[10];

/*
 * The largest number of round keys ever needed.
 */
#define MAXROUNDKEYS 15
