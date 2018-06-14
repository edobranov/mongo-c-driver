/*
 * Created by Evgeni Dobranov on 6/14/18.
 *
 * Source: http://www.isthe.com/chongo/src/fnv/hash_32a.c
 */

#include <sys/types.h>
#include "bson-fnv.h"

/*
 * 32 bit FNV-1a non-zero initial basis
 *
 * The FNV-1 initial basis is the FNV-0 hash of the following 32 octets:
 *
 *              chongo <Landon Curt Noll> /\../\
 *
 * NOTE: The \'s above are not back-slashing escape characters.
 * They are literal ASCII  backslash 0x5c characters.
 *
 * NOTE: The FNV-1a initial basis is the same value as FNV-1 by definition.
 */
#define FNV1_32A_INIT ((u_int32_t)0x811c9dc5)

/*
 * 32 bit magic FNV-1a prime
 */
#define FNV_32_PRIME ((Fnv32_t)0x01000193)

/*
 * For producing 24 bit FNV-1a hash using xor-fold on a 32 bit FNV-1a hash
 *
 * Source: http://www.isthe.com/chongo/tech/comp/fnv/index.html#xor-fold
 */
#define MASK_24 (((u_int32_t)1<<24)-1) /* i.e., (u_int32_t)0xffffff */

/*
 * fnv_24a_str - perform a 32 bit Fowler/Noll/Vo FNV-1a hash on a string
 *               and xor-fold it into a 24 bit hash
 *
 * input:
 *  str  - string to hash
 *
 * returns:
 *  24 bit hash as a static hash type
 *
 * NOTE: To use the recommended 32 bit FNV-1a hash, use FNV1_32A_INIT as the
 *       hval arg on the first call to either fnv_32a_buf() or fnv_32a_str().
 */
u_int32_t
fnv_24a_str (char *str, unsigned len)
{
   u_int32_t hval = FNV1_32A_INIT;            /* initial 32 bit hash basis */
   unsigned char *sb = (unsigned char *)str;   /* unsigned string */
   unsigned char *se = sb + len;

   /*
    * FNV-1a hash each octet in the buffer
    */
   while (sb < se) {

      /* xor the bottom with the current octet */
      hval ^= (u_int32_t)*sb++;

      /* multiply by the 32 bit FNV magic prime mod 2^32 */
#if defined(NO_FNV_GCC_OPTIMIZATION)
      hval *= FNV_32_PRIME;
#else
      hval += (hval<<1) + (hval<<4) + (hval<<7) + (hval<<8) + (hval<<24);
#endif
   }

   /* xor-fold the result to a 24 bit value */
   hval = (hval>>24) ^ (hval & MASK_24);

   /* return our new 24 bit hash value */
   return hval;
}