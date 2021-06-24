/*
 * Copyright (c) 2005 Apple Computer, Inc. All rights reserved.
 *
 * @APPLE_LICENSE_HEADER_START@
 * 
 * This file contains Original Code and/or Modifications of Original Code
 * as defined in and that are subject to the Apple Public Source License
 * Version 2.0 (the 'License'). You may not use this file except in
 * compliance with the License. Please obtain a copy of the License at
 * http://www.opensource.apple.com/apsl/ and read it before using this
 * file.
 * 
 * The Original Code and all software distributed under the License are
 * distributed on an 'AS IS' basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE, QUIET ENJOYMENT OR NON-INFRINGEMENT.
 * Please see the License for the specific language governing rights and
 * limitations under the License.
 * 
 * @APPLE_LICENSE_HEADER_END@
 */
/*
 *  CFNetworkInternal.h
 *  CFNetwork
 *
 *  Created by rew on Tue Sep 26 2000.
 *  Copyright (c) 2000-2004 Apple Computer, Inc. All rights reserved.
 *
 */
#ifndef __CFNETWORKINTERNAL__
#define __CFNETWORKINTERNAL__

#include <CoreFoundation/CFRuntime.h>
#include <CFNetwork/CFNetwork.h>
#include <dns_sd.h>
#include "CFNetworkThreadSupport.h"		/* Include here since it used to live here and files rely on that. */
#include "ProxySupport.h"				/* Include here since it used to live here and files rely on that. */


#if defined(__cplusplus)
extern "C" {
#endif
	
// An error domain which is either kCFStreamErrorDomainPOSIX or kCFStreamErrorDomainWinSock, depending on the platform.
#if defined(__WIN32__)
#define _kCFStreamErrorDomainNativeSockets    kCFStreamErrorDomainWinSock
#else
#define _kCFStreamErrorDomainNativeSockets    kCFStreamErrorDomainPOSIX
#endif

	
/* Use CF's logging routine. */
#define __kCFLogAssertion	15
CF_EXPORT void CFLog(int p, CFStringRef str, ...);
    
/* Bit manipulation macros */
/* Bits are numbered from 31 on left to 0 on right */
/* May or may not work if you use them on bitfields in types other than UInt32, bitfields the full width of a UInt32, or anything else for which they were not designed. */
#define __CFBitfieldMask(N1, N2)	((((UInt32)~0UL) << (31UL - (N1) + (N2))) >> (31UL - N1))
#define __CFBitfieldGetValue(V, N1, N2)	(((V) & __CFBitfieldMask(N1, N2)) >> (N2))
#define __CFBitfieldSetValue(V, N1, N2, X)	((V) = ((V) & ~__CFBitfieldMask(N1, N2)) | (((X) << (N2)) & __CFBitfieldMask(N1, N2)))
#define __CFBitfieldMaxValue(N1, N2)	__CFBitfieldGetValue(0xFFFFFFFFUL, (N1), (N2))

#define __CFBitIsSet(V, N)  (((V) & (1UL << (N))) != 0)
#define __CFBitSet(V, N)  ((V) |= (1UL << (N)))
#define __CFBitClear(V, N)  ((V) &= ~(1UL << (N)))

#ifdef __CONSTANT_CFSTRINGS__
#define CONST_STRING_DECL(S, V) const CFStringRef S = (const CFStringRef)__builtin___CFStringMakeConstantString(V);
#else

/* Hack: we take a copy of this from CFInternal.h. */

struct CF_CONST_STRING {
    CFRuntimeBase _base;
    uint8_t *     _ptr;
    uint32_t      _length;
};

extern int __CFConstantStringClassReference[];

#if defined(__WIN32__)
#define ___WindowsConstantStringClassReference (uintptr_t)&__CFConstantStringClassReference
#else
#define ___WindowsConstantStringClassReference NULL
#endif

// The original code used the preprocessor mnemonics __ppc__ and
// __i386__; however, these are/were just proxies for big and little
// endian byte ordering, respectively. Just use those for better
// portability.

#if defined(__BYTE_ORDER__)
# if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
#  define __CFNETWORK_BYTEORDER_BIG    0
#  define __CFNETWORK_BYTEORDER_LITTLE 1
# elif __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
#  define __CFNETWORK_BYTEORDER_BIG    1
#  define __CFNETWORK_BYTEORDER_LITTLE 0
# endif /* __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__ */
#elif defined(__LITTLE_ENDIAN__) && (__LITTLE_ENDIAN__ == 1)
# define __CFNETWORK_BYTEORDER_BIG    0
# define __CFNETWORK_BYTEORDER_LITTLE 1
#elif defined(__BIG_ENDIAN__) && (__BIG_ENDIAN__ == 1)
# define __CFNETWORK_BYTEORDER_BIG    1
# define __CFNETWORK_BYTEORDER_LITTLE 0
#else
#error "Endianness undefined!"
#endif /* defined(__BYTE_ORDER__) */

#if __CFNETWORK_BYTEORDER_BIG
# define CONST_STRING_DECL(S, V)         \
    struct CF_CONST_STRING __ ## S ## __ = {{(uintptr_t)&__CFConstantStringClassReference, {0x00, 0x00, 0x07, 0xc8}}, (uint8_t *)V, sizeof(V) - 1}; \
const CFStringRef S = (CFStringRef) & __ ## S ## __;
#elif !defined(__WIN32__) || (defined(__WIN32__) && defined(__GNUC__))
# define CONST_STRING_DECL(S, V)         \
    struct CF_CONST_STRING __ ## S ## __ = {{(uintptr_t)&__CFConstantStringClassReference, {0xc8, 0x07, 0x00, 0x00}}, (uint8_t *)V, sizeof(V) - 1}; \
const CFStringRef S = (CFStringRef) & __ ## S ## __;
#elif defined(__WIN32__)
# define CONST_STRING_DECL(S, V)         \
    struct CF_CONST_STRING __ ## S ## __ = {{(uintptr_t)&__CFConstantStringClassReference, {0xc8, 0x07, 0x00, 0x00}},(uint8_t *) V, sizeof(V) - 1}; \
CF_EXPORT const CFStringRef S = (CFStringRef) & __ ## S ## __;

# define CONST_STRING_DECL_EXPORT(S, V)          \
    struct CF_CONST_STRING __ ## S ## __ = {{___WindowsConstantStringClassReference, {0xc8, 0x07, 0x00, 0x00}}, (uint8_t *)V, sizeof(V) - 1}; \
CF_EXPORT const CFStringRef S = (CFStringRef) & __ ## S ## __;

#else
# define CONST_STRING_DECL(S, V)         \
    struct CF_CONST_STRING __ ## S ## __ = {{(uintptr_t)NULL, {0xc8, 0x07, 0x00, 0x00}},(uint8_t *)V, sizeof(V) - 1}; \
const CFStringRef S = (CFStringRef) & __ ## S ## __;

# define CONST_STRING_DECL_EXPORT(S, V)          \
    struct CF_CONST_STRING __ ## S ## __ = {{(uintptr_t)NULL, {0xc8, 0x07, 0x00, 0x00}}, (uint8_t *)V, sizeof(V) - 1}; \
CF_EXPORT const CFStringRef S = (CFStringRef) & __ ## S ## __;

#endif // __CFNETWORK_BYTEORDER_BIG

#undef ___WindowsConstantStringClassReference

#endif /* __CONSTANT_CFSTRINGS__ */

/*!
	@function __CFNetworkLoadFramework
	@discussion Loads the framework image pointed to by framework_path.
		This function will use the proper dyld suffix and search methods
		for the given situation.
	@param framework_path The path to the framework to be loaded.
	@result Returns a pointer to the image on success.  It returns NULL
		on failure.
*/
extern void* __CFNetworkLoadFramework(const char* const framework_path);


/*!
    @function _CFNetworkCFStringCreateWithCFDataAddress
    @discussion Creates a dotted IP string for the address given.
    @param alloc Allocator reference to use for the string allocation
    @param addr CFDataRef containing the struct sockaddr with the address.
    @result A CFStringRef containing the dotted IP string for the address.
        Returns NULL if the address could not be converted to dotted IP.
        Currently AF_INET and AF_INET6 are supported.
 */
extern CFStringRef _CFNetworkCFStringCreateWithCFDataAddress(CFAllocatorRef alloc, CFDataRef addr);


/*!
    @function _CFStringGetOrCreateCString
    @discussion Given a CFString, this function attempts to get the bytes of
		the string and create a C-style (null terminated) string from them.
		If the given buffer is too small, one of adequate length will be
		allocated with the given allocator.  It is the client's responsibility
		to deallocate the buffer if the returned buffer is not the same
		buffer which was passed.
    @param allocator Allocator to be used for allocation should the given
		buffer not be big enough.
	@param string CFString from which the bytes should be retrieved.  Must be
		non-NULL.
	@param buffer Buffer into which the bytes should be placed.  If this buffer
		is not big enough, one will be allocated.  Use NULL to always allocate.
	@param bufferLength Pointer to the size of the incoming buffer.  Upon a
		successful return, this will contain the number of bytes in the buffer,
		not counting the null termination.  Must be non-NULL if buffer is non-NULL.
	@param encoding String encoding to be used for decoding the bytes.
    @result Returns the buffer holding the bytes.  If the passed in buffer pointer
		is not the same as the result buffer pointer, the client must deallocate
		the buffer.
*/
extern UInt8* _CFStringGetOrCreateCString(CFAllocatorRef allocator, CFStringRef string, UInt8* buffer, CFIndex* bufferLength, CFStringEncoding encoding);


/*!
    @function _DNSServiceErrorToCFNetServiceError
    @discussion Given a DNSService error, this returns the appropriate CFNetService error.
    @param dnsError DNSServiceErrorType error.
    @result A SInt32 containing the equivalent CFNetService error.
*/
extern SInt32 _DNSServiceErrorToCFNetServiceError(DNSServiceErrorType dnsError);



#if defined(__cplusplus)
}
#endif


#endif	/* __CFNETWORKINTERNAL__ */
