/*
 * Centralised machinery for hybridised post-quantum + classical key
 * exchange setups, using the same message structure as ECDH but the
 * strings sent each way are the concatenation of a key or ciphertext
 * of each type, and the output shared secret is obtained by hashing
 * together both of the sub-methods' outputs.
 */

#include <stdio.h>
#include <stdlib.h>
#include <assert.h>

#include "putty.h"
#include "ssh.h"
#include "mpint.h"

/* ----------------------------------------------------------------------
 * Common definitions between client and server sides.
 */

typedef struct hybrid_alg hybrid_alg;

struct hybrid_alg {
    const ssh_hashalg *combining_hash;
    const pq_kemalg *pq_alg;
    const ssh_kex *classical_alg;
    void (*reformat)(ptrlen input, BinarySink *output);
};

static char *hybrid_description(const ssh_kex *kex)
{
    const struct hybrid_alg *alg = (const hybrid_alg *)kex->extra;

    /* Bit of a bodge, but think up a short name to describe the
     * classical algorithm */
    const char *classical_name;
    if (alg->classical_alg == &ssh_ec_kex_curve25519)
        classical_name = "Curve25519";
    else if (alg->classical_alg == &ssh_ec_kex_nistp256)
        classical_name = "NIST P256";
    else if (alg->classical_alg == &ssh_ec_kex_nistp384)
        classical_name = "NIST P384";
    else
        unreachable("don't have a name for this classical alg");

    return dupprintf("%s / %s hybrid key exchange",
                     alg->pq_alg->description, classical_name);
}

static void reformat_mpint_be(ptrlen input, BinarySink *output, size_t bytes)
{
    BinarySource src[1];
    BinarySource_BARE_INIT_PL(src, input);
    mp_int *mp = get_mp_ssh2(src);
    assert(!get_err(src));
    assert(get_avail(src) == 0);
    for (size_t i = bytes; i-- > 0 ;)
        put_byte(output, mp_get_byte(mp, i));
    mp_free(mp);
}

static void reformat_mpint_be_32(ptrlen input, BinarySink *output)
{
    reformat_mpint_be(input, output, 32);
}

static void reformat_mpint_be_48(ptrlen input, BinarySink *output)
{
    reformat_mpint_be(input, output, 48);
}

/* ----------------------------------------------------------------------
 * Client side.
 */

typedef struct hybrid_client_state hybrid_client_state;

static ecdh_key *hybrid_client_new(const ssh_kex *kex, bool is_server);
static void hybrid_client_free(ecdh_key *ek);
static void hybrid_client_getpublic(ecdh_key *ek, BinarySink *bs);
static bool hybrid_client_getkey(ecdh_key *ek, ptrlen remoteKey,
                                 BinarySink *bs);
static const ecdh_keyalg hybrid_client_vt = { hybrid_client_new,  hybrid_client_free,  hybrid_client_getpublic,  hybrid_client_getkey,  hybrid_description,  SSH2_PKTCTX_HYBRIDKEX};

struct hybrid_client_state {
    const hybrid_alg *alg;
    strbuf *pq_ek;
    pq_kem_dk *pq_dk;
    ecdh_key *classical;
    ecdh_key ek;
};

static ecdh_key *hybrid_client_new(const ssh_kex *kex, bool is_server)
{
    assert(!is_server);
    hybrid_client_state *s = snew(hybrid_client_state);
    s->alg = (const hybrid_alg *)kex->extra;
    s->ek.vt = &hybrid_client_vt;
    s->pq_ek = strbuf_new();
    s->pq_dk = pq_kem_keygen(s->alg->pq_alg, BinarySink_UPCAST(s->pq_ek));
    s->classical = ecdh_key_new(s->alg->classical_alg, is_server);
    return &s->ek;
}

static void hybrid_client_free(ecdh_key *ek)
{
    hybrid_client_state *s = container_of(ek, hybrid_client_state, ek);
    strbuf_free(s->pq_ek);
    pq_kem_free_dk(s->pq_dk);
    ecdh_key_free(s->classical);
    sfree(s);
}

/*
 * In the client, getpublic is called first: we make up a KEM key
 * pair, and send the public key along with a classical DH value.
 */
static void hybrid_client_getpublic(ecdh_key *ek, BinarySink *bs)
{
    hybrid_client_state *s = container_of(ek, hybrid_client_state, ek);
    put_datapl(bs, ptrlen_from_strbuf(s->pq_ek));
    ecdh_key_getpublic(s->classical, bs);
}

/*
 * In the client, getkey is called second, after the server sends its
 * response: we use our KEM private key to decapsulate the server's
 * ciphertext.
 */
static bool hybrid_client_getkey(ecdh_key *ek, ptrlen remoteKey, BinarySink *bs)
{
    hybrid_client_state *s = container_of(ek, hybrid_client_state, ek);

    BinarySource src[1];
    BinarySource_BARE_INIT_PL(src, remoteKey);

    ssh_hash *h = ssh_hash_new(s->alg->combining_hash);

    ptrlen pq_ciphertext = get_data(src, s->alg->pq_alg->c_len);
    if (get_err(src)) {
        ssh_hash_free(h);
        return false;                  /* not enough data */
    }
    if (!pq_kem_decaps(s->pq_dk, BinarySink_UPCAST(h), pq_ciphertext)) {
        ssh_hash_free(h);
        return false;                  /* pq ciphertext didn't validate */
    }

    ptrlen classical_data = get_data(src, get_avail(src));
    strbuf *classical_key = strbuf_new();
    if (!ecdh_key_getkey(s->classical, classical_data,
                         BinarySink_UPCAST(classical_key))) {
        ssh_hash_free(h);
        strbuf_free(classical_key);
        return false;                  /* classical DH key didn't validate */
    }
    s->alg->reformat(ptrlen_from_strbuf(classical_key), BinarySink_UPCAST(h));
    strbuf_free(classical_key);

    /*
     * Finish up: compute the final output hash and return it encoded
     * as a string.
     */
    unsigned char hashdata[MAX_HASH_LEN];
    ssh_hash_final(h, hashdata);
    put_stringpl(bs, make_ptrlen(hashdata, s->alg->combining_hash->hlen));
    smemclr(hashdata, sizeof(hashdata));

    return true;
}


/* ----------------------------------------------------------------------
 * Server side.
 */

typedef struct hybrid_server_state hybrid_server_state;

static ecdh_key *hybrid_server_new(const ssh_kex *kex, bool is_server);
static void hybrid_server_free(ecdh_key *ek);
static void hybrid_server_getpublic(ecdh_key *ek, BinarySink *bs);
static bool hybrid_server_getkey(ecdh_key *ek, ptrlen remoteKey,
                                 BinarySink *bs);
static const ecdh_keyalg hybrid_server_vt = { hybrid_server_new,  hybrid_server_free,  hybrid_server_getpublic,  hybrid_server_getkey,  hybrid_description,  SSH2_PKTCTX_HYBRIDKEX};

struct hybrid_server_state {
    const hybrid_alg *alg;
    strbuf *pq_ciphertext;
    ecdh_key *classical;
    ecdh_key ek;
};

static ecdh_key *hybrid_server_new(const ssh_kex *kex, bool is_server)
{
    assert(is_server);
    hybrid_server_state *s = snew(hybrid_server_state);
    s->alg = (const hybrid_alg *)kex->extra;
    s->ek.vt = &hybrid_server_vt;
    s->pq_ciphertext = strbuf_new_nm();
    s->classical = ecdh_key_new(s->alg->classical_alg, is_server);
    return &s->ek;
}

static void hybrid_server_free(ecdh_key *ek)
{
    hybrid_server_state *s = container_of(ek, hybrid_server_state, ek);
    strbuf_free(s->pq_ciphertext);
    ecdh_key_free(s->classical);
    sfree(s);
}

/*
 * In the server, getkey is called first: we receive a KEM encryption
 * key from the client and encapsulate a secret with it. We write the
 * output secret to bs; the data we'll send to the client is saved to
 * return from getpublic.
 */
static bool hybrid_server_getkey(ecdh_key *ek, ptrlen remoteKey, BinarySink *bs)
{
    hybrid_server_state *s = container_of(ek, hybrid_server_state, ek);

    BinarySource src[1];
    BinarySource_BARE_INIT_PL(src, remoteKey);

    ssh_hash *h = ssh_hash_new(s->alg->combining_hash);

    ptrlen pq_ek = get_data(src, s->alg->pq_alg->ek_len);
    if (get_err(src)) {
        ssh_hash_free(h);
        return false;                  /* not enough data */
    }
    if (!pq_kem_encaps(s->alg->pq_alg,
                       BinarySink_UPCAST(s->pq_ciphertext),
                       BinarySink_UPCAST(h), pq_ek)) {
        ssh_hash_free(h);
        return false;                  /* pq encryption key didn't validate */
    }

    ptrlen classical_data = get_data(src, get_avail(src));
    strbuf *classical_key = strbuf_new();
    if (!ecdh_key_getkey(s->classical, classical_data,
                         BinarySink_UPCAST(classical_key))) {
        ssh_hash_free(h);
        strbuf_free(classical_key);
        return false;                  /* classical DH key didn't validate */
    }
    s->alg->reformat(ptrlen_from_strbuf(classical_key), BinarySink_UPCAST(h));
    strbuf_free(classical_key);

    /*
     * Finish up: compute the final output hash and return it encoded
     * as a string.
     */
    unsigned char hashdata[MAX_HASH_LEN];
    ssh_hash_final(h, hashdata);
    put_stringpl(bs, make_ptrlen(hashdata, s->alg->combining_hash->hlen));
    smemclr(hashdata, sizeof(hashdata));

    return true;
}

static void hybrid_server_getpublic(ecdh_key *ek, BinarySink *bs)
{
    hybrid_server_state *s = container_of(ek, hybrid_server_state, ek);
    put_datapl(bs, ptrlen_from_strbuf(s->pq_ciphertext));
    ecdh_key_getpublic(s->classical, bs);
}


/* ----------------------------------------------------------------------
 * Selector vtable that instantiates the appropriate one of the above,
 * depending on is_server.
 */

static ecdh_key *hybrid_selector_new(const ssh_kex *kex, bool is_server)
{
    if (is_server)
        return hybrid_server_new(kex, is_server);
    else
        return hybrid_client_new(kex, is_server);
}

static const ecdh_keyalg hybrid_selector_vt = { hybrid_selector_new, 0, 0, 0,  hybrid_description,  SSH2_PKTCTX_HYBRIDKEX};

/* ----------------------------------------------------------------------
 * Actual KEX methods.
 */

static const hybrid_alg ssh_ntru_curve25519_hybrid = { &ssh_sha512,  &ssh_ntru,  &ssh_ec_kex_curve25519,  reformat_mpint_be_32};

static const ssh_kex ssh_ntru_curve25519 = { "sntrup761x25519-sha512", 0,  KEXTYPE_ECDH,  &ssh_sha512, {   &hybrid_selector_vt },  &ssh_ntru_curve25519_hybrid};

static const ssh_kex ssh_ntru_curve25519_openssh = { "sntrup761x25519-sha512@openssh.com", 0,  KEXTYPE_ECDH,  &ssh_sha512, {   &hybrid_selector_vt },  &ssh_ntru_curve25519_hybrid};

static const ssh_kex *const ntru_hybrid_list[] = {
    &ssh_ntru_curve25519,
    &ssh_ntru_curve25519_openssh,
};

const ssh_kexes ssh_ntru_hybrid_kex = {
    lenof(ntru_hybrid_list), ntru_hybrid_list,
};

static const hybrid_alg ssh_mlkem768_curve25519_hybrid = { &ssh_sha256,  &ssh_mlkem768,  &ssh_ec_kex_curve25519,  reformat_mpint_be_32};

static const ssh_kex ssh_mlkem768_curve25519 = { "mlkem768x25519-sha256", 0,  KEXTYPE_ECDH,  &ssh_sha256, {   &hybrid_selector_vt },  &ssh_mlkem768_curve25519_hybrid};

static const ssh_kex *const mlkem_curve25519_hybrid_list[] = {
    &ssh_mlkem768_curve25519,
};

const ssh_kexes ssh_mlkem_curve25519_hybrid_kex = {
    lenof(mlkem_curve25519_hybrid_list), mlkem_curve25519_hybrid_list,
};

static const hybrid_alg ssh_mlkem768_p256_hybrid = { &ssh_sha256,  &ssh_mlkem768,  &ssh_ec_kex_nistp256,  reformat_mpint_be_32};

static const ssh_kex ssh_mlkem768_p256 = { "mlkem768nistp256-sha256", 0,  KEXTYPE_ECDH,  &ssh_sha256, {   &hybrid_selector_vt },  &ssh_mlkem768_p256_hybrid};

static const hybrid_alg ssh_mlkem1024_p384_hybrid = { &ssh_sha384,  &ssh_mlkem1024,  &ssh_ec_kex_nistp384,  reformat_mpint_be_48};

static const ssh_kex ssh_mlkem1024_p384 = { "mlkem1024nistp384-sha384", 0,  KEXTYPE_ECDH,  &ssh_sha384, {   &hybrid_selector_vt },  &ssh_mlkem1024_p384_hybrid};

static const ssh_kex *const mlkem_nist_hybrid_list[] = {
    &ssh_mlkem1024_p384,
    &ssh_mlkem768_p256,
};

const ssh_kexes ssh_mlkem_nist_hybrid_kex = {
    lenof(mlkem_nist_hybrid_list), mlkem_nist_hybrid_list,
};
