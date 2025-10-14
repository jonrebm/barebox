// SPDX-License-Identifier: GPL-2.0-only

#include "tlv/format.h"
#define pr_fmt(fmt) "barebox-tlv: " fmt

#include <common.h>
#include <tlv/tlv.h>
#include <fcntl.h>
#include <libfile.h>
#include <linux/stat.h>
#include <crc.h>
#include <net.h>
#include <crypto/public_key.h>

static int tlv_verify_try_key(const struct public_key *key, const uint8_t *sig,
			      const uint32_t sig_len, const void *data,
			      unsigned long data_len)
{
	enum hash_algo algo = HASH_ALGO_SHA256;
	int ret;
	struct digest *digest = digest_alloc_by_algo(algo);
	void *hash;

	if (!digest)
		return -ENOMEM;

	digest_init(digest);
	if (IS_ERR(digest)) {
		digest_free(digest);
		return -EINVAL;
	}
	digest_update(digest, data, data_len);
	hash = xzalloc(digest_length(digest));
	digest_final(digest, hash);

	ret = public_key_verify(key, sig, sig_len, hash, algo);

	digest_free(digest);
	free(hash);

	if (ret)
		return -ENOKEY;
	return 0;
}

static int tlv_verify(struct tlv_header *header, const char *keyring)
{
	const struct public_key *key;
	size_t payload_sz = tlv_spki_hash_offset(header);
	void *spki_tlv_ptr = (void *)header + payload_sz;
	u32 spki_tlv = get_unaligned_le32(spki_tlv_ptr);
	const int SPKI_LEN = 4;
	u32 sig_len = get_unaligned_be16(&header->length_sig);
	int ret;
	int count_spki_matches = 0;

	if (!IS_ENABLED(CONFIG_TLV_SIGNATURE)) {
		pr_err("TLV signature selected in decoder but not enabled!\n");
		return -ENOSYS;
	} else if (sig_len == 0) {
		pr_err("TLV signature selected in decoder but an unsigned TLV matched by magic %08x!\n", be32_to_cpu(header->magic));
		return -EPROTO;
	}
	/* signature length field must always be zeroed during signage and verification */
	header->length_sig = 0;

	for_each_public_key_keyring(key, keyring) {
		u32 spki_key = get_unaligned_le32(key->hash);

		if (spki_key == spki_tlv) {
			count_spki_matches++;
			ret = tlv_verify_try_key(key, spki_tlv_ptr + SPKI_LEN, sig_len - SPKI_LEN, header, payload_sz);
			if (!ret)
				return 0;
			pr_warn("TLV spki %08x matched available key but signature verification failed: %s!\n", spki_tlv, strerror(-ret));
		}
	}

	/* reset signature length field after verification to avoid later confusion */
	put_unaligned_be16(sig_len, &header->length_sig);

	if (!count_spki_matches) {
		pr_warn("TLV spki %08x matched no key!\n", spki_tlv);
		return -ENOKEY;
	}
	return -EINVAL;
}

int tlv_parse(struct tlv_device *tlvdev,
	      const struct tlv_decoder *decoder)
{
	const struct tlv *tlv = NULL;
	struct tlv_mapping *map = NULL;
	struct tlv_header *header = tlv_device_header(tlvdev);
	u32 magic;
	u16 reserved;
	size_t size;
	int ret = 0;
	u32 crc = ~0;

	magic = be32_to_cpu(header->magic);

	size = tlv_total_len(header);
	reserved = get_unaligned_be16(&header->reserved);

	if (size == SIZE_MAX) {
		pr_warn("Invalid TLV header, overflows\n");
		return -EOVERFLOW;
	}

	crc = crc32_be(crc, header, size - 4);
	if (crc != tlv_crc(header)) {
		pr_warn("Invalid CRC32. Should be %08x\n", crc);
		return -EILSEQ;
	}

	if (decoder->signature_keyring) {
		ret = tlv_verify(header, decoder->signature_keyring);
		if (ret)
			return ret;
	}

	for_each_tlv(header, tlv) {
		struct tlv_mapping **mappings;
		u16 tag = TLV_TAG(tlv);
		int len = TLV_LEN(tlv);
		const void *val = TLV_VAL(tlv);

		pr_debug("[%04x] %*ph\n", tag, len, val);

		for (mappings = decoder->mappings; *mappings; mappings++) {
			for (map = *mappings; map->tag; map++) {
				if (map->tag == tag)
					goto done;
			}
		}

done:
		if (!map || !map->tag) {
			if (tag)
				pr_warn("skipping unknown tag: %04x\n", tag);
			continue;
		}

		ret = map->handle(tlvdev, map, len, val);
		if (ret < 0)
			return ret;
	}

	return PTR_ERR_OR_ZERO(tlv);
}

struct tlv_device *tlv_register_device_by_path(const char *path, struct device *parent)
{
	struct tlv_header *header;
	struct tlv_device *tlvdev;
	size_t size;

	header = tlv_read(path, &size);
	if (IS_ERR(header))
		return ERR_CAST(header);

	tlvdev = tlv_register_device(header, parent);
	if (IS_ERR(tlvdev))
		free(header);

	return tlvdev;
}

int of_tlv_fixup(struct device_node *root, void *ctx)
{
	struct device_node *chosen, *conf, *ethaddrs;
	struct eth_ethaddr *addr;

	chosen = of_create_node(root, "/chosen");
	if (!chosen)
		return -ENOMEM;

	conf = of_copy_node(chosen, ctx);

	ethaddrs = of_get_child_by_name(conf, "ethernet-address");
	if (!ethaddrs)
		return 0;

	list_for_each_entry(addr, &ethaddr_list, list) {
		char propname[sizeof("address-4294967295")];
		const char *enetaddr_tlv_str;
		u8 enetaddr_tlv[ETH_ALEN];
		struct property *pp;

		if (!eth_of_get_fixup_node(root, NULL, addr->ethid))
			continue;

		snprintf(propname, sizeof(propname), "address-%u", addr->ethid);
		pp = of_find_property(ethaddrs, propname, NULL);
		if (!pp)
			continue;

		enetaddr_tlv_str = of_property_get_value(pp);
		if (string_to_ethaddr(enetaddr_tlv_str, enetaddr_tlv))
			continue;

		if (memcmp(enetaddr_tlv, addr->ethaddr, ETH_ALEN))
			continue;

		of_delete_property(pp);
	}

	return 0;
}

int tlv_of_register_fixup(struct tlv_device *tlvdev)
{
	return of_register_fixup(of_tlv_fixup, tlv_of_node(tlvdev));
}

void tlv_of_unregister_fixup(struct tlv_device *tlvdev)
{
	of_unregister_fixup(of_tlv_fixup, tlv_of_node(tlvdev));
}

struct tlv_header *tlv_read(const char *filename, size_t *nread)
{
	struct tlv_header *header = NULL, *tmpheader;
	size_t size;
	int fd, ret;

	fd = open(filename, O_RDONLY);
	if (fd < 0)
		return ERR_PTR(fd);

	header = malloc(128);
	if (!header) {
		ret = -ENOMEM;
		goto err;
	}

	ret = read_full(fd, header, sizeof(*header));
	if (ret >= 0 && ret != sizeof(*header))
		ret = -ENODATA;
	if (ret < 0)
		goto err;

	size = tlv_total_len(header);

	if (size == SIZE_MAX) {
		pr_warn("Invalid TLV header, overflows\n");
		ret = -EOVERFLOW;
		goto err;
	}

	tmpheader = realloc(header, size);
	if (!tmpheader) {
		struct stat st;

		ret = fstat(fd, &st);
		if (ret)
			ret = -EIO;
		else if (size > st.st_size)
			ret = -ENODATA;
		else
			ret = -ENOMEM;
		goto err;
	}
	header = tmpheader;

	ret = read_full(fd, header->tlvs, size - sizeof(*header));
	if (ret < 0)
		goto err;

	/* file might have been truncated, but this will be handled
	 * in tlv_parse
	 */

	if (nread)
		*nread = sizeof(*header) + ret;

	close(fd);
	return header;
err:
	free(header);
	close(fd);
	return ERR_PTR(ret);
}

static struct tlv *__tlv_next(const struct tlv *tlv)
{
	return (void *)tlv + 4 + TLV_LEN(tlv);
}

struct tlv *tlv_next(const struct tlv_header *header,
			     const struct tlv *tlv)
{
	void *tlvs_start = (void *)&header->tlvs[0], *tlvs_end, *next_tlv;

	tlv = tlv ? __tlv_next(tlv) : tlvs_start;

	tlvs_end = tlvs_start + get_unaligned_be32(&header->length_tlv);
	if (tlv == tlvs_end)
		return NULL;

	next_tlv = __tlv_next(tlv);
	if (next_tlv > tlvs_end)
		return ERR_PTR(-ENODATA);

	return (void *)tlv;
}
