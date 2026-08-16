/* Tests for the token layer (#46, #53, #54).
 *
 * Needs no runner, no socket and no skip path: token.c takes its secret as
 * (pointer, length) and its clock as an explicit time_t, which was chosen for
 * exactly this.
 *
 * THIS TARGET IS ALSO AN ARCHITECTURAL ASSERTION. It compiles against
 * src/libtetrisauth/lib/token.c and links -lcrypto only, so an
 * #include "libtetrissh/..." in it stops it linking. The portability claim
 * that file rests on is a build failure rather than a review comment.
 *
 * Every case here is black box through token_mint() and token_verify().
 *
 * WHAT THIS SUITE LOST WHEN THE FORMAT STOPPED BEING A JWT. The cases for
 * alg:none, for other algs, and for a third header parameter are gone, and
 * they are not coming back: the format carries no header and names no
 * algorithm, so there is no downgrade to attempt and nothing to assert. The
 * base64url padding and alphabet cases went with the encoding. Their loss is
 * the point of the change rather than a gap in it - but a reader comparing
 * this file against its history should see the deletions accounted for here
 * and not assume coverage was quietly dropped.
 *
 * WHAT REPLACED THEM. The old format ignored claims it did not understand,
 * per RFC 7519 s4; this one has a fixed field count, so a sixth field is a
 * rejection - see test_extra_field_rejected(). And where key order used to be
 * the way to restate the same claims, a field swap is - see
 * test_swapped_fields_fail().
 *
 * Run from the repo root: make test */
#include <stdio.h>
#include "test_output.h"
#include <string.h>

#include <openssl/hmac.h>

#include "auth.h"

/* The secret every case signs and verifies under: 32 bytes, the RFC 7518 3.2
 * minimum. Fixed rather than random, because the known-answer vector below
 * depends on its exact bytes. */
#define SECRET "0123456789abcdef0123456789abcdef"
#define SECRET_LEN (sizeof(SECRET) - 1)

/* The clock, injected rather than read: iat, an exp one hour later, and a now
 * that sits before it. Every case that is not about expiry uses NOW, so a
 * failure there is never an expiry failure in disguise. */
#define IAT 1700000000LL
#define EXP 1700003600LL
#define NOW ((time_t)1700000000)

static const unsigned char *secret = (const unsigned char *)SECRET;

static int tests_run = 0, tests_failed = 0;

#define CHECK(cond, msg)                                                       \
    do                                                                         \
    {                                                                          \
        if (!(cond))                                                           \
        {                                                                      \
            test_output_failure_detail(msg, __FILE__, __LINE__);               \
            return -1;                                                         \
        }                                                                      \
    } while (0)

static void run(const char *name, int (*fn)(void))
{
    tests_run++;
    if (fn() != 0)
    {
        tests_failed++;
        test_output_fail(name);
    }
    else
        test_output_pass(name);
}

/* === Building tokens token_mint() would refuse to produce === */

/*
 * Sign whatever claim text it is handed and append the MAC, producing a token
 * that is valid in every way except for whatever the caller made wrong.
 *
 * This is what lets the field-shape cases reach the code they were written
 * for. Without it they would die at the MAC check, pass, and test nothing.
 *
 * Deliberately a second, independent hex encoder rather than a call into
 * token.c: if this one and the library's ever disagree, the known-answer
 * vector is the third opinion that says which is wrong.
 */
static void build_signed(char *out, const char *claims,
                         const unsigned char *sec, size_t sec_len)
{
    static const char DIGITS[] = "0123456789abcdef";
    size_t n = strlen(claims);
    unsigned char mac[32];
    unsigned int mac_len = 0;

    memcpy(out, claims, n);
    HMAC(EVP_sha256(), sec, (int)sec_len, (const unsigned char *)claims, n, mac,
         &mac_len);

    for (size_t i = 0; i < sizeof mac; i++)
    {
        out[n + i * 2] = DIGITS[mac[i] >> 4];
        out[n + i * 2 + 1] = DIGITS[mac[i] & 0x0f];
    }
    out[n + sizeof mac * 2] = '\0';
}

/* Same, but with the MAC field supplied verbatim, so one token's signature can
 * be pasted onto another token's claims. */
static void build_with_sig(char *out, const char *claims, const char *sig)
{
    strcpy(out, claims);
    strcat(out, sig);
}

/* Locate field 0..4 of a token: returns a pointer to its first character and
 * writes its length. */
static const char *field(const char *token, int which, size_t *len)
{
    const char *p = token;
    for (int i = 0; i < which; i++)
        p = strchr(p, '\n') + 1;
    const char *sep = strchr(p, '\n');
    *len = sep != NULL ? (size_t)(sep - p) : strlen(p);
    return p;
}

/*
 * Corrupt one character in the middle of a field, replacing it with a
 * different character OF THE SAME CLASS so the token stays structurally
 * well-formed and only the bytes the MAC covers change. That is what makes the
 * expected answer TOKEN_E_SIGNATURE rather than TOKEN_E_MALFORMED.
 */
static void flip_in_field(char *token, int which)
{
    size_t len;
    char *p = (char *)field(token, which, &len);
    size_t at = len / 2;
    if (p[at] >= '0' && p[at] <= '9')
        p[at] = p[at] == '0' ? '1' : '0';
    else
        p[at] = p[at] == 'a' ? 'b' : 'a';
}

/* The one good token the tampering cases start from. The claim text it
 * produces is spelled out in test_known_answer(), and the near-miss cases
 * spell their own. */
static int mint_canonical(char *tok)
{
    token_claims_t c = {0};
    c.sub = 42;
    strcpy(c.name, "alice");
    c.iat = IAT;
    c.exp = EXP;
    return token_mint(tok, TOKEN_MAX_LEN, &c, secret, SECRET_LEN);
}

/* === Round trip and encoding === */

/* Mint a token, verify it, and check all four claims come back unchanged. */
static int test_round_trip(void)
{
    char tok[TOKEN_MAX_LEN];
    CHECK(mint_canonical(tok) == 0, "mint failed");

    token_claims_t got;
    CHECK(token_verify(tok, secret, SECRET_LEN, NOW, &got) == TOKEN_OK,
          "verify rejected our own token");
    CHECK(got.sub == 42, "sub did not survive");
    CHECK(strcmp(got.name, "alice") == 0, "name did not survive");
    CHECK(got.iat == IAT, "iat did not survive");
    CHECK(got.exp == EXP, "exp did not survive");
    return 0;
}

/*
 * One hardcoded secret/claims/expected-token vector, computed outside this
 * tree with `openssl dgst -sha256 -mac HMAC`, so a refactor that silently
 * changes the format or the signed input fails loudly. The claim text is
 * spelled out character by character because the separators and their exact
 * placement are the format.
 */
static int test_known_answer(void)
{
    static const char EXPECTED[] =
        "42\nalice\n1700000000\n1700003600\n"
        "4431a86c4ffd6a7419156456e1b9c23abbbc752d9955e0d34f13e15d89ddc008";

    char tok[TOKEN_MAX_LEN];
    CHECK(mint_canonical(tok) == 0, "mint failed");
    CHECK(strcmp(tok, EXPECTED) == 0, "minted token differs from the vector");
    return 0;
}

/* The MAC is lowercase hex and nothing else. Pinned separately from the vector
 * because the vector would still pass if only its own digits happened to
 * agree. */
static int test_mac_is_lowercase_hex(void)
{
    char tok[TOKEN_MAX_LEN];
    CHECK(mint_canonical(tok) == 0, "mint failed");

    size_t len;
    const char *sig = field(tok, 4, &len);
    CHECK(len == TOKEN_SIG_HEX_LEN, "the MAC field is not 64 characters");
    for (size_t i = 0; i < len; i++)
        CHECK((sig[i] >= '0' && sig[i] <= '9') ||
                  (sig[i] >= 'a' && sig[i] <= 'f'),
              "the MAC field contains something that is not lowercase hex");
    return 0;
}

/* Names of 1 to 15 characters, which is #47's whole range. */
static int test_every_name_length_round_trips(void)
{
    for (size_t n = 1; n <= TOKEN_NAME_MAX; n++)
    {
        token_claims_t c = {0};
        c.sub = 7;
        memset(c.name, 'a', n);
        c.name[n] = '\0';
        c.iat = IAT;
        c.exp = EXP;

        char tok[TOKEN_MAX_LEN];
        CHECK(token_mint(tok, sizeof tok, &c, secret, SECRET_LEN) == 0,
              "mint failed for some name length");

        token_claims_t got;
        CHECK(token_verify(tok, secret, SECRET_LEN, NOW, &got) == TOKEN_OK,
              "verify failed for some name length");
        CHECK(strcmp(got.name, c.name) == 0, "name did not survive a length");
    }
    return 0;
}

/* The widest claims #47 and a long long allow must still fit TOKEN_MAX_LEN.
 * The header claims 143 bytes worst case; this is the case that would catch
 * that arithmetic being wrong. */
static int test_widest_claims_still_fit(void)
{
    token_claims_t c = {0};
    c.sub = -9223372036854775807LL - 1;
    memset(c.name, 'a', TOKEN_NAME_MAX);
    c.name[TOKEN_NAME_MAX] = '\0';
    c.iat = -9223372036854775807LL - 1;
    c.exp = 9223372036854775807LL;

    char tok[TOKEN_MAX_LEN];
    CHECK(token_mint(tok, sizeof tok, &c, secret, SECRET_LEN) == 0,
          "the widest possible claims did not fit");

    /* LLONG_MIN does not round trip - parse_ll() rejects it as overflow - so
     * this asserts the mint side only, which is what the bound is about. */
    CHECK(strlen(tok) < TOKEN_MAX_LEN, "the widest token overran its bound");
    return 0;
}

/* === Tampering === */

/* One character changed in each of the five fields in turn. All five must come
 * back TOKEN_E_SIGNATURE. */
static int test_flipped_byte_in_each_field(void)
{
    for (int f = 0; f < 5; f++)
    {
        char tok[TOKEN_MAX_LEN];
        CHECK(mint_canonical(tok) == 0, "mint failed");
        flip_in_field(tok, f);

        token_claims_t got;
        /* The exact code matters: if a flipped MAC came back malformed the
         * case would pass without ever reaching the compare. */
        CHECK(token_verify(tok, secret, SECRET_LEN, NOW, &got) ==
                  TOKEN_E_SIGNATURE,
              "a flipped byte was not reported as a signature failure");
    }
    return 0;
}

/* A good token presented under a different key. */
static int test_wrong_secret(void)
{
    char tok[TOKEN_MAX_LEN];
    CHECK(mint_canonical(tok) == 0, "mint failed");

    const unsigned char other[] = "0123456789abcdef0123456789abcdeF";
    token_claims_t got;
    CHECK(token_verify(tok, other, sizeof other - 1, NOW, &got) ==
              TOKEN_E_SIGNATURE,
          "a token verified under the wrong secret");
    return 0;
}

/*
 * A MAC field that is not exactly 64 characters, short and long.
 *
 * NOTE THE CODE CHANGED HERE. Under the old format a short signature decoded
 * to 31 bytes and was refused on length by the signature check, so it came
 * back E_SIGNATURE. The width is now a structural property of the field, so it
 * is caught by the split and comes back E_MALFORMED. Nothing is weaker - the
 * rejection simply moved earlier.
 */
static int test_signature_of_the_wrong_length(void)
{
    char tok[TOKEN_MAX_LEN];
    token_claims_t got;

    CHECK(mint_canonical(tok) == 0, "mint failed");
    tok[strlen(tok) - 1] = '\0';
    CHECK(token_verify(tok, secret, SECRET_LEN, NOW, &got) == TOKEN_E_MALFORMED,
          "a short MAC was not rejected on width");

    CHECK(mint_canonical(tok) == 0, "mint failed");
    strcat(tok, "0");
    CHECK(token_verify(tok, secret, SECRET_LEN, NOW, &got) == TOKEN_E_MALFORMED,
          "a long MAC was not rejected on width");
    return 0;
}

/*
 * A MAC of the right width carrying characters that are not hex.
 *
 * This is E_SIGNATURE rather than E_MALFORMED on purpose: verification
 * compares the hex text instead of decoding it, so there is no decoder to
 * reject the character and a non-hex MAC is simply a MAC that does not match.
 * The case exists to pin that choice, which is also what makes an
 * uppercase-hex spelling of a correct MAC fail.
 */
static int test_non_canonical_mac_spellings(void)
{
    char tok[TOKEN_MAX_LEN];
    token_claims_t got;
    size_t len;

    CHECK(mint_canonical(tok) == 0, "mint failed");
    ((char *)field(tok, 4, &len))[0] = 'z';
    CHECK(token_verify(tok, secret, SECRET_LEN, NOW, &got) ==
              TOKEN_E_SIGNATURE,
          "a non-hex MAC character was not a mismatch");

    /* The same MAC, correct in value, spelled in uppercase. */
    CHECK(mint_canonical(tok) == 0, "mint failed");
    char *sig = (char *)field(tok, 4, &len);
    for (size_t i = 0; i < len; i++)
        if (sig[i] >= 'a' && sig[i] <= 'f')
            sig[i] = (char)(sig[i] - 'a' + 'A');
    CHECK(token_verify(tok, secret, SECRET_LEN, NOW, &got) ==
              TOKEN_E_SIGNATURE,
          "an uppercase spelling of a correct MAC verified");
    return 0;
}

/* Tokens that are not five fields: empty, too few, too many, and a separator
 * inside the MAC field. */
static int test_malformed_shapes(void)
{
    token_claims_t got;

    CHECK(token_verify("", secret, SECRET_LEN, NOW, &got) == TOKEN_E_MALFORMED,
          "empty token accepted");
    CHECK(token_verify("42\nalice\n1\n2", secret, SECRET_LEN, NOW, &got) ==
              TOKEN_E_MALFORMED,
          "four fields accepted");

    /* Six fields, correctly signed over the first five, so this dies on the
     * field count rather than on the MAC. */
    char tok[TOKEN_MAX_LEN];
    build_signed(tok, "42\nalice\n1700000000\n1700003600\nextra\n", secret,
                 SECRET_LEN);
    CHECK(token_verify(tok, secret, SECRET_LEN, NOW, &got) == TOKEN_E_MALFORMED,
          "six fields accepted");

    /* A separator inside the MAC field, which is the same rejection reached by
     * a different route. */
    CHECK(mint_canonical(tok) == 0, "mint failed");
    size_t len;
    ((char *)field(tok, 4, &len))[len / 2] = '\n';
    CHECK(token_verify(tok, secret, SECRET_LEN, NOW, &got) == TOKEN_E_MALFORMED,
          "a separator inside the MAC field was accepted");
    return 0;
}

/*
 * A sixth field, properly signed, must be REJECTED.
 *
 * This is the deliberate reversal of the old format's behaviour: RFC 7519 s4
 * required unknown claims to be ignored, and the suite had a case asserting a
 * fifth claim verified. The field count is fixed now, so extra data is a
 * rejection - the strictly safer answer, and one this format can afford
 * because it has no interoperability obligation.
 */
static int test_extra_field_rejected(void)
{
    char tok[TOKEN_MAX_LEN];
    build_signed(tok, "42\nalice\n1700000000\n1700003600\nadmin\n", secret,
                 SECRET_LEN);

    token_claims_t got;
    CHECK(token_verify(tok, secret, SECRET_LEN, NOW, &got) == TOKEN_E_MALFORMED,
          "an extra field was accepted");
    return 0;
}

/* === Claims === */

/* The three points around exp: one second before passes, now == exp fails,
 * one second after fails. */
static int test_expiry_boundary(void)
{
    char tok[TOKEN_MAX_LEN];
    CHECK(mint_canonical(tok) == 0, "mint failed");

    token_claims_t got;
    CHECK(token_verify(tok, secret, SECRET_LEN, (time_t)(EXP - 1), &got) ==
              TOKEN_OK,
          "one second before exp was rejected");
    /* Leeway is zero and the kept wording is "on or after", so this is the
     * edge that must not drift. */
    CHECK(token_verify(tok, secret, SECRET_LEN, (time_t)EXP, &got) ==
              TOKEN_E_EXPIRED,
          "now == exp was accepted");
    CHECK(token_verify(tok, secret, SECRET_LEN, (time_t)(EXP + 1), &got) ==
              TOKEN_E_EXPIRED,
          "an expired token was accepted");
    return 0;
}

/* A properly signed token with no exp field. Under the old format this was a
 * missing optional claim and had to be caught by a rule; here it is one field
 * short and cannot be spelled at all. The case is kept because the property it
 * protects - no token is eternal - is the same. */
static int test_missing_exp_is_not_eternal(void)
{
    char tok[TOKEN_MAX_LEN];
    build_signed(tok, "42\nalice\n1700000000\n", secret, SECRET_LEN);

    token_claims_t got;
    CHECK(token_verify(tok, secret, SECRET_LEN, NOW, &got) == TOKEN_E_MALFORMED,
          "a token with no exp was accepted");
    return 0;
}

/*
 * The regression guard for someone restructuring token_verify(): the signature
 * covers the received bytes, so the same values in different positions must
 * fail. iat and exp are swapped here, which is the nearest thing this format
 * has to the old reordered-keys case.
 */
static int test_swapped_fields_fail(void)
{
    char tok[TOKEN_MAX_LEN];
    CHECK(mint_canonical(tok) == 0, "mint failed");

    size_t len;
    const char *sig = field(tok, 4, &len);
    char sig_field[TOKEN_SIG_HEX_LEN + 1];
    memcpy(sig_field, sig, len);
    sig_field[len] = '\0';

    char swapped[TOKEN_MAX_LEN];
    build_with_sig(swapped, "42\nalice\n1700003600\n1700000000\n", sig_field);

    token_claims_t got;
    CHECK(token_verify(swapped, secret, SECRET_LEN, NOW, &got) ==
              TOKEN_E_SIGNATURE,
          "reordered claim values verified under the original signature");
    return 0;
}

/* Claim texts that are signed correctly and still wrong: leading zeros, a '+'
 * sign, surrounding space, an empty field, a non-integer, a 16 character name,
 * and a name outside #47's allowlist. */
static int test_bad_claim_shapes(void)
{
    struct
    {
        const char *claims;
        token_result_t want;
        const char *msg;
    } cases[] = {
        {"042\nalice\n1700000000\n1700003600\n", TOKEN_E_CLAIMS,
         "a leading zero was accepted"},
        {"+42\nalice\n1700000000\n1700003600\n", TOKEN_E_CLAIMS,
         "a '+' sign was accepted"},
        {"42\nalice\n1700000000\n 1700003600\n", TOKEN_E_CLAIMS,
         "a space before a claim was accepted"},
        {"\nalice\n1700000000\n1700003600\n", TOKEN_E_CLAIMS,
         "an empty sub was accepted"},
        {"42\nalice\n1700000000\n1.5\n", TOKEN_E_CLAIMS,
         "a non-integer exp was accepted"},
        {"42\nalice\n1700000000\n99999999999999999999\n", TOKEN_E_CLAIMS,
         "an out-of-range exp was accepted"},
        {"42\nabcdefghijklmnop\n1700000000\n1700003600\n", TOKEN_E_CLAIMS,
         "a 16 character name was accepted"},
        {"42\n\n1700000000\n1700003600\n", TOKEN_E_CLAIMS,
         "an empty name was accepted"},
        {"42\nali ce\n1700000000\n1700003600\n", TOKEN_E_CLAIMS,
         "a name outside #47's allowlist was accepted"},
    };

    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++)
    {
        char tok[TOKEN_MAX_LEN];
        build_signed(tok, cases[i].claims, secret, SECRET_LEN);
        token_claims_t got;
        CHECK(token_verify(tok, secret, SECRET_LEN, NOW, &got) == cases[i].want,
              cases[i].msg);
    }
    return 0;
}

/* === token_mint's own refusals === */

/*
 * The emitter has no escaper and is safe only by virtue of #47's allowlist, so
 * it re-validates rather than trusting its caller. The newline is the case
 * that matters most now: it is the delimiter, so a name carrying one would be
 * claim injection rather than merely a malformed field.
 */
static int test_mint_rejects_names_outside_the_allowlist(void)
{
    static const char *bad[] = {"ali\nce", "ali ce", "ali.ce", "ali\"ce",
                                "ali\\ce", ""};

    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++)
    {
        token_claims_t c = {0};
        c.sub = 1;
        strcpy(c.name, bad[i]);
        c.iat = IAT;
        c.exp = EXP;

        char tok[TOKEN_MAX_LEN];
        CHECK(token_mint(tok, sizeof tok, &c, secret, SECRET_LEN) == -1,
              "a name outside the allowlist was minted");
        CHECK(tok[0] == '\0', "a rejected mint left something in the buffer");
    }
    return 0;
}

/* A name filling all 16 bytes of the field with no NUL. */
static int test_mint_rejects_an_unterminated_name(void)
{
    token_claims_t c = {0};
    c.sub = 1;
    memset(c.name, 'a', sizeof c.name); /* all 16 bytes, no NUL */
    c.iat = IAT;
    c.exp = EXP;

    char tok[TOKEN_MAX_LEN];
    CHECK(token_mint(tok, sizeof tok, &c, secret, SECRET_LEN) == -1,
          "an unterminated name was minted");
    return 0;
}

/* A 31 byte secret, on both sides: mint must refuse it and verify must not
 * return TOKEN_OK under it. */
static int test_short_secret(void)
{
    const unsigned char short_secret[] =
        "0123456789abcdef0123456789abcde"; /*31*/
    token_claims_t c = {0};
    c.sub = 1;
    strcpy(c.name, "alice");
    c.iat = IAT;
    c.exp = EXP;

    char tok[TOKEN_MAX_LEN];
    CHECK(token_mint(tok, sizeof tok, &c, short_secret,
                     sizeof short_secret - 1) == -1,
          "minted under a secret shorter than the hash output");

    CHECK(mint_canonical(tok) == 0, "mint failed");
    token_claims_t got;
    CHECK(token_verify(tok, short_secret, sizeof short_secret - 1, NOW, &got) !=
              TOKEN_OK,
          "verified under a secret shorter than the hash output");
    return 0;
}

/* An output buffer too small for the token: mint refuses rather than
 * truncating, and leaves nothing behind in it. */
static int test_mint_refuses_a_small_buffer(void)
{
    token_claims_t c = {0};
    c.sub = 42;
    strcpy(c.name, "alice");
    c.iat = IAT;
    c.exp = EXP;

    char tok[64];
    CHECK(token_mint(tok, sizeof tok, &c, secret, SECRET_LEN) == -1,
          "a token was written into a buffer too small for it");
    CHECK(tok[0] == '\0', "a rejected mint left something in the buffer");
    return 0;
}

int main(void)
{
    test_output_begin("test_token");

    run("mints and verifies its own token", test_round_trip);
    run("matches the known-answer vector", test_known_answer);
    run("writes the MAC as lowercase hex", test_mac_is_lowercase_hex);
    run("round trips every allowed name length",
        test_every_name_length_round_trips);
    run("fits the widest claims it can be handed", test_widest_claims_still_fit);

    run("rejects a flipped byte in each field", test_flipped_byte_in_each_field);
    run("rejects a token signed under another secret", test_wrong_secret);
    run("rejects a MAC that is not 64 characters",
        test_signature_of_the_wrong_length);
    run("rejects non-canonical MAC spellings", test_non_canonical_mac_spellings);
    run("rejects malformed field shapes", test_malformed_shapes);
    run("rejects an extra field rather than ignoring it",
        test_extra_field_rejected);

    run("expires on or after exp, with no leeway", test_expiry_boundary);
    run("treats a missing exp as a failure, not as eternal",
        test_missing_exp_is_not_eternal);
    run("rejects claim values moved between fields", test_swapped_fields_fail);
    run("rejects malformed and out-of-range claims", test_bad_claim_shapes);

    run("refuses to mint a name outside #47's allowlist",
        test_mint_rejects_names_outside_the_allowlist);
    run("refuses to mint an unterminated name",
        test_mint_rejects_an_unterminated_name);
    run("refuses a secret shorter than the hash output", test_short_secret);
    run("refuses an output buffer too small", test_mint_refuses_a_small_buffer);

    test_output_summary(tests_run, tests_failed, 0);
    return tests_failed == 0 ? 0 : 1;
}
