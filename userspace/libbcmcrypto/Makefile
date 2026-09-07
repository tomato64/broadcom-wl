# Broadcom's crypto primitives, as a static archive.
#
# nas needs AES, AES key wrap, SHA1/SHA256, HMAC, MD5, RC4, PRF, passhash,
# Diffie-Hellman and a bignum implementation. FreshTomato builds these into a
# shared libbcmcrypto.so because httpd and wps link it too; nothing else in
# Tomato64 does, so this is a .a linked straight into nas.
#
# Built from src/ - FreshTomato's src-rt-6.x.4708/bcmcrypto/ - not from the
# prebuilt .o files its tree also ships, which are 2012-toolchain uclibc
# objects.

include ../common.mk

CFLAGS_ALL = $(BCM_CPPFLAGS) -Isrc $(BCM_CFLAGS)

# FreshTomato's object list, verbatim, including the sha256/hmac_sha256 pair
# its comment leaves unconditional.
OBJS := src/aes.o src/aeskeywrap.o src/rijndael-alg-fst.o src/dh.o src/bn.o \
	src/sha1.o src/passhash.o src/prf.o src/md5.o src/hmac.o src/rc4.o \
	src/random.o src/sha256.o src/hmac_sha256.o

all: libbcmcrypto.a

libbcmcrypto.a: $(OBJS)
	$(AR) cr $@ $^

%.o: %.c
	$(CC) $(CFLAGS_ALL) -c -o $@ $<

clean:
	rm -f $(OBJS) libbcmcrypto.a
