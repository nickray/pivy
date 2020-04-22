/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 *
 * Copyright (c) 2017, Joyent Inc
 * Author: Alex Wilson <alex.wilson@joyent.com>
 */

#if !defined(_PIV_H)
#define _PIV_H

#include <stdint.h>
#include <assert.h>

#if defined(__APPLE__)
#include <PCSC/wintypes.h>
#include <PCSC/winscard.h>
#else
#include <wintypes.h>
#include <winscard.h>
#endif

#include <sys/types.h>

#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include "errf.h"
#include "libssh/digest.h"

/*
 * This is the "public" interface of our PIV client implementation, backed by
 * libpcsc.
 *
 * PIV is a standard for organising cryptographic smartcards and tokens designed
 * by the United States NIST for authenticating staff at government agencies
 * and branches of the military. It is also supported by a number of
 * off-the-shelf devices and open-source Javacard applets, and has become a
 * useful standard outside the US government.
 *
 * PIV is specified in NIST SP 800-73-4, but it can be hard to read this spec
 * standalone -- it depends on a lot of assumed knowledge from the ISO7816
 * smartcard specifications, particularly ISO7816-3 and ISO7816-4.
 *
 * The libpcsc API comes from Microsoft Windows and can be found documented on
 * MSDN, and also by the open-source reimplementation pcsclite. It has become
 * the cross-platform de facto standard for communication with smartcards.
 *
 * We support both some operations at the level of the entire PIV applet (e.g.
 * read data from files like CHUID and so on) and also operations acting on
 * particular PIV key slots.
 *
 * Basic flow for using this interface:
 *
 *   SCardEstablishContext
 *        +
 *        |
 *        v
 *   +----+-------+
 *   |SCARDCONTEXT|
 *   +--+---------+
 *      |
 *      |
 *      |
 *      +->  piv_enumerate  ----+
 *      |                       |      +----------------+
 *      |                       |      |struct piv_token|
 *      |                       +----> |                |  ---> read token info
 *      |                       |      +----------------+
 *      +->  piv_find   --------+             |
 *                                            |
 *                                            |
 *                                            |
 *      +---+    piv_txn_begin   <------------+
 *      |
 *      |
 *      +---------->   piv_select   ----+--->   read/write files
 *                                      |
 *                          |           +--->   admin operations
 *                          |           |
 *                          |           +--->   verify or change PIN etc
 *   +---- piv_read_cert <--+
 *   |
 *   |
 *   +-->  piv_get_slot -----+    +---------------+
 *   |                       +--> |struct piv_slot|
 *   |                       |    |               |
 *   +-->  piv_slot_next ----+    +---------------+
 *                                      |
 *                                      |
 *                                      |
 *                                      +--->   read cert/key info
 *                                      |
 *                                      +--->   key operations (sign, ecdh etc)
 *
 * YubicoPIV-specific commands and options are generally prefixed with "YK"
 * (e.g. ykpiv_generate for the version of the piv_generate function with
 * YubicoPIV extensions).
 */

/*
 * PIV key slots have an 8-bit numeric ID. This is the list of all the slot
 * IDs that we support.
 */
enum piv_slotid {
	PIV_SLOT_9A = 0x9A,
	PIV_SLOT_9B = 0x9B,
	PIV_SLOT_9C = 0x9C,
	PIV_SLOT_9D = 0x9D,
	PIV_SLOT_9E = 0x9E,

	PIV_SLOT_82 = 0x82,
	PIV_SLOT_95 = 0x95,

	PIV_SLOT_F9 = 0xF9,

	PIV_SLOT_PIV_AUTH = PIV_SLOT_9A,
	PIV_SLOT_ADMIN = PIV_SLOT_9B,
	PIV_SLOT_SIGNATURE = PIV_SLOT_9C,
	PIV_SLOT_KEY_MGMT = PIV_SLOT_9D,
	PIV_SLOT_CARD_AUTH = PIV_SLOT_9E,

	PIV_SLOT_RETIRED_1 = PIV_SLOT_82,
	PIV_SLOT_RETIRED_20 = PIV_SLOT_95,

	PIV_SLOT_YK_ATTESTATION = PIV_SLOT_F9,
};

/*
 * Tags for various PIV "files" or "objects" that can be retrieved.
 *
 * Most of these are used internally, but you can also pass them to e.g.
 * piv_read_file() if you want to.
 */
enum piv_tags {
	PIV_TAG_CARDCAP = 0x5FC107,
	PIV_TAG_CHUID = 0x5FC102,
	PIV_TAG_SECOBJ = 0x5FC106,
	PIV_TAG_KEYHIST = 0x5FC10C,
	PIV_TAG_PRINTINFO = 0x5FC109,
	PIV_TAG_DISCOV = 0x7E,
	PIV_TAG_CERT_9A = 0x5FC105,
	PIV_TAG_CERT_9C = 0x5FC10A,
	PIV_TAG_CERT_9D = 0x5FC10B,
	PIV_TAG_CERT_9E = 0x5FC101,

	PIV_TAG_CERT_82 = 0x5FC10D,	/* First retired slot */
	PIV_TAG_CERT_95 = 0x5FC120,	/* Last retired slot */

	PIV_TAG_CERT_YK_ATTESTATION = 0x5FFF01,
};

/*
 * Supported cryptographic algorithms and their PIV ID numbers. You can find
 * the table of these in NIST SP 800-78-4.
 */
enum piv_alg {
	PIV_ALG_3DES = 0x03,
	PIV_ALG_AES128 = 0x08,
	PIV_ALG_AES192 = 0x0A,
	PIV_ALG_AES256 = 0x0C,

	PIV_ALG_RSA1024 = 0x06,
	PIV_ALG_RSA2048 = 0x07,
	PIV_ALG_ECCP256 = 0x11,
	PIV_ALG_ECCP384 = 0x14,

	/*
	 * Proprietary hack for Javacards running PivApplet -- they don't
	 * support bare ECDSA so instead we have to give them the full input
	 * data and they hash it on the card.
	 */
	PIV_ALG_ECCP256_SHA1 = 0xf0,
	PIV_ALG_ECCP256_SHA256 = 0xf1,
};

/* Types of PIV cardholder authentication methods. */
enum piv_pin {
	/* PIV application PIN, local to the PIV applet. */
	PIV_PIN = 0x80,
	/* A global PIN used by all applets on the card. */
	PIV_GLOBAL_PIN = 0x00,
	/* PIN Unlock code, used if the PIN is lost/forgotten. */
	PIV_PUK = 0x81,

	/*
	 * We don't really support these yet, but OCC is "on-chip comparison"
	 * of biometric data.
	 */
	PIV_OCC = 0x96,
	PIV_OCC2 = 0x97,

	/* Only useful with securechannel/VCI (not supported) */
	PIV_PAIRING = 0x98
};

enum ykpiv_pin_policy {
	YKPIV_PIN_DEFAULT = 0x00,
	YKPIV_PIN_NEVER = 0x01,
	YKPIV_PIN_ONCE = 0x02,
	YKPIV_PIN_ALWAYS = 0x03,
};

enum ykpiv_touch_policy {
	YKPIV_TOUCH_DEFAULT = 0x00,
	YKPIV_TOUCH_NEVER = 0x01,
	YKPIV_TOUCH_ALWAYS = 0x02,
	YKPIV_TOUCH_CACHED = 0x03,		/* Cached for 15sec */
};

#define	GUID_LEN	16

struct piv_slot;
struct piv_token;

/*
 * Enumerates all PIV tokens attached to the given SCARDCONTEXT.
 *
 * Note that the PIV tokens will not have their certificates enumerated as
 * yet and you should use piv_read_cert() / piv_read_all_certs() to populate
 * the list of slots if you want to use one.
 *
 * Errors:
 *  - PCSCError: a PCSC call failed in a way that is not retryable
 */
MUST_CHECK
errf_t *piv_enumerate(SCARDCONTEXT ctx, struct piv_token **tokens);

/*
 * Retrieves a PIV token on the system which matches a given GUID or GUID
 * prefix. If guidlen < GUID_LEN, then guid will be interpreted as a prefix
 * to search for.
 *
 * This is faster than using piv_enumerate() and searching the list yourself
 * since it doesn't try to fully probe each token for capabilities before
 * checking the GUID.
 *
 * Errors:
 *  - PCSCError: a PCSC call failed in a way that is not retryable
 *  - DuplicateError: a GUID prefix was given and it is not unique on the system
 *  - NotFoundError: token matching the guid was not found
 */
MUST_CHECK
errf_t *piv_find(SCARDCONTEXT ctx, const uint8_t *guid, size_t guidlen,
    struct piv_token **token);

/*
 * Returns the next token on a list of tokens such as that returned by
 * piv_enumerate().
 */
struct piv_token *piv_token_next(struct piv_token *token);

/*
 * Releases a list of tokens acquired from piv_enumerate or a token from
 * piv_find.
 */
void piv_release(struct piv_token *pk);

/* Returns the string PCSC "reader name" for the token. */
const char *piv_token_rdrname(const struct piv_token *token);

/*
 * Returns the card's FASC-N (a NIST card identity string). Lots of
 * non-US-government PIV cards won't have anything here or will have garbage.
 */
const uint8_t *piv_token_fascn(const struct piv_token *token, size_t *len);

const uint8_t *piv_token_expiry(const struct piv_token *token, size_t *len);

/* The buffer returned from these is always GUID_LEN bytes in length. */
const uint8_t *piv_token_guid(const struct piv_token *token);
const uint8_t *piv_token_chuuid(const struct piv_token *token);

/*
 * Convenience function: returns the piv_token_guid() data as a hexadecimal
 * zero-terminated string.
 */
const char *piv_token_guid_hex(const struct piv_token *token);

/*
 * Retrieve the advertised algorithms supported by the card (if any). This is
 * not a compulsory field.
 */
size_t piv_token_nalgs(const struct piv_token *token);
enum piv_alg piv_token_alg(const struct piv_token *token, size_t idx);

boolean_t piv_token_has_chuid(const struct piv_token *token);
boolean_t piv_token_has_signed_chuid(const struct piv_token *token);

/*
 * Returns the default authentication mechanism for the card (typically this
 * is one of the possible types of PIN). The card may allow other methods to
 * be used as well, but it specifies this one as the primary method.
 */
enum piv_pin piv_token_default_auth(const struct piv_token *token);

/*
 * Returns true if the card supports a type of user authentication.
 */
boolean_t piv_token_has_auth(const struct piv_token *token, enum piv_pin auth);

/*
 * Returns true if the card supports VCI (virtual contact interface) secure
 * messaging -- this is used to provide secure communications with the card
 * over contactless interfaces.
 */
boolean_t piv_token_has_vci(const struct piv_token *token);

/*
 * Returns the number of key history slots in use on the token which have
 * certs stored on the actual card itself.
 */
uint piv_token_keyhistory_oncard(const struct piv_token *token);
/*
 * Returns the number of key history slots in use on the token which have
 * certs stored at a URL instead of on the card (see also
 * piv_token_offcard_url()).
 */
uint piv_token_keyhistory_offcard(const struct piv_token *token);
/* Returns the URL used to retrieve off-card key history certs. */
const char *piv_token_offcard_url(const struct piv_token *token);

/*
 * Returns true if the card advertises that it implements YubicoPIV extensions
 */
boolean_t piv_token_is_ykpiv(const struct piv_token *token);

/* The buffer is always 3 bytes long. */
const uint8_t *ykpiv_token_version(const struct piv_token *token);
/*
 * Compares the YubicoPIV version advertised by the card to the given tuple of
 * (major, minor, patch). Returns -1 if the card version is earlier than the
 * given version, 0 if it is the same, and 1 if it is later.
 */
int ykpiv_version_compare(const struct piv_token *token, uint8_t major,
    uint8_t minor, uint8_t patch);

/*
 * Returns true if the card allows reading the YubiKey serial number over
 * PIV interface. Only YubicoPIV >=5.0.0 supports this command.
 */
boolean_t ykpiv_token_has_serial(const struct piv_token *token);
/* Retrieves a YubiKey serial number. */
uint32_t ykpiv_token_serial(const struct piv_token *token);

/*
 * Gets a reference to a particular key/cert slot on the card. This must have
 * been enumerated using piv_read_cert, or else this will return NULL.
 */
struct piv_slot *piv_get_slot(struct piv_token *tk, enum piv_slotid slotid);

/*
 * Iterate over all the key slots found on a given card. Give NULL for the
 * "slot" argument to retrieve the first slot.
 */
struct piv_slot *piv_slot_next(struct piv_token *tk, struct piv_slot *slot);

/*
 * Forces the enumeration of a slot which doesn't have a valid certificate on
 * the card. This can useful to ask the card for a signature from a particular
 * slot even though no certificate has been written there yet (or is stored
 * off-card in the case of key history slots).
 */
struct piv_slot *piv_force_slot(struct piv_token *tk, enum piv_slotid slotid,
   enum piv_alg alg);

/* Returns the key reference ID for the given slot. */
enum piv_slotid piv_slot_id(const struct piv_slot *slot);

/* Returns the algorithm ID for the given slot. */
enum piv_alg piv_slot_alg(const struct piv_slot *slot);

/*
 * Returns the certificate stored for a given slot.
 *
 * The memory referenced by the returned pointer should be treated as const
 * and not freed or modified (it will be freed with the piv_slot).
 */
X509 *piv_slot_cert(const struct piv_slot *slot);
/* Helper: retrieves the subject DN from the certificate for a slot. */
const char *piv_slot_subject(const struct piv_slot *slot);

/*
 * Returns the public key for a slot.
 *
 * The memory referenced by the returned pointer should be treated as const
 * and not freed or modified (it will be freed with the piv_slot).
 */
struct sshkey *piv_slot_pubkey(const struct piv_slot *slot);

/*
 * Begins a new transaction on the card. Needs to be called before any
 * interaction with the card is possible.
 *
 * Errors:
 *  - IOError: general communication failure
 */
MUST_CHECK
errf_t *piv_txn_begin(struct piv_token *key);

/*
 * Ends a transaction.
 */
void piv_txn_end(struct piv_token *key);

/* Returns true if the token is in an open transaction (from piv_txn_begin) */
boolean_t piv_token_in_txn(const struct piv_token *token);

/*
 * Selects the PIV applet on the card. You should run this first in each
 * txn to prepare the card for other PIV commands.
 *
 * Errors:
 *  - IOError: general card communication failure
 *  - InvalidDataError: device returned invalid or unsupported payload to
 *                      select command
 *  - NotFoundError: PIV applet not found on card
 */
MUST_CHECK
errf_t *piv_select(struct piv_token *tk);

/*
 * Reads the certificate in a given slot on the card, and updates the list
 * of struct piv_slots with info about it.
 *
 * This is required before commands that require a slot reference can be used
 * (e.g. piv_sign, piv_ecdh).
 *
 * Errors:
 *  - IOError: general card communication failure
 *  - NotFoundError: no key/cert is present in this slot
 *  - NotSupportedError: card does not support the use of this slot
 *  - PermissionError: the cert in this slot requires either using a contact
 *                     interface (and the card is connected contactless), or
 *                     requires a PIN
 *  - InvalidDataError: device returned an invalid payload or unparseable
 *                      certificate
 *  - APDUError: card rejected the request (e.g because applet not selected)
 */
MUST_CHECK
errf_t *piv_read_cert(struct piv_token *tk, enum piv_slotid slotid);
/*
 * Attempts to read certificates in all supported PIV slots on the card, by
 * calling piv_read_cert repeatedly. Ignores ENOENT and ENOTSUP errors. Any
 * other error will return early and may not try all slots.
 */
MUST_CHECK
errf_t *piv_read_all_certs(struct piv_token *tk);

/*
 * Authenticates as the card administrator using a 3DES key.
 *
 * Errors:
 *  - IOError: general card communication failure
 *  - NotFoundError: the card has no 3DES admin key
 *  - NotSupportedError: the card does not support 3DES admin auth
 *  - InvalidDataError: the card returned unparseable data
 *  - PermissionError: the key was invalid or admin auth not allowed through
 *                     this interface (e.g. contactless)
 *  - APDUError: the card rejected the command
 */
MUST_CHECK
errf_t *piv_auth_admin(struct piv_token *tk, const uint8_t *key, size_t keylen);

/*
 * YubicoPIV-specific: changes the 3DES card administrator key.
 *
 * Errors:
 *  - ArgumentError: tk is not YubicoPIV-compatible or touchpolicy is
 *                   unsupported on this version of YubicoPIV
 *  - IOError: general card communication failure
 *  - PermissionError: must call piv_auth_admin() first
 *  - APDUError: the card rejected the command
 */
MUST_CHECK
errf_t *ykpiv_set_admin(struct piv_token *tk, const uint8_t *key, size_t keylen,
    enum ykpiv_touch_policy touchpolicy);

/*
 * Generates a new asymmetric private key in a slot on the token, and returns
 * the public key.
 *
 * The public key should be freed by the caller with sshkey_free().
 *
 * Errors:
 *  - IOError: general card communication failure
 *  - ArgumentError: algorithm or slot ID not supported
 *  - PermissionError: the card requires admin authentication before generating
 *                     keys
 *  - InvalidDataError: the card returned invalid data which was unparseable
 *                      or unsafe to use (e.g. bad EC public point)
 *  - APDUError: the card rejected the command
 */
MUST_CHECK
errf_t *piv_generate(struct piv_token *tk, enum piv_slotid slotid,
    enum piv_alg alg, struct sshkey **pubkey);

/*
 * Writes the key history object of the card with the given counts of on-
 * and off-card certs and a URL for retrieving off-card certificates.
 *
 * You should use this after generating a key in one of the key history
 * slots.
 *
 * Errors:
 *  - IOError: general card communication failure
 *  - ArgumentError: counts are too large, or offcard > 0 && offcard_url == NULL
 *  - PermissionError: the card requires admin authentication before writing
 *  - APDUError: the card rejected the command
 */
MUST_CHECK
errf_t *piv_write_keyhistory(struct piv_token *tk, uint oncard, uint offcard,
    const char *offcard_url);

/*
 * YubicoPIV specific: generates a new asymmetric private key in a slot on the
 * token, and returns the public key in the same manner as piv_generate(), but
 * takes two extra arguments for the PIN and Touch policy that can be set with
 * YubicoPIV.
 *
 * Errors:
 *  - IOError: general card communication failure
 *  - ArgumentError: algorithm or slot ID not supported, card is not YubicoPIV
 *                   or version does not support given policies
 *  - PermissionError: the card requires admin authentication before generating
 *                     keys
 *  - InvalidDataError: the card returned invalid data which was unparseable
 *                      or unsafe to use (e.g. bad EC public point)
 *  - APDUError: the card rejected the command
 */
MUST_CHECK
errf_t *ykpiv_generate(struct piv_token *tk, enum piv_slotid slotid,
    enum piv_alg alg, enum ykpiv_pin_policy pinpolicy,
    enum ykpiv_touch_policy touchpolicy, struct sshkey **pubkey);

/*
 * YubicoPIV specific: import an asymmetric private key into a given slot.
 *
 * Errors:
 *  - IOError: general card communication failure
 *  - ArgumentError: algorithm or slot ID not supported, card is not YubicoPIV
 *                   or version does not support given policies
 *  - PermissionError: the card requires admin authentication before generating
 *                     keys
 *  - APDUError: the card rejected the command
 */
MUST_CHECK
errf_t *ykpiv_import(struct piv_token *tk, enum piv_slotid slotid,
    struct sshkey *privkey, enum ykpiv_pin_policy pinpolicy,
    enum ykpiv_touch_policy touchpolicy);

/*
 * Loads a certificate for a given slot on the token.
 *
 * "flags" should include bits from enum piv_certinfo_flags (and piv_cert_comp).
 *
 * Errors:
 *  - IOError: general card communication failure
 *  - DeviceOutOfMemoryError: certificate is too large to fit on card
 *  - PermissionError: admin authentication required to write a cert
 *  - NotSupportedError: slot unsupported
 *  - APDUError: other card error
 */
MUST_CHECK
errf_t *piv_write_cert(struct piv_token *tk, enum piv_slotid slotid,
    const uint8_t *data, size_t datalen, uint flags);

/*
 * Writes a file object on the PIV token by its bare tag number.
 *
 * The "data" buffer should contain everything that goes inside the '53' tag
 * in the INS_PUT_DATA command. You do not need to include the '53' tag itself.
 *
 * Errors:
 *  - IOError: general card communication failure
 *  - DeviceOutOfMemoryError: file is too large to fit on card
 *  - PermissionError: admin authentication required to write a cert
 *  - NotSupportedError: file object tag unsupported
 *  - APDUError: other card error
 */
MUST_CHECK
errf_t *piv_write_file(struct piv_token *pt, uint tag,
    const uint8_t *data, size_t len);

/*
 * Reads a file object on the PIV token by its bare tag number.
 *
 * Like piv_write_file() this returns a data buffer containing the contents
 * of the '53' tag returned by INS_GET_DATA. The '53' tag itself is not
 * included.
 *
 * The "len" argument will be written with the length of data and the "data"
 * argument written with a pointer to an allocated data buffer of that length.
 *
 * The buffer of file data returned should be released with
 * piv_file_data_free().
 *
 * Errors:
 *  - IOError: general card communication failure
 *  - PermissionError: card didn't allow this object to be read (might require
 *                     PIN or is only retrievable over contact interface)
 *  - NotFoundError: no file found at the given tag
 *  - InvalidDataError: the tag structure returned by the card made no sense
 */
MUST_CHECK
errf_t *piv_read_file(struct piv_token *pt, uint tag, uint8_t **data,
    size_t *len);

/*
 * Zeroes and releases a file data buffer allocated by piv_read_file().
 */
void piv_file_data_free(uint8_t *data, size_t len);

/*
 * Tries to unlock the PIV token using a PIN code.
 *
 * The "pin" argument should be a NULL-terminated ASCII numeric string of the
 * PIN to use. Max length is 8 digits.
 *
 * The boolean "canskip" argument is used to indicate whether PIN entry should
 * be skipped if the PIN has already been entered (we use an empty VERIFY
 * command to check the security status). This should be to B_FALSE before
 * using "PIN Always" slots like the 9C Digital Signature slot.
 *
 * If the argument "retries" is given, then it will be read to determine a
 * minimum number of remaining attempts to assert are possible before trying to
 * unlock: if less than "*retries" attempts are remaining, we will not attempt
 * to unlock and will return EAGAIN.
 *
 * If EACCES is returned, then "retries" will also be written with the new
 * remaining attempts count.
 *
 * Some cards may accept characters other than numbers in a PIN -- such
 * behaviour is completely card implementation-defined, but typically a
 * character-set violation will result in an APDUError being returned.
 *
 * Errors:
 *  - ArgumentError: PIN supplied was zero-length or >8 chars long
 *  - IOError: general card communication failure
 *  - APDUError: the card rejected the command (e.g. because applet not
 *               selected)
 *  - MinRetriesError: the PIN has a remaining retries count that is too low
 *                     when compared with input value of "retries"
 *  - NotSupportedError: if pin was given as NULL to do a retry counter check
 *                       and the card does not support this form of the
 *                       command
 *  - PermissionError: the PIN code was incorrect. If non-NULL, the "retries"
 *                     argument will be written with the number of attempts
 *                     remaining before the card locks itself (and potentially
 *                     erases keys)
 */
MUST_CHECK
errf_t *piv_verify_pin(struct piv_token *tk, enum piv_pin type, const char *pin,
    uint *retries, boolean_t canskip);

/*
 * Changes the PIV PIN on a token.
 *
 * The "pin" and "newpin" arguments should be a NULL-terminated ASCII numeric
 * string of the PIN to use. Max length is 8 digits.
 *
 * Errors:
 *  - ArgumentError: PIN supplied was zero-length or >8 digits long
 *  - IOError: general card communication failure
 *  - APDUError: the card rejected the command (e.g. because applet not
 *               selected)
 *  - PermissionError: the old PIN code was incorrect.
 */
MUST_CHECK
errf_t *piv_change_pin(struct piv_token *tk, enum piv_pin type, const char *pin,
    const char *newpin);

/*
 * Resets the PIV PIN on a token using the PUK.
 *
 * The "puk" and "newpin" arguments should be a NULL-terminated ASCII numeric
 * string of the PIN to use. Max length is 8 digits.
 *
 * Errors:
 *  - ArgumentError: PIN supplied was zero-length or >8 digits long
 *  - IOError: general card communication failure
 *  - APDUError: the card rejected the command (e.g. because applet not selected)
 *  - PermissionError: the PUK was incorrect.
 */
MUST_CHECK
errf_t *piv_reset_pin(struct piv_token *tk, enum piv_pin type, const char *puk,
    const char *newpin);

/*
 * YubicoPIV only: resets the entire PIV applet to defaults, including PIN, PUK,
 * 9B admin key and all certificate keys and slots.
 *
 * Requires that the PIN and PUK have both been blocked (i.e. all retries
 * used up) before executing.
 *
 * Errors:
 *  - APDUError: the card rejected the command (e.g. because applet not selected)
 *  - NotSupportedError: the card does not support YubicoPIV extensions
 *  - IOError: general card communication failure
 *  - ResetConditionsError: conditions to allow a factory reset were not met
 *                          (need to have PIN and PUK blocked)
 */
MUST_CHECK
errf_t *ykpiv_reset(struct piv_token *tk);

/*
 * YubicoPIV only: changes the maximum number of retries for the PIN and PUK.
 * This also resets both PIN and PUK to their default values. To execute it
 * you must have called both piv_auth_admin() and piv_verify_pin() in this
 * transaction.
 *
 * Errors:
 *  - IOError: general card communication failure
 *  - APDUError: the card rejected the command (e.g. because applet not selected)
 *  - NotSupportedError: the card does not support YubicoPIV extensions
 *  - PermissionError: the necessary auth has not been done before calling
 *                     (piv_auth_admin() and piv_verify_pin()).
 */
MUST_CHECK
errf_t *ykpiv_set_pin_retries(struct piv_token *tk, uint pintries, uint puktries);

/*
 * Authenticates a PIV key slot by matching its public key against the given
 * public key, and then asking it to sign randomly generated data to validate
 * that the key does match.
 *
 * Errors:
 *  - IOError: general card communication failure
 *  - APDUError: the card rejected the command (e.g. because applet not selected)
 *  - PermissionError: the key slot in question is locked
 *  - NotSupportedError: the card returned a GEN_AUTH payload type that isn't
 *                       supported
 *  - KeyAuthError: the key validation failed (either because it doesn't match
 *                  the provided pubkey, or because the signature did not
 *                  validate)
 */
MUST_CHECK
errf_t *piv_auth_key(struct piv_token *tk, struct piv_slot *slot,
    struct sshkey *pubkey);

/*
 * Requests an attestation certificate.
 *
 * Errors:
 *  - IOError: general card communication failure
 *  - NotSupportedError: the card does not support YubicoPIV extensions
 *  - APDUError: the card rejected the command (e.g. because applet not selected)
 */
MUST_CHECK
errf_t *ykpiv_attest(struct piv_token *tk, struct piv_slot *slot,
    uint8_t **data, size_t *len);

/*
 * Signs a payload using a private key stored on the card.
 *
 * "data" must contain "datalen" bytes of payload that will be signed. For
 * piv_sign() this is the actual raw data (and this function or the card will
 * hash it for you as part of signing). For piv_sign_prehash() the "data" is
 * the hash itself instead. If the card only supports hash-on-card for an EC
 * key slot, piv_sign_prehash() will return NotSupportedError.
 *
 * "hashalgo" will be written with the SSH digest ID of the hash algorithm that
 * was used. It can also be filled out with a desired hash algorithm before
 * calling (this will probably only work with RSA). We might not be able to give
 * you the algo you asked for (you will need to check it on return).
 *
 * "signature" will be written with a pointer to a buffer "siglen" bytes long
 * containing the output signature in ASN.1/X509 format. It should be released
 * with free().
 *
 * Errors:
 *   - IOError: general card communication failure
 *   - PermissionError: the key slot in question is locked and cannot be used.
 *                      You might need to unlock the card with piv_verify_pin.
 *   - APDUError: the card rejected the command (e.g. because applet not selected)
 *   - InvalidDataError: the card returned unparseable or invalid payloads
 *   - NotFoundError: the given slot has no key in it or is not supported by
 *                    the card
 *   - NotSupportedError: algorithm or slot is not supported
 */
MUST_CHECK
errf_t *piv_sign(struct piv_token *tk, struct piv_slot *slot,
    const uint8_t *data, size_t datalen, enum sshdigest_types *hashalgo,
    uint8_t **signature, size_t *siglen);
MUST_CHECK
errf_t *piv_sign_prehash(struct piv_token *tk, struct piv_slot *slot,
    const uint8_t *hash, size_t hashlen, uint8_t **signature, size_t *siglen);

/*
 * Performs an ECDH key derivation between the private key on the token and
 * the given EC public key.
 *
 * "pubkey" must point at an EC public key.
 *
 * "secret" will be written with a pointer to a buffer "seclen" bytes long
 * containing the output shared secret. It should be released with freezero().
 *
 * Errors:
 *   - IOError: general card communication failure
 *   - PermissionError: the key slot in question is locked and cannot be used.
 *                      You might need to unlock the card with piv_verify_pin.
 *   - APDUError: the card rejected the command (e.g. because applet not selected)
 *   - InvalidDataError: the card returned a GEN_AUTH payload type that isn't
 *                       supported or was invalid
 */
MUST_CHECK
errf_t *piv_ecdh(struct piv_token *tk, struct piv_slot *slot,
    struct sshkey *pubkey, uint8_t **secret, size_t *seclen);


struct piv_ecdh_box;

struct piv_ecdh_box *piv_box_new(void);
struct piv_ecdh_box *piv_box_clone(const struct piv_ecdh_box *box);
void piv_box_free(struct piv_ecdh_box *box);

MUST_CHECK
errf_t *piv_box_set_data(struct piv_ecdh_box *box, const uint8_t *data, size_t len);
MUST_CHECK
errf_t *piv_box_set_datab(struct piv_ecdh_box *box, struct sshbuf *buf);
MUST_CHECK
errf_t *piv_box_seal(struct piv_token *tk, struct piv_slot *slot,
    struct piv_ecdh_box *box);
MUST_CHECK
errf_t *piv_box_seal_offline(struct sshkey *pubk, struct piv_ecdh_box *box);
MUST_CHECK
errf_t *piv_box_to_binary(struct piv_ecdh_box *box, uint8_t **output, size_t *len);

boolean_t piv_box_has_guidslot(const struct piv_ecdh_box *box);
const uint8_t *piv_box_guid(const struct piv_ecdh_box *box);
const char *piv_box_guid_hex(const struct piv_ecdh_box *box);
enum piv_slotid piv_box_slot(const struct piv_ecdh_box *box);
struct sshkey *piv_box_pubkey(const struct piv_ecdh_box *box);
struct sshkey *piv_box_ephem_pubkey(const struct piv_ecdh_box *box);
MUST_CHECK
errf_t *piv_box_copy_pubkey(const struct piv_ecdh_box *box, struct sshkey **tgt);
const char *piv_box_cipher(const struct piv_ecdh_box *box);
const char *piv_box_kdf(const struct piv_ecdh_box *box);
size_t piv_box_encsize(const struct piv_ecdh_box *box);
boolean_t piv_box_sealed(const struct piv_ecdh_box *box);
size_t piv_box_nonce_size(const struct piv_ecdh_box *box);
uint piv_box_version(const struct piv_ecdh_box *box);

void piv_box_set_guid(struct piv_ecdh_box *box, const uint8_t *guid,
    size_t len);
void piv_box_set_slot(struct piv_ecdh_box *box, enum piv_slotid slot);

MUST_CHECK
errf_t *piv_box_from_binary(const uint8_t *input, size_t len,
    struct piv_ecdh_box **box);
MUST_CHECK
errf_t *piv_box_find_token(struct piv_token *tks, struct piv_ecdh_box *box,
    struct piv_token **tk, struct piv_slot **slot);
MUST_CHECK
errf_t *piv_box_open(struct piv_token *tk, struct piv_slot *slot,
    struct piv_ecdh_box *box);
MUST_CHECK
errf_t *piv_box_open_offline(struct sshkey *privkey, struct piv_ecdh_box *box);
MUST_CHECK
errf_t *piv_box_take_data(struct piv_ecdh_box *box, uint8_t **data, size_t *len);
MUST_CHECK
errf_t *piv_box_take_datab(struct piv_ecdh_box *box, struct sshbuf **buf);

MUST_CHECK
errf_t *sshbuf_put_piv_box(struct sshbuf *buf, struct piv_ecdh_box *box);
MUST_CHECK
errf_t *sshbuf_get_piv_box(struct sshbuf *buf, struct piv_ecdh_box **box);

/* Low-level APDU access */
struct apdu;

enum iso_class {
	CLA_ISO = 0x00,
	CLA_CHAIN = 0x10
};

enum iso_sel_p1 {
	SEL_APP_AID = 0x04
};

enum iso_ins {
	/* Standard commands from ISO7816-4 */
	INS_SELECT = 0xA4,
	INS_GET_DATA = 0xCB,
	INS_VERIFY = 0x20,
	INS_CHANGE_PIN = 0x24,
	INS_RESET_PIN = 0x2C,
	INS_GEN_AUTH = 0x87,
	INS_PUT_DATA = 0xDB,
	INS_GEN_ASYM = 0x47,
	INS_CONTINUE = 0xC0,

	/* YubicoPIV specific */
	INS_SET_MGMT = 0xFF,
	INS_IMPORT_ASYM = 0xFE,
	INS_GET_VER = 0xFD,
	INS_SET_PIN_RETRIES = 0xFA,
	INS_RESET = 0xFB,
	INS_GET_SERIAL = 0xF8,
	INS_ATTEST = 0xF9,
};

enum iso_sw {
	SW_NO_ERROR = 0x9000,
	SW_FUNC_NOT_SUPPORTED = 0x6A81,
	SW_CONDITIONS_NOT_SATISFIED = 0x6985,
	SW_SECURITY_STATUS_NOT_SATISFIED = 0x6982,
	SW_BYTES_REMAINING_00 = 0x6100,
	SW_CORRECT_LE_00 = 0x6C00,
	SW_WARNING_NO_CHANGE_00 = 0x6200,
	SW_WARNING_EOF = 0x6282,
	SW_WARNING_00 = 0x6300,
	SW_FILE_NOT_FOUND = 0x6A82,
	SW_INCORRECT_PIN = 0x63C0,
	SW_INCORRECT_P1P2 = 0x6A86,
	SW_WRONG_DATA = 0x6A80,
	SW_OUT_OF_MEMORY = 0x6A84,
	SW_WRONG_LENGTH = 0x6700,
	SW_INS_NOT_SUP = 0x6D00,
	SW_FILE_INVALID = 0x6983,
};

/*
 * Creates an APDU with the given class, instruction, p1 and p2 values.
 */
struct apdu *piv_apdu_make(enum iso_class cls, enum iso_ins ins, uint8_t p1,
    uint8_t p2);

/*
 * Sets the command data for an apdu. The command data is not copied, so
 * the data behind this pointer must remain valid until the apdu has been used
 * and released by calling piv_apdu_free().
 */
void piv_apdu_set_cmd(struct apdu *apdu, const uint8_t *data, size_t len);

/*
 * Retrieves the status word from a completed APDU.
 *
 * Returns 0 if the APDU has not been completed.
 */
uint16_t piv_apdu_sw(const struct apdu *apdu);
/*
 * Retrieves a reference to the reply data from a completed APDU, writing
 * the size in "len". The status word is not included.
 *
 * This pointer is only valid until piv_apdu_free() is called.
 */
const uint8_t *piv_apdu_get_reply(const struct apdu *apdu, size_t *len);

/* Frees an APDU and any reply data held by it. */
void piv_apdu_free(struct apdu *pdu);

/*
 * Transceives a single APDU with a given token, sending the command and
 * receiving a response. If ERRF_OK is returned, the APDU is then completed
 * and piv_apdu_sw() and piv_apdu_get_reply() can be used on it.
 */
MUST_CHECK
errf_t *piv_apdu_transceive(struct piv_token *pk, struct apdu *pdu);
/*
 * Transceives a chain of APDUs, allowing both the command data and reply data
 * to span multiple APDUs. The struct apdu will be used and filled out as if
 * one single large APDU had been transceived.
 */
MUST_CHECK
errf_t *piv_apdu_transceive_chain(struct piv_token *pk, struct apdu *apdu);

/*
 * If you set this to B_TRUE, we will bunyan_log the full contents of all APDUs,
 * including sensitive information! Be careful!
 */
extern boolean_t piv_full_apdu_debug;

#endif
