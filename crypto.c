/* CRYPTO.C     (C) Copyright Jan Jaeger, 2000-2012                  */
/*              (C) Copyright "Fish" (David B. Trout), 2018          */
/*              Hercules Crypto support                              */
/*                                                                   */
/*   Released under "The Q Public License Version 1"                 */
/*   (http://www.hercules-390.org/herclic.html) as modifications to  */
/*   Hercules.                                                       */

#include "hstdinc.h"

#define _CRYPTO_C_
#define _HENGINE_DLL_

#include "hercules.h"
#include "opcode.h"
#include "crypto.h"

//#define    WRAPPINGKEYS_DEBUG       // (#define for debugging)

/*-------------------------------------------------------------------*/
/*          (delineates ARCH_DEP from non-arch_dep)                  */
/*-------------------------------------------------------------------*/

#if !defined( _GEN_ARCH )

  #if defined(              _ARCH_NUM_1 )
    #define   _GEN_ARCH     _ARCH_NUM_1
    #include "crypto.c"
  #endif

  #if defined(              _ARCH_NUM_2 )
    #undef    _GEN_ARCH
    #define   _GEN_ARCH     _ARCH_NUM_2
    #include "crypto.c"
  #endif

/*-------------------------------------------------------------------*/
/*            (delineates ARCH_DEP from non-arch_dep)                */
/*-------------------------------------------------------------------*/

/*-------------------------------------------------------------------*/
/*  non-ARCH_DEP section: compiled only ONCE after last arch built   */
/*-------------------------------------------------------------------*/
/*  Note: the last architecture has been built so the normal non-    */
/*  underscore FEATURE values are now #defined according to the      */
/*  LAST built architecture just built (usually zarch = 900). This   */
/*  means from this point onward (to the end of file) you should     */
/*  ONLY be testing the underscore _FEATURE values to see if the     */
/*  given feature was defined for *ANY* of the build architectures.  */
/*-------------------------------------------------------------------*/

/*********************************************************************/
/*                  IMPORTANT PROGRAMMING NOTE!                      */
/*********************************************************************/
/*                                                                   */
/* It is CRITICALLY IMPORTANT to not use any architecture dependent  */
/* macros anywhere in any of your non-arch_dep functions! This means */
/* you CANNOT use GREG, RADR, VADR, etc. anywhere in your function!  */
/*                                                                   */
/* Basically you MUST NOT use any architecture dependent macro that  */
/* is #defined in the "feature.h" header.  If you you need to use    */
/* any of them, then your function MUST be an "ARCH_DEP" function    */
/* that is placed within the ARCH_DEP section at the beginning of    */
/* this module where it can be compiled multiple times, once for     */
/* each of the supported architectures so the macro gets #defined    */
/* to its proper value for the architecture! YOU HAVE BEEN WARNED!   */
/*                                                                   */
/*********************************************************************/

#if defined( _FEATURE_076_MSA_EXTENSION_FACILITY_3 )

/*-------------------------------------------------------------------*/
/*           Open and initialize a CSRNG provider                    */
/*-------------------------------------------------------------------*/
bool hopen_CSRNG()
{
    if (!sysblk.wkrandhand)
    {
#if !defined( NEED_CSRNG_INIT )

        sysblk.wkrandhand = DUMMY_CYRPTO_HANDLE;

#elif defined( USE_RAND_API )

        int i, randval;

        srand( (unsigned) time(0) );

        for (i=0; i < 256; i++)
        {
            randval = (int) rand() * (int) (host_tod() & 0xFFFFFFFF);
            srand( (unsigned) randval );
        }

        sysblk.wkrandhand = DUMMY_CYRPTO_HANDLE;

#elif defined( USE_DEV_RANDOM )

        int fd, rc;

        do fd = open( "/dev/random", O_RDONLY );
        while (fd < 0 && errno == EINTR);

        if (0
            || fd < 0
            || ioctl( fd, RNDGETENTCNT, &rc ) < 0
        )
        {
            // "Crypto: '%s' failed: %s"
            WRMSG( HHC01494, "E", "ioctl()", strerror( errno ));
            return false;
        }

        sysblk.wkrandhand = fd;

        if (rc < MIN_ENTROPY_BITS)
        {
            /* Use poll(), just like libsodium, since
               we do not want to read from the device.
            */
            struct pollfd pfd;

            pfd.fd      = fd;
            pfd.events  = POLLIN;

            do rc = poll( &pfd, 1, -1 );
            while
            (0
                || (rc < 0 && (errno == EINTR || errno == EAGAIN))
                || (1
                    && ioctl( fd, RNDGETENTCNT, &rc ) >= 0
                    && rc < MIN_ENTROPY_BITS
                   )
            );

            if (rc < 0)
            {
                // "Crypto: '%s' failed: %s"
                WRMSG( HHC01494, "E", "poll()", strerror( errno ));
                hclose_CSRNG();
                return false;
            }
        }

#else // defined( _WIN32 )

        NTSTATUS  ntStatus  = BCryptOpenAlgorithmProvider
        (
            (BCRYPT_ALG_HANDLE*) &sysblk.wkrandhand,
            BCRYPT_RNG_ALGORITHM,
            MS_PRIMITIVE_PROVIDER,
            0
        );

        if (!NT_SUCCESS( ntStatus ))
        {
            // "Crypto: '%s' failed: %s"
            WRMSG( HHC01494, "E", "BCryptOpenAlgorithmProvider()",
                strerror( w32_NtStatusToLastError( ntStatus )));
            sysblk.wkrandhand = 0;
            return false;
        }
#endif
    }

    return true;
}

/*-------------------------------------------------------------------*/
/*           Close and de-initialize a CSRNG provider                */
/*-------------------------------------------------------------------*/
bool hclose_CSRNG()
{
    if (sysblk.wkrandhand)
    {
#if defined( USE_DEV_RANDOM )

        int rc, fd = sysblk.wkrandhand;

        do rc = close( fd );
        while (rc < 0 && errno == EINTR);

        if (rc < 0)
        {
            // "Crypto: '%s' failed: %s"
            WRMSG( HHC01494, "E", "close()", strerror( errno ));
            return false;
        }

#elif defined( _WIN32 )

        NTSTATUS  ntStatus  = BCryptCloseAlgorithmProvider
        (
            sysblk.wkrandhand,
            0
        );

        if (!NT_SUCCESS( ntStatus ))
        {
            // "Crypto: '%s' failed: %s"
            WRMSG( HHC01494, "E", "BCryptCloseAlgorithmProvider()",
                strerror( w32_NtStatusToLastError( ntStatus )));
            return false;
        }
#endif

        sysblk.wkrandhand = 0;
    }

    return true;
}

/*-------------------------------------------------------------------*/
/*           Obtain random bytes from CSRNG provider                 */
/*-------------------------------------------------------------------*/
bool hget_random_bytes( BYTE* buf, size_t amt )
{
#if defined( USE_ARC4RANDOM )

    arc4random_buf( buf, amt );

#elif defined( USE_SYS_GETRANDOM )

    size_t   chunk, offset = 0;
    ssize_t  rc;

    while (amt > 0)
    {
        chunk = (amt <= MAX_CSRNG_BYTES) ? amt : MAX_CSRNG_BYTES;

        do rc = syscall( SYS_getrandom, buf + offset, chunk, 0 );
        while (rc < 0 && errno == EINTR);

        if (rc < 0)
        {
            // "Crypto: '%s' failed: %s"
            WRMSG( HHC01494, "E", "syscall()", strerror( errno ));
            return false;
        }

        offset  +=  rc;
        amt     -=  rc;
    }

#elif defined( USE_DEV_RANDOM )

    size_t   chunk, offset = 0;
    ssize_t  rc;

    while (amt > 0)
    {
        chunk = (amt <= MAX_CSRNG_BYTES) ? amt : MAX_CSRNG_BYTES;

        do rc = read( sysblk.wkrandhand, buf + offset, chunk );
        while (rc < 0 && (errno == EAGAIN || errno == EINTR));

        if (rc < 0)
        {
            // "Crypto: '%s' failed: %s"
            WRMSG( HHC01494, "E", "read()", strerror( errno ));
            return false;
        }

        offset  +=  rc;
        amt     -=  rc;
    }

#elif defined( USE_RAND_API )

    while (amt--)
        *buf++ = (rand() & 0xFF);

#else // defined( _WIN32 )

    NTSTATUS ntStatus = BCryptGenRandom( sysblk.wkrandhand, buf, amt, 0 );

    if (!NT_SUCCESS( ntStatus ))
    {
        // "Crypto: '%s' failed: %s"
        WRMSG( HHC01494, "E", "BCryptGenRandom()",
            strerror( w32_NtStatusToLastError( ntStatus )));
        return false;
    }

#endif

    return true;
}

/*-------------------------------------------------------------------*/
/*               Function: renew_wrapping_keys                       */
/*-------------------------------------------------------------------*/
/* Each time a clear reset is performed a new set of wrapping keys   */
/* and their associated verification patterns are generated. The     */
/* contents of the two wrapping-key registers are kept internal to   */
/* the model so that no program, including the operating system,     */
/* can directly observe their clear value.                           */
/*-------------------------------------------------------------------*/
void renew_wrapping_keys()
{
    U64     cpuid;
    BYTE    lparname[8];
    BYTE    randbytes[32];
    size_t  i, idx = 0;
    BYTE    lparnum1;

    CASSERT( sizeof( sysblk.wkvpaes_reg ) >= sizeof( cpuid ) + sizeof( lparname ) + 1, crypto_c );
    CASSERT( sizeof( sysblk.wkvpdea_reg ) >= sizeof( cpuid ) + sizeof( lparname ) + 1, crypto_c );

    /* Gather needed data */

    cpuid = sysblk.cpuid;
    lparnum1 = sysblk.lparnum & 0xff;
    get_lparname( lparname );

    VERIFY( hopen_CSRNG() );
    {
        VERIFY( hget_random_bytes( sysblk.wkaes_reg, sizeof( sysblk.wkaes_reg )));
        VERIFY( hget_random_bytes( sysblk.wkdea_reg, sizeof( sysblk.wkdea_reg )));
        VERIFY( hget_random_bytes( randbytes,        sizeof( randbytes        )));
    }
    VERIFY( hclose_CSRNG() );

    /*
    **  We set the verification pattern to:
    **
    **    CPUID         (8 bytes)
    **    LPAR Name     (8 bytes)
    **    LPAR Number   (1 byte)   (low order)
    **    Random bytes  (n bytes)  (remainder)
    */

    memset( sysblk.wkvpaes_reg, 0, sizeof( sysblk.wkvpaes_reg ));
    memset( sysblk.wkvpdea_reg, 0, sizeof( sysblk.wkvpdea_reg ));

    /* CPUID */

    idx += sizeof( cpuid );  /* (since it's processed right to left) */

    for (i=0; i < sizeof( cpuid ); i++)
    {
        sysblk.wkvpaes_reg[ idx - 1 - i ] = cpuid & 0xff;
        sysblk.wkvpdea_reg[ idx - 1 - i ] = cpuid & 0xff;
        cpuid >>= 8;
    }

    /* LPAR Name */

    memcpy( &sysblk.wkvpaes_reg[ idx ], lparname, sizeof( lparname ));
    memcpy( &sysblk.wkvpdea_reg[ idx ], lparname, sizeof( lparname ));

    idx += sizeof( lparname );  /* (next field starts past this one) */

    /* LPAR Number */

    sysblk.wkvpaes_reg[ idx ] = lparnum1;
    sysblk.wkvpdea_reg[ idx ] = lparnum1;

    idx += 1;

    /* Random bytes (if there's room) */

    for (i=0; i < sizeof( sysblk.wkvpaes_reg ) - idx; i++)
                          sysblk.wkvpaes_reg[    idx + i ] = randbytes[i];

    for (i=0; i < sizeof( sysblk.wkvpdea_reg ) - idx; i++)
                          sysblk.wkvpdea_reg[    idx + i ] = randbytes[i];

    /* Display wrapping keys if debugging... */

#if defined( WRAPPINGKEYS_DEBUG )  // (see beginning of source modue)

    {
        char  buf[128] = {0};

#define DBG_WRMSG_CRYPT_REG( dbg_msg, wk_reg )                                                      \
                                                                                                    \
        do                                                                                          \
        {                                                                                           \
            STRLCPY( buf, dbg_msg );                                                                \
                                                                                                    \
            for (i=0; i < sizeof( sysblk.wk_reg ); i++) snprintf( buf   + strlen( buf ),            \
                                                          sizeof( buf ) - strlen( buf ), "%02X",    \
                                  sysblk.wk_reg[i] );                                               \
                                                                                                    \
            WRMSG( HHC90190, "D", buf );                                                            \
        }                                                                                           \
        while (0)

        DBG_WRMSG_CRYPT_REG( "AES wrapping key:    ",   wkaes_reg );
        DBG_WRMSG_CRYPT_REG( "AES wrapping key vp: ", wkvpaes_reg );
        DBG_WRMSG_CRYPT_REG( "DEA wrapping key:    ",   wkdea_reg );
        DBG_WRMSG_CRYPT_REG( "DEA wrapping key vp: ", wkvpdea_reg );
    }

#endif // defined( WRAPPINGKEYS_DEBUG )
}
#endif /* defined( _FEATURE_076_MSA_EXTENSION_FACILITY_3 ) */

#endif /* !defined( _GEN_ARCH ) */
