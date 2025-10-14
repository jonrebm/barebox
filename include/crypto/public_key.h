#ifndef __CRYPTO_PUBLIC_KEY_H
#define __CRYPTO_PUBLIC_KEY_H

#include <digest.h>
#include <string.h>

struct rsa_public_key;
struct ecdsa_public_key;

enum public_key_type {
	PUBLIC_KEY_TYPE_RSA,
	PUBLIC_KEY_TYPE_ECDSA,
};

struct public_key {
	enum public_key_type type;
	struct list_head list;
	char *key_name_hint;
	char *keyring;
	unsigned char *hash;
	unsigned int hashlen;

	union {
		struct rsa_public_key *rsa;
		struct ecdsa_public_key *ecdsa;
	};
};

int public_key_add(struct public_key *key);
const struct public_key *public_key_get(const char *name, const char *keyring);
const struct public_key *public_key_next(const struct public_key *prev);

#define for_each_public_key(key) \
		for (key = public_key_next(NULL); key; key = public_key_next(key))

#define for_each_public_key_keyring(key, _keyring)                        \
	for_each_public_key(key)                                          \
		if (!key->keyring || strcmp(key->keyring, _keyring) != 0) \
			continue;                                         \
		else

int public_key_verify(const struct public_key *key, const uint8_t *sig,
		      const uint32_t sig_len, const uint8_t *hash,
		      enum hash_algo algo);

#endif /* __CRYPTO_PUBLIC_KEY_H */
