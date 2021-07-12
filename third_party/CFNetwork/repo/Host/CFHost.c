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
 *  CFHost.cpp
 *  CFNetwork
 *
 *  Created by Jeremy Wyld on Thu Nov 28 2002.
 *  Copyright (c) 2002 Apple Computer, Inc. All rights reserved.
 *
 */

#if 0
#pragma mark Description
#endif

/*
	CFHost is built as a CFRuntimeBase object.  The actual registration of the class type
	takes place when the first call for the type id is made (CFHostGetTypeID).  The object
	instantiation functions use this call for creation, therefore any of the creators will
	cause registration of the class.

	CFHost's underlying lookups can be any asynchronous CFType (i.e. CFMachPort, CFSocket,
	CFFileDescriptor, SCNetworkReachability, etc.).  The lookup should be created and
	scheduled on the run loops and modes saved in the "schedules" array.  The array is
	maintained in order to allow scheduling separate from the lookup.  With this, lookup
	can be created after schedules have been placed on the object.  The lookup can then be
	scheduled the same as the object.  The schedules array contains a list of pairs of run
	loops and modes (e.g. [<rl1>, <mode1>, <rl2>, <mode2>, ...]).  There can be zero or more
	items in the array, but the count should always be divisible by 2.

	A cancel is just another type of lookup.  A custom CFRunLoopSource is created which
	is simply signalled instantly.  This will cause synchronous lookups on other run loops
	(threads) to cancel out immediately.

	All resolved information is stored in a dictionary on the host object.  The key is the
	CFHostInfoType with the value being specific to the type.  Value types should be
	documented with the CFHostInfoType declarations.  In the case where a lookup produces
	no data, kCFNull should be used for the value of the type.  This distinguishes the
	lookup as being performed and returning no data, which is different from not ever
	performing the lookup.

	Duplicate suppression is performed for hostname lookups.  The first hostname lookup
	that is performed creates a "master" lookup.  The master is just another CFHostRef
	whose lookup is started as a special info type.  This signals to it that it is the
	master and that there are clients of it.  The master is then placed in a global dictionary
	of outstanding lookups.  When a second is started, it is checked for existance in the
	global list.  If/When found, the second request is added to the list of clients.  The
	master lookup is scheduled on all loops and modes as the list of clients.  When the
	master lookup completes, all clients in the list are informed.  If all clients cancel,
	the master lookup will be canceled and removed from the master lookups list.
*/


#if 0
#pragma mark -
#pragma mark Includes
#endif

#if HAVE_CONFIG_H
#include "opencfnetwork-config.h"
#endif

#include <AssertMacros.h>
#include <CFNetwork/CFNetwork.h>
#include <CFNetwork/CFNetworkPriv.h>
#include "CFNetworkInternal.h"							/* for __CFSpinLock and __CFSpinUnlock */
#include "CFNetworkSchedule.h"

#include <math.h>										/* for fabs */
#include <sys/socket.h>

#if defined(__MACH__)
# include <netdb_async.h>
# include <SystemConfiguration/SystemConfiguration.h>	/* for SCNetworkReachability and flags */
#elif defined(__linux__)
#ifndef _GNU_SOURCE
# define _GNU_SOURCE 1                                  /* for getaddrinfo_a */
#endif /* _GNU_SOURCE */
# include <arpa/inet.h>
# include <netdb.h>
# include <signal.h>
# include <sys/signalfd.h>
# include <sys/syscall.h>
# include <poll.h>
# include <unistd.h>
#endif /* defined(__MACH__) */

#if HAVE_ARES_H
#include <ares.h>
#endif

#if 0
#pragma mark -
#pragma mark Constants
#endif

#if !defined(LOG_CFHOST)
#define LOG_CFHOST 1
#endif

#if LOG_CFHOST
#define __CFHostMaybeLog(format, ...)  do { fprintf(stderr, format, ##__VA_ARGS__); fflush(stderr); } while (0)
#else
#define __CFHostMaybeLog(format, ...)
#endif

#define __CFHostMaybeTrace(dir, name)                           \
	__CFHostMaybeLog(dir " %s\n", name)
#define __CFHostMaybeTraceWithFormat(dir, name, format, ...)	\
	__CFHostMaybeLog(dir " %s" format, name, ##__VA_ARGS__)
#define __CFHostTraceEnterWithFormat(format, ...)               \
	__CFHostMaybeTraceWithFormat("-->", __func__, " " format, ##__VA_ARGS__)
#define __CFHostTraceExitWithFormat(format, ...)                \
	__CFHostMaybeTraceWithFormat("<--", __func__, " " format, ##__VA_ARGS__)
#define __CFHostTraceEnter()                                    \
	__CFHostTraceEnterWithFormat("\n")
#define __CFHostTraceExit()                                     \
	__CFHostTraceExitWithFormat("\n")

/* extern */ const SInt32 kCFStreamErrorDomainNetDB = 12;
/* extern */ const SInt32 kCFStreamErrorDomainSystemConfiguration = 13;

#define _kCFNullHostInfoType				((CFHostInfoType)0xFFFFFFFF)

#define _kCFHostIPv4Addresses				((CFHostInfoType)0x0000FFFE)
#define _kCFHostIPv6Addresses				((CFHostInfoType)0x0000FFFD)
#define _kCFHostMasterAddressLookup			((CFHostInfoType)0x0000FFFC)
#define _kCFHostByPassMasterAddressLookup	((CFHostInfoType)0x0000FFFB)

#define _kCFHostCacheMaxEntries				25
#define _kCFHostCacheTimeout				((CFTimeInterval)1.0)


#if 0
#pragma mark -
#pragma mark Constant Strings
#endif

#ifdef __CONSTANT_CFSTRINGS__
#define _kCFHostBlockingMode	CFSTR("_kCFHostBlockingMode")
#define _kCFHostDescribeFormat	CFSTR("<CFHost 0x%x>{info=%@}")
#else
CONST_STRING_DECL_LOCAL(_kCFHostBlockingMode, "_kCFHostBlockingMode")
CONST_STRING_DECL_LOCAL(_kCFHostDescribeFormat, "<CFHost 0x%x>{info=%@}")
#endif	/* __CONSTANT_CFSTRINGS__ */

#if defined(__linux__)
#define __kCFHostLinuxSignalFdSignal ((int)SIGRTMIN + 11)
#endif

#if 0
#pragma mark -
#pragma mark CFHost struct
#endif

typedef struct {

	CFRuntimeBase 			_base;

	CFSpinLock_t			_lock;

	CFStreamError			_error;

	CFMutableDictionaryRef	_info;

	//CFMutableDictionaryRef  _lookups;		// key = CFHostInfoType and value = CFTypeRef
	CFTypeRef				_lookup;
	CFHostInfoType			_type;

	CFMutableArrayRef		_schedules;		// List of loops and modes
	CFHostClientCallBack	_callback;
	CFHostClientContext		_client;
} _CFHost;

#if defined(__linux__)
typedef struct {
	struct gaicb    _request_gaicb;
	struct addrinfo _request_hints;
	struct gaicb *  _request_list[1];
} _CFHostGAIARequest;

typedef struct {
    ares_channel        _request_channel;
    size_t              _request_pending;
    const char *        _request_name;
    CFFileDescriptorRef _request_lookup;
    uint16_t            _request_events;
    CFStreamError *     _request_error;
    int                 _request_status;
    struct addrinfo *   _request_addrinfo;
    _CFHost *           _request_host;
} _CFHostAresRequest;
#endif /* defined(__linux__) */

#if 0
#pragma mark -
#pragma mark Static Function Declarations
#endif

#if defined(__linux__)
static void _CFHostInitializeAres(void);
#endif
static void _CFHostRegisterClass(void);
static _CFHost* _HostCreate(CFAllocatorRef allocator);

static void _HostDestroy(_CFHost* host);
static CFStringRef _HostDescribe(_CFHost* host);

static void _HostCancel(_CFHost* host);

static Boolean _HostBlockUntilComplete(_CFHost* host);

static Boolean _CreateLookup_NoLock(_CFHost* host, CFHostInfoType info, Boolean* _Radar4012176);

static UInt8* _CreateLookup_Common(CFTypeRef thing, CFStreamError* error);
static void _HandleGetAddrInfoStatus(int eai_status, CFStreamError* error, Boolean intuitStatus);
static void _InitGetAddrInfoHints(CFHostInfoType info, struct addrinfo *hints);
static CFTypeRef _CreateMasterAddressLookup(CFStringRef name, CFHostInfoType info, CFTypeRef context, CFStreamError* error);
#if defined(__MACH__)
static CFMachPortRef _CreatePrimaryAddressLookup_Mach(CFStringRef name, CFHostInfoType info, CFTypeRef context, CFStreamError* error);
#endif
#if defined(__linux__)
static int _SignalFdModifySignalWithError(int how, int signal, sigset_t *set, CFStreamError *error);
static int _SignalFdSetSignalWithError(int signal, sigset_t *set, CFStreamError *error);
static int _SignalFdClearSignalWithError(int signal, sigset_t *set, CFStreamError *error);
static int _SignalFdClearGetAddrInfoSignalWithHost(_CFHost *host);
static int _CreateSignalFd(int signal, CFStreamError *error);
static int _CreateAddressLookupRequest(const char *name, CFHostInfoType info, int signal, CFStreamError *error);
static struct gaicb * _SignalFdGetAddrInfoResult(CFFileDescriptorRef fdref);
static void _PrimaryAddressLookupCallBack_Linux(CFFileDescriptorRef fdref, CFOptionFlags callBackTypes, void *info);
static CFFileDescriptorRef _CreateAddressLookupSource_GetAddrInfo_A(int signal, CFTypeRef context, CFStreamError *error);
static CFFileDescriptorRef _CreatePrimaryAddressLookup_Linux_Ares(CFStringRef name, CFHostInfoType info, CFTypeRef context, CFStreamError* error);
#if HAVE_GETADDRINFO_A && 0
static CFFileDescriptorRef _CreatePrimaryAddressLookup_Linux_GetAddrInfo_A(CFStringRef name, CFHostInfoType info, CFTypeRef context, CFStreamError* error);
#endif /* HAVE_GETADDRINFO_A && 0 */
#endif /* defined(__linux__) */
static CFTypeRef _CreateAddressLookup(CFStringRef name, CFHostInfoType info, void* context, CFStreamError* error);
static CFTypeRef _CreateNameLookup(CFDataRef address, void* context, CFStreamError* error);
#if defined(__MACH__)
static CFMachPortRef _CreateNameLookup_Mach(CFDataRef address, void* context, CFStreamError* error);
static SCNetworkReachabilityRef _CreateReachabilityLookup(CFTypeRef thing, void* context, CFStreamError* error);
static CFMachPortRef _CreateDNSLookup(CFTypeRef thing, CFHostInfoType type, void* context, CFStreamError* error);
#endif /* #if defined(__MACH__) */
#if defined(__linux__)
static CFFileDescriptorRef _CreateDNSLookup(CFTypeRef thing, CFHostInfoType type, void* context, CFStreamError* error);
static CFFileDescriptorRef _CreateNameLookup_Linux_Ares(CFDataRef address, void* context, CFStreamError* error);
#endif /* defined(__linux__) */
typedef void (*FreeAddrInfoCallBack)(struct addrinfo *res);
static void _GetAddrInfoCallBackWithFree(int eai_status, const struct addrinfo *res, void *ctxt, FreeAddrInfoCallBack freeaddrinfo_cb);
static void _GetAddrInfoCallBack(int eai_status, const struct addrinfo* res, void* ctxt);
#if defined(__MACH__)
static void _GetAddrInfoMachPortCallBack(CFMachPortRef port, void* msg, CFIndex size, void* info);
#endif
#if defined(__MACH__)
typedef void (*FreeNameInfoCallBack)(char *hostname, char *serv);
static void _GetNameInfoCallBackWithFree(int eai_status, char *hostname, char *serv, void* ctxt, FreeNameInfoCallBack freenameinfo_cb);
static void _GetNameInfoCallBack(int eai_status, char *hostname, char *serv, void* ctxt);
static void _GetNameInfoMachPortCallBack(CFMachPortRef port, void* msg, CFIndex size, void* info);
#endif /* defined(__MACH__) */
#if defined(__MACH__)
static void _NetworkReachabilityCallBack(SCNetworkReachabilityRef target, SCNetworkConnectionFlags flags, void* ctxt);
static void _NetworkReachabilityByIPCallBack(_CFHost* host);
#endif /* defined(__MACH__) */

#if defined(__MACH__)
static void _DNSCallBack(int32_t status, char *buf, uint32_t len, struct sockaddr *from, int fromlen, void *context);
static void _DNSMachPortCallBack(CFMachPortRef port, void* msg, CFIndex size, void* info);
#endif
static void _MasterCallBack(CFHostRef theHost, CFHostInfoType typeInfo, const CFStreamError *error, CFStringRef name);
static void _AddressLookupSchedule_NoLock(_CFHost* host, CFRunLoopRef rl, CFStringRef mode);
static void _AddressLookupPerform(_CFHost* host);

static void _ExpireCacheEntries(void);

static CFArrayRef _CFArrayCreateDeepCopy(CFAllocatorRef alloc, CFArrayRef array);
#if defined(__MACH__)
static Boolean _IsDottedIp(CFStringRef name);
#endif


#if 0
#pragma mark -
#pragma mark Globals
#endif

static _CFOnceLock _kCFHostRegisterClass = _CFOnceInitializer;
#if defined(__linux__)
static _CFOnceLock _kCFHostInitializeAres = _CFOnceInitializer;
#endif
static CFTypeID _kCFHostTypeID = _kCFRuntimeNotATypeID;

static _CFMutex* _HostLock;						/* Lock used for cache and master list */
static CFMutableDictionaryRef _HostLookups;		/* Active hostname lookups; for duplicate supression */
static CFMutableDictionaryRef _HostCache;		/* Cached hostname lookups (successes only) */


#if 0
#pragma mark -
#pragma mark Static Function Definitions
#endif

/* static */ void
_CFHostInitializeAres(void) {
    __CFHostTraceEnter();

    int status = ares_library_init(ARES_LIB_INIT_ALL);
    __Verify_Action(status == ARES_SUCCESS, abort());

    __CFHostTraceExit();
}

/* static */ void
_CFHostRegisterClass(void) {

	static const CFRuntimeClass _kCFHostClass = {
		0,												// version
		"CFHost",										// class name
		NULL,      										// init
		NULL,      										// copy
		(void(*)(CFTypeRef))_HostDestroy,				// dealloc
		NULL,      										// equal
		NULL,      										// hash
		NULL,      										// copyFormattingDesc
		(CFStringRef(*)(CFTypeRef cf))_HostDescribe		// copyDebugDesc
	};


    _kCFHostTypeID = _CFRuntimeRegisterClass(&_kCFHostClass);

	/* Set up the "master" for simultaneous, duplicate lookups. */
	_HostLock = (_CFMutex*)CFAllocatorAllocate(kCFAllocatorDefault, sizeof(_HostLock[0]), 0);
	if (_HostLock) _CFMutexInit(_HostLock, FALSE);
	_HostLookups = CFDictionaryCreateMutable(kCFAllocatorDefault,
													0,
													&kCFTypeDictionaryKeyCallBacks,
													&kCFTypeDictionaryValueCallBacks);

	_HostCache = CFDictionaryCreateMutable(kCFAllocatorDefault,
										   0,
										   &kCFTypeDictionaryKeyCallBacks,
										   &kCFTypeDictionaryValueCallBacks);
}


/* static */ _CFHost*
_HostCreate(CFAllocatorRef allocator) {

	CFDictionaryKeyCallBacks keys = {0, NULL, NULL, NULL, NULL, NULL};

	_CFHost* result = (_CFHost*)_CFRuntimeCreateInstance(allocator,
														   CFHostGetTypeID(),
														   sizeof(result[0]) - sizeof(CFRuntimeBase),
														   NULL);

	if (result) {

		// Save a copy of the base so it's easier to zero the struct
		CFRuntimeBase copy = result->_base;

		// Clear everything.
		memset(result, 0, sizeof(result[0]));

		// Put back the base
		memmove(&(result->_base), &copy, sizeof(result->_base));

        CF_SPINLOCK_INIT_FOR_STRUCTS(result->_lock);

		// No lookup by default
		result->_type = _kCFNullHostInfoType;

		// Create the dictionary of lookup information
		result->_info = CFDictionaryCreateMutable(allocator, 0, &keys, &kCFTypeDictionaryValueCallBacks);

		// Create the list of loops and modes
		result->_schedules = CFArrayCreateMutable(allocator, 0, &kCFTypeArrayCallBacks);

		// If any failed, need to release and return null
		if (!result->_info || !result->_schedules) {
			CFRelease((CFTypeRef)result);
			result = NULL;
		}
	}

	return result;
}


/* static */ void
_HostDestroy(_CFHost* host) {

	// Prevent anything else from taking hold
	__CFSpinLock(&(host->_lock));

	// Release the user's context info if there is some and a release method
	if (host->_client.info && host->_client.release)
		host->_client.release(host->_client.info);

	// If there is a lookup, release it.
	if (host->_lookup) {

		// Remove the lookup from run loops and modes
		_CFTypeUnscheduleFromMultipleRunLoops(host->_lookup, host->_schedules);

		// Go ahead and invalidate the lookup
		_CFTypeInvalidate(host->_lookup);

		// Release the lookup now.
		CFRelease(host->_lookup);
	}

	// Release any gathered information
	if (host->_info)
		CFRelease(host->_info);

	// Release the list of loops and modes
	if (host->_schedules)
		CFRelease(host->_schedules);
}


/* static */ CFStringRef
_HostDescribe(_CFHost* host) {

	CFStringRef result;

	__CFSpinLock(&host->_lock);

	result = CFStringCreateWithFormat(CFGetAllocator((CFHostRef)host),
									  NULL,
									  _kCFHostDescribeFormat,
									  host,
									  host->_info);

	__CFSpinUnlock(&host->_lock);

	return result;
}


/* static */ void
_HostCancel(_CFHost* host) {

	CFHostClientCallBack cb = NULL;
	CFStreamError error;
	void* info = NULL;
	CFHostInfoType type = _kCFNullHostInfoType;

	// Retain here to guarantee safety really after the lookups release,
	// but definitely before the callback.
	CFRetain((CFHostRef)host);

	// Lock the host
	__CFSpinLock(&host->_lock);

	// If the lookup canceled, don't need to do any of this.
	if (host->_lookup) {

		// Save the callback if there is one at this time.
		cb = host->_callback;

		// Save the type of lookup for the callback.
		type = host->_type;

		// Save the error and client information for the callback
		memmove(&error, &(host->_error), sizeof(error));
		info = host->_client.info;

		// Remove the lookup from run loops and modes
		_CFTypeUnscheduleFromMultipleRunLoops(host->_lookup, host->_schedules);

		// Invalidate the run loop source that got here
		CFRunLoopSourceInvalidate((CFRunLoopSourceRef)(host->_lookup));

		// Release the lookup now.
		CFRelease(host->_lookup);
		host->_lookup = NULL;
		host->_type = _kCFNullHostInfoType;
	}

	// Unlock the host so the callback can be made safely.
	__CFSpinUnlock(&host->_lock);

	// If there is a callback, inform the client of the finish.
	if (cb)
		cb((CFHostRef)host, type, &error, info);

	// Go ahead and release now that the callback is done.
	CFRelease((CFHostRef)host);
}


/* static */ Boolean
_HostBlockUntilComplete(_CFHost* host) {

	// Assume success by default
	Boolean result = TRUE;
	CFRunLoopRef rl = CFRunLoopGetCurrent();

	// Schedule in the blocking mode.
	CFHostScheduleWithRunLoop((CFHostRef)host, rl, _kCFHostBlockingMode);

	// Lock in order to check for lookup
	__CFSpinLock(&(host->_lock));

	// Check that lookup exists.
	while (host->_lookup) {

		// Unlock again so the host can continue to be processed.
		__CFSpinUnlock(&(host->_lock));

		// Run the loop in a private mode with it returning whenever a source
		// has been handled.
		CFRunLoopRunInMode(_kCFHostBlockingMode, DBL_MAX, TRUE);

		// Lock again in preparation for lookup check
		__CFSpinLock(&(host->_lock));
	}

	// Fail if there was an error.
	if (host->_error.error)
		result = FALSE;

	// Unlock the host again.
	__CFSpinUnlock(&(host->_lock));

	// Unschedule from the blocking mode
	CFHostUnscheduleFromRunLoop((CFHostRef)host, rl, _kCFHostBlockingMode);

	return result;
}


/* static */ Boolean
_CreateLookup_NoLock(_CFHost* host, CFHostInfoType info, Boolean* _Radar4012176) {

	Boolean result = FALSE;

	// Get the existing names and addresses
	CFArrayRef names = (CFArrayRef)CFDictionaryGetValue(host->_info, (const void*)kCFHostNames);
	CFArrayRef addrs = (CFArrayRef)CFDictionaryGetValue(host->_info, (const void*)kCFHostAddresses);

	// Grab the first of each if they exist in order to perform any of the lookups
	CFStringRef name = names && ((CFTypeRef)names != kCFNull) && CFArrayGetCount(names) ? (CFStringRef)CFArrayGetValueAtIndex(names, 0) : NULL;
	CFDataRef addr = addrs && ((CFTypeRef)addrs != kCFNull) && CFArrayGetCount(addrs) ? (CFDataRef)CFArrayGetValueAtIndex(addrs, 0) : NULL;

	*_Radar4012176 = FALSE;

	// Only allow one lookup at a time
	if (host->_lookup)
		return result;

	switch (info) {

		// If a address lookup and there is a name, create and start the lookup.
		case kCFHostAddresses:

			if (name) {

				CFArrayRef cached = NULL;

				/* Expire any entries from the cache */
				_ExpireCacheEntries();

				/* Lock the cache */
				_CFMutexLock(_HostLock);

				/* Go for a cache entry. */
				if (_HostCache)
					cached = (CFArrayRef)CFDictionaryGetValue(_HostCache, name);

				if (cached)
					CFRetain(cached);

				_CFMutexUnlock(_HostLock);

				/* Create a lookup if no cache entry. */
				if (!cached)
					host->_lookup = _CreateAddressLookup(name, info, host, &(host->_error));

				else {

					CFAllocatorRef alloc = CFGetAllocator(name);

					/* Make a copy of the addresses in the cached entry. */
					CFArrayRef cp = _CFArrayCreateDeepCopy(alloc,
														   CFHostGetInfo((CFHostRef)CFArrayGetValueAtIndex(cached, 0), _kCFHostMasterAddressLookup, NULL));

					CFRunLoopSourceContext ctxt = {
						0,
						host,
						CFRetain,
						CFRelease,
						CFCopyDescription,
						NULL,
						NULL,
						NULL,
						NULL,
						(void (*)(void*))_AddressLookupPerform
					};

					/* Create the lookup source.  This source will be signalled immediately. */
					host->_lookup = CFRunLoopSourceCreate(alloc, 0, &ctxt);

					/* Upon success, add the data and signal the source. */
					if (host->_lookup && cp) {

						CFDictionaryAddValue(host->_info, (const void*)info, cp);

						CFRunLoopSourceSignal((CFRunLoopSourceRef)host->_lookup);
						*_Radar4012176 = TRUE;
					}

					else {

						host->_error.error = ENOMEM;
						host->_error.domain = kCFStreamErrorDomainPOSIX;
					}

					if (cp)
						CFRelease(cp);
					else if (host->_lookup) {
						CFRelease(host->_lookup);
						host->_lookup = NULL;
					}

					CFRelease(cached);
				}
			}

			break;

		// If a name lookup and there is an address, create and start the lookup.
		case kCFHostNames:
			if (addr) host->_lookup = _CreateNameLookup(addr, host, &(host->_error));
			break;

		// Create a reachability check using the address or name (prefers address).
		case kCFHostReachability:
#if defined(__MACH__)
			{
				CFTypeRef use = (addr != NULL) ? (CFTypeRef)addr : (CFTypeRef)name;

				/* Create the reachability lookup. */
				host->_lookup = _CreateReachabilityLookup(use, host, &(host->_error));

				/*
				** <rdar://problem/3612320> Check reachability by IP address doesn't work?
				**
				** Reachability when created with an IP has not future trigger point in
				** order to get the flags callback.  The behavior of the reachabilty object
				** can not change, so as a workaround, CFHost does an immediate flags
				** request and then creates the CFRunLoopSourceRef for the asynchronous
				** trigger.
				*/
				if (host->_lookup && ((use == addr) || _IsDottedIp(use))) {

					CFRunLoopSourceContext ctxt = {
						0,														// version
						host,													// info
						NULL,													// retain
						NULL,													// release
						NULL,													// copyDescription
						NULL,													// equal
						NULL,													// hash
						NULL,													// schedule
						NULL,													// cancel
						(void(*)(void*))(&_NetworkReachabilityByIPCallBack)		// perform
					};

					SCNetworkConnectionFlags flags = 0;
					CFAllocatorRef alloc = CFGetAllocator(host);

					/* Get the flags right away for dotted IP. */
					SCNetworkReachabilityGetFlags((SCNetworkReachabilityRef)(host->_lookup), &flags);

					/* Remove the callback that was set already. */
					SCNetworkReachabilitySetCallback((SCNetworkReachabilityRef)(host->_lookup), NULL, NULL);

					/* Toss out the lookup because a new one will be set up. */
					CFRelease(host->_lookup);
					host->_lookup = NULL;

					/* Create the asynchronous source */
					host->_lookup = CFRunLoopSourceCreate(alloc, 0, &ctxt);

					if (!host->_lookup) {
						host->_error.error = ENOMEM;
						host->_error.domain = kCFStreamErrorDomainPOSIX;
					}

					else {

						// Create the data for hanging off the host info dictionary
						CFDataRef reachability = CFDataCreate(alloc, (const UInt8*)&flags, sizeof(flags));

						// Make sure to toss the cached info now.
						CFDictionaryRemoveValue(host->_info, (const void*)kCFHostReachability);

						// If didn't create the data, fail with out of memory.
						if (!reachability) {

							/* Release and toss the lookup. */
							CFRelease(host->_lookup);
							host->_lookup = NULL;

							host->_error.error = ENOMEM;
							host->_error.domain = kCFStreamErrorDomainPOSIX;
						}

						else {
							// Save the reachability information
							CFDictionaryAddValue(host->_info, (const void*)kCFHostReachability, reachability);
							CFRelease(reachability);

							/* Signal the reachability for immediate attention. */
							CFRunLoopSourceSignal((CFRunLoopSourceRef)(host->_lookup));
						}
					}
				}
			}
#else
			{
				host->_error.error = EOPNOTSUPP;
				host->_error.domain = kCFStreamErrorDomainPOSIX;
			}
#endif /* #if defined(__MACH__) */
			break;

		case _kCFHostMasterAddressLookup:
			host->_lookup = _CreateMasterAddressLookup(name, info, host, &(host->_error));
			break;

		// Create a general DNS check using the name or address (prefers name).
		default:

			if (name) {
				if ((info == _kCFHostIPv4Addresses) || (info == _kCFHostIPv6Addresses) || (info == _kCFHostByPassMasterAddressLookup))
					host->_lookup = _CreateMasterAddressLookup(name, info, host, &(host->_error));
				else
					host->_lookup = _CreateDNSLookup(name, info, host, &(host->_error));
			}
			else if (addr) {

				name = _CFNetworkCFStringCreateWithCFDataAddress(CFGetAllocator(addr), addr);

				if (name) {

					host->_lookup = _CreateDNSLookup(name, info, host, &(host->_error));

					CFRelease(name);
				}

				else {

					host->_error.error = ENOMEM;
					host->_error.domain = kCFStreamErrorDomainPOSIX;
				}
			}
			break;
	}

	if (host->_lookup) {
		host->_type = info;
		result = TRUE;
	}

	return result;
}

/* static */ UInt8*
_CreateLookup_Common(CFTypeRef thing, CFStreamError* error) {
	const CFAllocatorRef allocator = CFGetAllocator(thing);
	const CFIndex        length    = CFStringGetLength((CFStringRef)thing);
	CFIndex              converted;
	UInt8*               result = NULL;

	// Get the bytes of the conversion
	result =  _CFStringGetOrCreateCString(allocator, (CFStringRef)thing, NULL, &converted, kCFStringEncodingUTF8);

	// If the buffer failed to create, set the error and bail.
	if (!result) {

		// Set the no memory error.
		error->error = ENOMEM;
		error->domain = kCFStreamErrorDomainPOSIX;

		// Bail
		return result;
	}

	// See if all the bytes got converted.
	if (converted != length) {

		// If not, this amounts to a host not found error.  This is to primarily
		// deal with embedded bad characters in host names coming from URL's
		// (e.g. www.apple.com%00www.notapple.com).
		error->error = HOST_NOT_FOUND;
		error->domain = (CFStreamErrorDomain)kCFStreamErrorDomainNetDB;

		CFAllocatorDeallocate(allocator, result);
		result = NULL;
	}

	return result;
}

/* static */ void
_HandleGetAddrInfoStatus(int eai_status, CFStreamError* error, Boolean intuitStatus)
{
	if (eai_status != 0) {
		// If it's a system error, get the real error otherwise it's a
		// NetDB error.
		if (eai_status == EAI_SYSTEM) {
			error->error  = errno;
			error->domain = kCFStreamErrorDomainPOSIX;
		} else {
			error->error  = eai_status;
			error->domain = (CFStreamErrorDomain)kCFStreamErrorDomainNetDB;
		}
	}

	else if (intuitStatus) {
		// No error set, see if errno has anything.  If so, mark the error as
		// a POSIX error.
	    if (errno != 0) {
			error->error  = errno;
			error->domain = (CFStreamErrorDomain)kCFStreamErrorDomainPOSIX;

			// Don't know what happened, so mark it as an internal netdb error.
		} else {
			error->error  = NETDB_INTERNAL;
			error->domain = (CFStreamErrorDomain)kCFStreamErrorDomainNetDB;
		}
	}
}

/* static */ void
_InitGetAddrInfoHints(CFHostInfoType info, struct addrinfo *hints)
{
#ifdef AI_PARALLEL
	const int ai_flags = AI_ADDRCONFIG | AI_PARALLEL;
#else
	const int ai_flags = AI_ADDRCONFIG;
#endif /* AI_PARALLEL */

	memset(hints, 0, sizeof(struct addrinfo));

	if (info == _kCFHostIPv4Addresses) {
		hints->ai_family = AF_INET;
	} else if (info == _kCFHostIPv6Addresses) {
		hints->ai_family = AF_INET6;
	} else {
		hints->ai_family = AF_UNSPEC;
	}

	hints->ai_socktype   = SOCK_STREAM;
	hints->ai_flags      = ai_flags;
}

/**
 *  @brief
 *    Initiate and create the first domain name resolution lookup for
 *    a given host name.
 *
 *  Per the discussion at file scope, the first lookup that is
 *  performed creates a "master", or primary, lookup. The primary
 *  lookup is just another CFHostRef whose lookup is started as a
 *  special info type. This signals to it that it is the primary and
 *  that there are clients of it.  The primary is then placed in a
 *  global dictionary of outstanding lookups.  When a second is
 *  started, it is checked for existance in the global list.  If/when
 *  found, the second request is added to the list of clients.  The
 *  primary lookup is scheduled on all loops and modes as the list of
 *  clients.  When the primary lookup completes, all clients in the
 *  list are informed.  If all clients cancel, the primary lookup will
 *  be canceled and removed from the primary lookups list.
 *
 *  @param[in]      name     A reference to a string containing a host
 *                           name or IP address that is to be lookedup
 *                           and/or resolved.
 *  @param[in]      info     A value of type CFHostInfoType
 *                           specifying the type of information that
 *                           is to be retrieved. See #CFHostInfoType
 *                           for possible values.
 *  @param[in]      context  A pointer to user-supplied context to be
 *                           returned to the caller on completion of the
 *                           lookup.
 *  @param[in,out]  error    A pointer to a #CFStreamError structure,
 *                           that if an error occurs, is set to the
 *                           error and the error's domain. In
 *                           synchronous mode, the error indicates why
 *                           resolution failed, and in asynchronous
 *                           mode, the error indicates why resolution
 *                           failed to start.
 *
 *  @returns
 *    The asynchronous, schedulable CFType for the lookup operation on
 *    success; otherwise, NULL.
 *
 */
/* static */ CFTypeRef
_CreateMasterAddressLookup(CFStringRef name, CFHostInfoType info, CFTypeRef context, CFStreamError* error) {
	CFTypeRef result = NULL;

#if defined(__MACH__)
	result = _CreatePrimaryAddressLookup_Mach(name, info, context, error);
#elif defined(__linux__)
# if HAVE_GETADDRINFO_A && 0
	result = _CreatePrimaryAddressLookup_Linux_GetAddrInfo_A(name, info, context, error);
# elif HAVE_ARES_INIT && 1
    result = _CreatePrimaryAddressLookup_Linux_Ares(name, info, context, error);
# else
#  error "No Linux primary getaddrinfo/gethostbyname DNS lookup implementation!"
# endif /* HAVE_GETADDRINFO_A */
#else
#warning "Platform portability issue!"
#endif

	return result;
}

#if defined(__MACH__)
/* static */ CFMachPortRef
_CreateMasterAddressLookup_Mach(CFStringRef name, CFHostInfoType info, CFTypeRef context, CFStreamError* error) {

	const CFAllocatorRef allocator = CFGetAllocator(name);
	UInt8* buffer;
	CFMachPortRef result = NULL;

	buffer = _CreateLookup_Common(name, error);

	if (!buffer)
		return result;

	// Got a good name to send to lookup.
	else {

        struct addrinfo hints;
		mach_port_t prt = MACH_PORT_NULL;
		CFMachPortContext ctxt = {0, (void*)context, CFRetain, CFRelease, CFCopyDescription};

		// Set up the hints for getaddrinfo
		_InitGetAddrInfoHints(info, &hints);

		// Start the async lookup
		error->error = getaddrinfo_async_start(&prt, (const char*)buffer, NULL, &hints, _GetAddrInfoCallBack, (void*)context);

		// If the callback port was created, attempt to create the CFMachPort wrapper on it.
		if (!prt ||
			!(result = CFMachPortCreateWithPort(allocator, prt, _GetAddrInfoMachPortCallBack, &ctxt, NULL)))
		{
			_HandleGetAddrInfoStatus(error->error, error, TRUE);
		}
	}

	// Release the buffer that was allocated for the name
	CFAllocatorDeallocate(allocator, buffer);

	return result;
}
#endif /* defined(__MACH__) */

#if defined(__linux__)
/* static */ int
_SignalFdModifySignalWithError(int how, int signal, sigset_t *set, CFStreamError *error)
{
	int       result;

	__CFHostTraceEnterWithFormat("how %d signal %d set %p error %p\n",
								 how, signal, set, error);

	__Require(set != NULL, done);
	__Require(error != NULL, done);

	sigemptyset(set);
	sigaddset(set, signal);

	result = pthread_sigmask(how, set, NULL);
	__Require_Action(result == 0,
					 done,
					 error->error = result;
					 error->domain = kCFStreamErrorDomainPOSIX);

 done:
	__CFHostTraceExitWithFormat("result %d\n", result);

	return result;
}

/* static */ int
_SignalFdSetSignalWithError(int signal, sigset_t *set, CFStreamError *error) {
	return (_SignalFdModifySignalWithError(SIG_BLOCK, signal, set, error));
}

/* static */ int
_SignalFdClearSignalWithError(int signal, sigset_t *set, CFStreamError *error) {
	return (_SignalFdModifySignalWithError(SIG_UNBLOCK, signal, set, error));
}

/* static */ int
_SignalFdClearGetAddrInfoSignalWithHost(_CFHost *host) {
	const int signal = __kCFHostLinuxSignalFdSignal;
	sigset_t  sigset;
	int       result;

	__CFSpinLock(&host->_lock);

	result = _SignalFdClearSignalWithError(signal, &sigset, &host->_error);

	__CFSpinUnlock(&host->_lock);

	return result;
}

/* static */ int
_CreateSignalFd(int signal, CFStreamError *error) {
	const int kInvalidExistingDescriptor = -1;
	const int flags = 0;
	sigset_t  sigset;
	int       status;
	int       result = -1;

	__CFHostTraceEnterWithFormat("signal %d error %p\n", signal, error);

	status = _SignalFdSetSignalWithError(signal, &sigset, error);
	__Require(status == 0, done);

	result = signalfd(kInvalidExistingDescriptor, &sigset, flags);
	__Require_Action(result != -1,
					 done,
					 error->error  = errno;
					 error->domain = kCFStreamErrorDomainPOSIX);

 done:
	__CFHostTraceExitWithFormat("result %d\n", result);

	return result;
}

/* static */ int
_CreateAddressLookupRequest(const char *name, CFHostInfoType info, int signal, CFStreamError *error) {
	struct sigevent      sigev;
	_CFHostGAIARequest * gai_request = NULL;
	int                  result = -1;

	__CFHostTraceEnterWithFormat("name %p (%s) info 0x%x signal %d error %p\n",
								 name, (name != NULL) ? name : "", info, signal, error);

	__Require_Action(name != NULL, done, result = -EINVAL);

	memset(&sigev, 0, sizeof(sigev));
  
	gai_request = (_CFHostGAIARequest *)CFAllocatorAllocate(kCFAllocatorDefault, sizeof(_CFHostGAIARequest), 0);
	__Require_Action(gai_request != NULL,
					 done,
					 result        = -ENOMEM;
					 error->error  = -result;
					 error->domain = kCFStreamErrorDomainPOSIX);

	_InitGetAddrInfoHints(info, &gai_request->_request_hints);

	memset(&gai_request->_request_gaicb, 0, sizeof(struct gaicb));

	gai_request->_request_gaicb.ar_name     = name;
	gai_request->_request_gaicb.ar_request  = &gai_request->_request_hints;

	gai_request->_request_list[0]           = &gai_request->_request_gaicb;

	sigev.sigev_notify          = SIGEV_SIGNAL;
	sigev.sigev_value.sival_ptr = &gai_request->_request_gaicb;
	sigev.sigev_signo           = signal;

	result = getaddrinfo_a(GAI_NOWAIT, gai_request->_request_list, 1, &sigev);

	if (result != 0) {
		_HandleGetAddrInfoStatus(result, error, TRUE);

		CFAllocatorDeallocate(kCFAllocatorDefault, gai_request);
	}

 done:
	__CFHostTraceExitWithFormat("result %d\n", result);

	return result;
}

/* static */ struct gaicb *
_SignalFdGetAddrInfoResult(CFFileDescriptorRef fdref) {
	CFFileDescriptorNativeDescriptor fd;
	struct signalfd_siginfo          fdsi;
	ssize_t                          status;
	struct gaicb *                   result = NULL;

	__CFHostTraceEnterWithFormat("fdref %p\n", fdref);

    fd = CFFileDescriptorGetNativeDescriptor(fdref);
    __Require(fd != -1, done);

	do {
		status = read(fd, &fdsi, sizeof(fdsi));
	} while ((status == -1) && (errno == EAGAIN));
	__Require(status == sizeof(fdsi), done);

	__Require(fdsi.ssi_signo == __kCFHostLinuxSignalFdSignal, done);
	__Require(fdsi.ssi_code == SI_ASYNCNL, done);

	result = (struct gaicb *)(fdsi.ssi_ptr);
	__Require(result != NULL, done);

 done:
	__CFHostTraceExitWithFormat("result %p\n", result);

	return result;
}

/* static */ void
_PrimaryAddressLookupCallBack_Linux(CFFileDescriptorRef fdref, CFOptionFlags callBackTypes, void *info) {
	int            status;
	struct gaicb * request = NULL;

	__CFHostTraceEnterWithFormat("fdref %p callBackTypes %lx info %p\n",
								 fdref, callBackTypes, info);

	// Attempt to retrieve the getaddrinfo_a result that fired the
	// completion signal that triggered this callback.

	request = _SignalFdGetAddrInfoResult(fdref);
	__Require(request != NULL, done);

	// Invoke the common, shared getaddrinfo{,_a} callback.

	_GetAddrInfoCallBack(gai_error(request), request->ar_result, info);

	// Release the buffer that was previously allocated for the lookup
	// name when the request was made as well as the request itself.

	CFAllocatorDeallocate(kCFAllocatorDefault, (void *)request->ar_name);
	CFAllocatorDeallocate(kCFAllocatorDefault, request);

 done:
	// Clear the signal we previously established to trigger this callback.

	status = _SignalFdClearGetAddrInfoSignalWithHost(info);
	__Verify(status == 0);

	CFFileDescriptorInvalidate(fdref);
	CFRelease(fdref);

	__CFHostTraceExit();
}

/* static */ CFFileDescriptorRef
_CreateAddressLookupSource_GetAddrInfo_A(int signal, CFTypeRef context, CFStreamError *error) {
	const Boolean           kCloseOnInvalidate = TRUE;
	int                     sigfd;
    CFFileDescriptorContext fdrefContext = { 0, (void *)context, NULL, NULL, NULL };
	CFFileDescriptorRef     result = NULL;

	__CFHostTraceEnterWithFormat("signal %d context %p error %p\n", signal, context, error);	

	sigfd = _CreateSignalFd(signal, error);
	__Require(sigfd != -1, done);

	result = CFFileDescriptorCreate(kCFAllocatorDefault,
									sigfd,
									kCloseOnInvalidate,
									_PrimaryAddressLookupCallBack_Linux,
									&fdrefContext);
	if (!result) {
		error->error = ENOMEM;
		error->domain = kCFStreamErrorDomainPOSIX;

		close(sigfd);

	} else {
		CFFileDescriptorEnableCallBacks(result, kCFFileDescriptorReadCallBack);

	}

 done:
	__CFHostTraceExitWithFormat("result %p\n", result);

	return result;
}

static int
_AresStatusMapToAddrInfoError(int ares_status)
{
    int result;

    switch (ares_status) {

    case ARES_SUCCESS:
        result = 0;
        break;

    case ARES_ENODATA:
        result = EAI_NODATA;
        break;

    case ARES_ENOMEM:
        result = EAI_MEMORY;
        break;

    case ARES_ECANCELLED:
        result = EAI_CANCELED;
        break;

    case ARES_ENONAME:
        result = EAI_NONAME;
        break;

    case ARES_EBADFLAGS:
        result = EAI_BADFLAGS;
        break;

    case ARES_EBADFAMILY:
        result = EAI_ADDRFAMILY;
        break;

    case ARES_EFORMERR:
    case ARES_ESERVFAIL:
    case ARES_ENOTFOUND:
    case ARES_ENOTIMP:
    case ARES_EREFUSED:
    case ARES_EBADQUERY:
    case ARES_EBADNAME:
    case ARES_EBADRESP:
    case ARES_ECONNREFUSED:
    case ARES_ETIMEOUT:
    case ARES_EOF:
    case ARES_EFILE:
    case ARES_EBADSTR:
    case ARES_EBADHINTS:
    case ARES_ENOTINITIALIZED:
    case ARES_ELOADIPHLPAPI:
    case ARES_EADDRGETNETWORKPARAMS:
    case ARES_EDESTRUCTION:
    default:
        result = EAI_FAIL;
        break;

    }

    return result;
}

static void
_AresStatusMapToStreamError(int status, CFStreamError *error) {
    switch (status) {

    case ARES_SUCCESS:
        error->error = 0;
        error->domain = (CFStreamErrorDomain)kCFStreamErrorDomainNetDB;
        break;

    case ARES_ENODATA:
        error->error = EAI_NODATA;
        error->domain = (CFStreamErrorDomain)kCFStreamErrorDomainNetDB;
        break;

    case ARES_ENOMEM:
        error->error = EAI_MEMORY;
        error->domain = (CFStreamErrorDomain)kCFStreamErrorDomainNetDB;
        break;

    case ARES_ECANCELLED:
        error->error = EAI_CANCELED;
        error->domain = (CFStreamErrorDomain)kCFStreamErrorDomainNetDB;
        break;

    case ARES_ENONAME:
        error->error = EAI_NONAME;
        error->domain = (CFStreamErrorDomain)kCFStreamErrorDomainNetDB;
        break;


    case ARES_EBADFLAGS:
        error->error = EAI_BADFLAGS;
        error->domain = (CFStreamErrorDomain)kCFStreamErrorDomainNetDB;
        break;

    case ARES_EBADFAMILY:
        error->error = EAI_ADDRFAMILY;
        error->domain = (CFStreamErrorDomain)kCFStreamErrorDomainNetDB;
        break;

    case ARES_EFORMERR:
    case ARES_ESERVFAIL:
    case ARES_ENOTFOUND:
    case ARES_ENOTIMP:
    case ARES_EREFUSED:
    case ARES_EBADQUERY:
    case ARES_EBADNAME:
    case ARES_EBADRESP:
    case ARES_ECONNREFUSED:
    case ARES_ETIMEOUT:
    case ARES_EOF:
    case ARES_EFILE:
    case ARES_EBADSTR:
    case ARES_EBADHINTS:
    case ARES_ENOTINITIALIZED:
    case ARES_ELOADIPHLPAPI:
    case ARES_EADDRGETNETWORKPARAMS:
    case ARES_EDESTRUCTION:
    default:
        error->error  = EAI_FAIL;
        error->domain = (CFStreamErrorDomain)kCFStreamErrorDomainNetDB;
        break;

    }
}

static void
_AresFileDescriptorRefCallBack(CFFileDescriptorRef fdref, CFOptionFlags callBackTypes, void *info) {
    _CFHostAresRequest * ares_request = (_CFHostAresRequest *)(info);
    CFFileDescriptorNativeDescriptor fd;
    ares_socket_t readfd;
    ares_socket_t writefd;

	__CFHostTraceEnterWithFormat("fdref %p callBackTypes %lx info %p\n",
								 fdref, callBackTypes, info);

    __CFHostMaybeLog("%d: ares_request %p\n", __LINE__, ares_request);

    fd = CFFileDescriptorGetNativeDescriptor(fdref);
    __Require(fd != -1, done);

    __CFHostMaybeLog("%d: fd %d\n", __LINE__, fd);

    readfd = (ares_request->_request_events & POLLIN) ? fd : ARES_SOCKET_BAD;
    writefd = (ares_request->_request_events & POLLOUT) ? fd : ARES_SOCKET_BAD;

    __CFHostMaybeLog("%d: readfd %d writefd %d\n",
                     __LINE__,
                     readfd,
                     writefd);

    ares_process_fd(ares_request->_request_channel,
                    readfd,
                    writefd);

    if (ares_request->_request_pending == 0) {
        ares_destroy(ares_request->_request_channel);

        // Release the request.

        CFAllocatorDeallocate(kCFAllocatorDefault, ares_request);
    }

 done:
    __CFHostTraceExit();
}

static void
_AresSocketStateCallBack(void *data,
                         ares_socket_t socket_fd,
                         int readable,
                         int writable) {
	const Boolean           kCloseOnInvalidate = TRUE;
    _CFHostAresRequest *    ares_request = (_CFHostAresRequest *)(data);
    CFFileDescriptorContext fdContext = { 0, (void *)ares_request, NULL, NULL, NULL };
	CFFileDescriptorRef     f = NULL;

    __CFHostTraceEnterWithFormat("data %p socket_fd %d readable %d writable %d\n",
                                 data, socket_fd, readable, writable);

    __CFHostMaybeLog("%d: ares_request %p\n", __LINE__, ares_request);

    if (ares_request != NULL) {
        if (ares_request->_request_lookup == NULL) {
            __CFHostMaybeLog("Allocating new CF file descriptor...\n");
            f = CFFileDescriptorCreate(kCFAllocatorDefault,
                                       socket_fd,
                                       !kCloseOnInvalidate,
                                       _AresFileDescriptorRefCallBack,
                                       &fdContext);

            __CFHostMaybeLog("%d: f %p\n", __LINE__, f);

            if (!f) {
                ares_request->_request_error->error = ENOMEM;
                ares_request->_request_error->domain = kCFStreamErrorDomainPOSIX;

            } else {
                ares_request->_request_lookup = f;

            }
        }

        if (ares_request->_request_lookup != NULL) {
            __CFHostMaybeLog("Updating events and callbacks on existing CF file descriptor...\n");
            if (readable) {
                CFFileDescriptorEnableCallBacks(ares_request->_request_lookup, kCFFileDescriptorReadCallBack);
                ares_request->_request_events |= POLLIN;
            } else {
                ares_request->_request_events &= ~POLLIN;
            }

            if (writable) {
                CFFileDescriptorEnableCallBacks(ares_request->_request_lookup, kCFFileDescriptorWriteCallBack);
                ares_request->_request_events |= POLLOUT;
            } else {
                ares_request->_request_events &= ~POLLOUT;
            }

            __CFHostMaybeLog("%d: ares_request (%p)->_request_events %hx\n",
                             __LINE__,
                             ares_request,
                             ares_request->_request_events);
        }
    }

    __CFHostTraceExit();
}

static void 
LogName(const char *aType, const char *aName)
{
    __CFHostMaybeLog("%s: %s\n", aType, aName);
}

static void
LogAddress(int aFamily, const char *aData)
{
    const size_t buflen = INET6_ADDRSTRLEN;
    char         buffer[INET6_ADDRSTRLEN];
    const void * addr;
    socklen_t    addrlen;
    const char * result;

    switch (aFamily) {

    case AF_INET:
        addr = aData;
        addrlen = sizeof(struct in_addr);
        break;

    case AF_INET6:
        addr = aData;
        addrlen = sizeof(struct in6_addr);
        break;

    default:
        addr = NULL;
        addrlen = 0;
        break;

    }
    
    if ((addr != NULL) && (addrlen > 0)) {
        result = ares_inet_ntop(aFamily, addr, buffer, buflen);

        if (result)
        {
            __CFHostMaybeLog("%s\n", buffer);
        }
    }
}

static void
_AresFreeAddrInfo(struct addrinfo *res) {
    struct addrinfo *current;
    struct addrinfo * volatile next;

    for (current = res; current != NULL; current = next) {
        next = current->ai_next;
        CFAllocatorDeallocate(kCFAllocatorDefault, current);
    }
}

static struct addrinfo *
_AresHostentToAddrInfo(const struct hostent *hostent, CFStreamError *error) {
    int               status   = 0;
    int               i;
    char *            current  = NULL;
    struct addrinfo * first    = NULL;
    struct addrinfo * previous = NULL;
    struct addrinfo * result   = NULL;

    __CFHostTraceEnterWithFormat("hostent %p\n", hostent);

    __Require_Action(hostent != NULL, map_status, status = EINVAL);
    __Require_Action(hostent->h_name != NULL, map_status, status = EINVAL);
    __Require_Action(hostent->h_addr_list != NULL, map_status, status = EINVAL);
    __Require_Action(error != NULL, map_status, status = EINVAL);

    for (i = 0; ((current = hostent->h_addr_list[i]) != NULL); i++)
    {
        const char kNull = '\0';
        const int family = hostent->h_addrtype;
        const size_t canonname_len = strlen(hostent->h_name) + sizeof(kNull);
        size_t addr_size;
        size_t total_size;
        struct sockaddr_in *saddr;
        struct sockaddr_in6 *saddr6;

        __CFHostMaybeLog("strlen(hostent->h_name) %zu sizeof(kNull) %zu canonname_len %zu\n", strlen(hostent->h_name), sizeof(kNull), canonname_len);

        switch (family) {

        case AF_INET:
            addr_size = sizeof(struct sockaddr_in);
            break;

        case AF_INET6:
            addr_size = sizeof(struct sockaddr_in6);
            break;

        default:
            addr_size = 0;
            break;

        }

        __Require_Action(addr_size > 0,
                         done,
                         error->error  = EAI_ADDRFAMILY;
                         error->domain = (CFStreamErrorDomain)kCFStreamErrorDomainNetDB);

        total_size = sizeof(struct addrinfo) + canonname_len + addr_size;

        result = CFAllocatorAllocate(kCFAllocatorDefault, total_size, 0);
        __Require_Action(result != NULL, map_status, status = ENOMEM);

        memset(result, 0, total_size);

        result->ai_addr      = (struct sockaddr *)((uint8_t *)result + sizeof(struct addrinfo));
        result->ai_canonname = (char *)((uint8_t *)result->ai_addr + addr_size);

        result->ai_family   = family;
        result->ai_socktype = SOCK_STREAM;
        result->ai_addrlen  = addr_size;

        memcpy(result->ai_canonname, hostent->h_name, canonname_len);

        switch (family) {

        case AF_INET:
            saddr = (struct sockaddr_in *)result->ai_addr;

            memcpy(&saddr->sin_addr, current, sizeof(struct in_addr));
            saddr->sin_family = family;
            break;

        case AF_INET6:
            saddr6 = (struct sockaddr_in6 *)result->ai_addr;

            memcpy(&saddr6->sin6_addr, current, sizeof(struct in6_addr));
            saddr6->sin6_family = family;
            break;

        }

        if (first == NULL)
            first = result;

        if (previous != NULL)
            previous->ai_next = result;

        previous = result;
    }

 map_status:
    if (status != 0) {
        error->error = status;
        error->domain = kCFStreamErrorDomainPOSIX;

        if (first != NULL) {
            _AresFreeAddrInfo(first);
            first = NULL;
        }
    }

done:
    __CFHostTraceExitWithFormat("first %p\n", first);

    return first;
}

static void
_AresAccumulateAddrInfo(_CFHostAresRequest *ares_request, struct addrinfo *ai) {
    struct addrinfo *tail;

    __CFHostTraceEnterWithFormat("ares_request %p ai %p\n",
                                 ares_request, ai);

    __Require(ares_request != NULL, done);
    __Require(ai != NULL, done);

    tail = ai;

    // Find the tail node of the provided list.

    while (tail->ai_next) {
        tail = tail->ai_next;
    }

    // Point the tail node of the provided list at the last
    // accumulated result.

    tail->ai_next = ares_request->_request_addrinfo;

    // Point the last accumulated result at the provided list.

    ares_request->_request_addrinfo = ai;

 done:
  __CFHostTraceExit();
}

static void
_AresQueryCompletedCallBack(void *arg,
                            int status,
                            int timeouts,
                            struct hostent *hostent) {
    _CFHostAresRequest *ares_request = (_CFHostAresRequest *)(arg);

    __CFHostTraceEnterWithFormat("arg %p status %d timeouts %d hostent %p\n",
                                 arg, status, timeouts, hostent);

    __CFHostMaybeLog("%d: ares_request %p\n", __LINE__, ares_request);

    if (status == ARES_SUCCESS)
    {
        if (hostent != NULL)
        {
            int i;
            char *current;
            struct addrinfo *ai;

            if (hostent->h_name != NULL)
            {
                LogName("hostname", hostent->h_name);
            }

            if (hostent->h_aliases != NULL)
            {
                for (i = 0; ((current = hostent->h_aliases[i]) != NULL); i++)
                {
                    LogName("alias", current);
                }
            }

            if (hostent->h_addr_list != NULL)
            {
                for (i = 0; ((current = hostent->h_addr_list[i]) != NULL); i++)
                {
                    LogAddress(hostent->h_addrtype,
                               current);
                }
            }

            if (ares_request->_request_name != NULL) {
                ai = _AresHostentToAddrInfo(hostent, ares_request->_request_error);
                if (ai != NULL) {
                    _AresAccumulateAddrInfo(ares_request, ai);
            }

            __CFHostMaybeLog("%d: ares_request (%p)->_request_pending %zu\n",
                             __LINE__,
                             ares_request,
                             ares_request->_request_pending);

            if (ares_request->_request_pending > 0)
            {
                ares_request->_request_pending--;
            }

            if (ares_request->_request_pending > 0) {
                __CFHostMaybeLog("There are still lookup requests pending...\n");

                if (ares_request->_request_lookup != NULL) {
                    __CFHostMaybeLog("Re-enable callbacks!\n");

                    if (ares_request->_request_events & POLLIN) {
                        CFFileDescriptorEnableCallBacks(ares_request->_request_lookup,
                                                        kCFFileDescriptorReadCallBack);
                    }

                    if (ares_request->_request_events & POLLOUT) {
                        CFFileDescriptorEnableCallBacks(ares_request->_request_lookup,
                                                        kCFFileDescriptorWriteCallBack);
                    }
                }
            } else if (ares_request->_request_pending == 0) {
                __CFHostMaybeLog("There are no more lookup requests pending, cleaning up...\n");

                if (ares_request->_request_name != NULL) {
                    // Invoke the common, shared getaddrinfo{,_a} callback.

                    _GetAddrInfoCallBackWithFree(_AresStatusMapToAddrInfoError(status),
                                                 ares_request->_request_addrinfo,
                                                 ares_request->_request_host,
                                                 _AresFreeAddrInfo);
                }

                if (ares_request->_request_lookup != NULL) {
                    CFFileDescriptorInvalidate(ares_request->_request_lookup);
                    CFRelease(ares_request->_request_lookup);
                    ares_request->_request_lookup = NULL;
                }

                // Release the buffer that was previously allocated
                // for the lookup name when the request was made.

                CFAllocatorDeallocate(kCFAllocatorDefault, (void *)ares_request->_request_name);
            }
        }
    } else if (status == ARES_ECANCELLED) {
        __CFHostMaybeLog("%d: ares_request %p is being cancelled\n",
                         __LINE__, ares_request);
        __CFHostMaybeLog("%d: ares_request (%p)->_request_pending %zu\n",
                         __LINE__,
                         ares_request,
                         ares_request->_request_pending);
    } else {
        if (ares_request->_request_pending > 0)
        {
            ares_request->_request_pending--;
        }
    }

    ares_request->_request_status = status;

    __CFHostTraceExit();
}

/* static */ CFFileDescriptorRef
_CreatePrimaryAddressLookup_Linux_Ares(CFStringRef name, CFHostInfoType info, CFTypeRef context, CFStreamError* error) {
	const CFAllocatorRef allocator = CFGetAllocator(name);
    const int            optmask = ARES_OPT_SOCK_STATE_CB;
	UInt8*               buffer;
    struct ares_options  options;
    _CFHostAresRequest * ares_request = NULL;
    int                  status;
    Boolean              ipv4only = FALSE;
    Boolean              ipv6only = FALSE;
    CFFileDescriptorRef  result = NULL;

	__CFHostTraceEnterWithFormat("name %p info %x context %p error %p\n",
							name, info, context, error);

	// Create a CFString representation of the lookup by converting it
	// into a null-terminated C string buffer consumable by
	// getaddrinfo_a.

	buffer = _CreateLookup_Common(name, error);
	__Require(buffer != NULL, done);

	ares_request = (_CFHostAresRequest *)CFAllocatorAllocate(kCFAllocatorDefault, sizeof(_CFHostAresRequest), 0);
	__Require_Action(ares_request != NULL,
					 done,
					 error->error  = ENOMEM;
					 error->domain = kCFStreamErrorDomainPOSIX;
                     CFAllocatorDeallocate(allocator, buffer));

    __CFHostMaybeLog("%d: ares_request %p\n", __LINE__, ares_request);

    memset(ares_request, 0, sizeof(_CFHostAresRequest));

    ares_request->_request_name    = (const char *)buffer;
    ares_request->_request_error   = error;
    ares_request->_request_host    = (_CFHost *)context;

    __CFHostMaybeLog("%d: ares_request (%p)->_request_name %s\n",
                     __LINE__,
                     ares_request,
                     ares_request->_request_name);

    options.sock_state_cb      = _AresSocketStateCallBack;
    options.sock_state_cb_data = ares_request;

    status = ares_init_options(&ares_request->_request_channel,
                               &options,
                               optmask);
    __Require_Action(status == ARES_SUCCESS,
                     done,
                     _AresStatusMapToStreamError(status, error);
                     CFAllocatorDeallocate(kCFAllocatorDefault, ares_request);
                     CFAllocatorDeallocate(allocator, buffer));

	if (info == _kCFHostIPv4Addresses) {
        ipv4only = TRUE;
        ares_request->_request_pending = 1;
	} else if (info == _kCFHostIPv6Addresses) {
        ipv6only = TRUE;
        ares_request->_request_pending = 1;
	} else {
        ares_request->_request_pending = 2;
    }

    __CFHostMaybeLog("%d: ares_request (%p)->_request_pending %zu\n",
                     __LINE__,
                     ares_request,
                     ares_request->_request_pending);

    if (!ipv6only) {
        ares_gethostbyname(ares_request->_request_channel,
                           ares_request->_request_name,
                           AF_INET,
                           _AresQueryCompletedCallBack,
                           ares_request);
    }

    if (!ipv4only) {
        ares_gethostbyname(ares_request->_request_channel,
                           ares_request->_request_name,
                           AF_INET6,
                           _AresQueryCompletedCallBack,
                           ares_request);
    }

    __CFHostMaybeLog("%d: ares_request (%p)->_request_pending %zu\n",
                     __LINE__,
                     ares_request,
                     ares_request->_request_pending);

    result = ares_request->_request_lookup;

 done:
	__CFHostTraceExitWithFormat("result %p\n", result);

	return result;
}

# if HAVE_GETADDRINFO_A && 0
/* static */ CFFileDescriptorRef
_CreatePrimaryAddressLookup_Linux_GetAddrInfo_A(CFStringRef name, CFHostInfoType info, CFTypeRef context, CFStreamError* error) {
	const CFAllocatorRef    allocator = CFGetAllocator(name);
	UInt8*                  buffer;
	const int               signal = __kCFHostLinuxSignalFdSignal;
	CFFileDescriptorRef     result = NULL;
	int                     status;

	__CFHostTraceEnterWithFormat("name %p info %x context %p error %p\n",
							name, info, context, error);

	// Create a CFString representation of the lookup by converting it
	// into a null-terminated C string buffer consumable by
	// getaddrinfo_a.

	buffer = _CreateLookup_Common(name, error);
	__Require(buffer != NULL, done);

	// Create the CFFileDescriptor-based lookup source that will
	// handle the I/O for the asynchronous getaddrinfo_a call.

	result = _CreateAddressLookupSource_GetAddrInfo_A(signal, context, error);
	__Require_Action(result != NULL, done, CFAllocatorDeallocate(allocator, buffer));

	status = _CreateAddressLookupRequest((const char *)buffer, info, signal, error);
	if (status != 0) {
		_HandleGetAddrInfoStatus(status, error, TRUE);

		CFAllocatorDeallocate(allocator, buffer);
		CFFileDescriptorInvalidate(result);
		CFRelease(result);

		result = NULL;
	}

 done:
	__CFHostTraceExitWithFormat("result %p\n", result);

	return result;
}
#endif /* # if HAVE_GETADDRINFO_A && 0 */
#endif /* #if defined(__linux__) */

/* static */ CFTypeRef
_CreateAddressLookup(CFStringRef name, CFHostInfoType info, void* context, CFStreamError* error) {
	Boolean started = FALSE;
	CFTypeRef result = NULL;

	memset(error, 0, sizeof(error[0]));

	if (info == _kCFHostMasterAddressLookup)
		result = _CreateMasterAddressLookup(name, info, context, error);

	else {
		CFHostRef host = NULL;
		CFMutableArrayRef list = NULL;

		/* Lock the master lookups list and cache */
		_CFMutexLock(_HostLock);

		/* Get the list with the host lookup and other sources for this name */
		list = (CFMutableArrayRef)CFDictionaryGetValue(_HostLookups, name);

		/* Get the host if there is a list.  Host is at index zero. */
		if (list)
			host = (CFHostRef)CFArrayGetValueAtIndex(list, 0);

		/* If there is no list, this is the first; so set everything up. */
		else {

			/* Create the list to hold the host and sources. */
			list = CFArrayCreateMutable(kCFAllocatorDefault, 0, &kCFTypeArrayCallBacks);

			/* Set up the error in case the list wasn't created. */
			if (!list) {
				error->error = ENOMEM;
				error->domain = kCFStreamErrorDomainPOSIX;
			}

			else {

				name = CFStringCreateCopy(kCFAllocatorDefault, name);

				/* Add the list of clients for the name to the dictionary. */
				CFDictionaryAddValue(_HostLookups, name, list);

				CFRelease(name);

				/* Dictionary holds it now. */
				CFRelease(list);

				/* Make the real lookup. */
				host = CFHostCreateWithName(kCFAllocatorDefault, name);

				if (!host) {
					error->error = ENOMEM;
					error->domain = kCFStreamErrorDomainPOSIX;
				}

				else {
					CFHostClientContext ctxt = {0, (void*)name, CFRetain, CFRelease, CFCopyDescription};

					/* Place the CFHost at index 0. */
					CFArrayAppendValue(list, host);

					/* The list holds it now. */
					CFRelease(host);

					/* Set the client for asynchronous callback. */
					CFHostSetClient(host, (CFHostClientCallBack)_MasterCallBack, &ctxt);

					/* Kick off the resolution.  NULL the client if the resolution can't start. */
					started = CFHostStartInfoResolution(host, _kCFHostMasterAddressLookup, error);
					if (!started) {

						// It is absolutely imperative that
						// CFHostStartInfoResolution (or its
						// info-type-specific helpers) set an error of
						// some sort if it (they) failed. In response
						// to failure, the name/list key/value pair
						// will be removed from _HostLookups and,
						// along with them, the host will then be
						// invalid and go out of scope.
						//
						// If processing continues on the false
						// assumption that there were no errors,
						// execution flow will fault when the newly
						// created run loop source below is added to a
						// list that is no longer valid.

						// CFAssert2(error->error != 0, __kCFLogAssertion, ""resolution failed but error is not set");

						CFHostSetClient(host, NULL, NULL);

						/* If it failed, don't keep it in the outstanding lookups list. */
						CFDictionaryRemoveValue(_HostLookups, name);

						// Name, host, and list are no longer valid
						// and in scope at this point. A stream error
						// MUST be set, per the comment above or any
						// manipulation of name, list, or host
						// hereafter will fault.
					}
				}
			}
		}

		/* Everything is still good? */
		if (started && !error->error) {

			CFRunLoopSourceContext ctxt = {
				0,
				context,
				CFRetain,
				CFRelease,
				CFCopyDescription,
				NULL,
				NULL,
				(void (*)(void*, CFRunLoopRef, CFStringRef))_AddressLookupSchedule_NoLock,
				NULL,
				(void (*)(void*))_AddressLookupPerform
			};

			/* Create the lookup source.  This source will be signalled once the shared lookup finishes. */
			result = CFRunLoopSourceCreate(CFGetAllocator(name), 0, &ctxt);

			/* If it succeed, add it to the list of other pending clients. */
			if (result) {
				CFArrayAppendValue(list, result);
			}

			else {

				error->error = ENOMEM;
				error->domain = kCFStreamErrorDomainPOSIX;

				/* If this was going to be the only client, need to clean up. */
				if (host && CFArrayGetCount(list) == 1) {

					/* NULL the client for the Mmster lookup and cancel it. */
					CFHostSetClient(host, NULL, NULL);
					CFHostCancelInfoResolution(host, _kCFHostMasterAddressLookup);

					/* Remove it from the list of pending lookups and clients. */
					CFDictionaryRemoveValue(_HostLookups, name);
				}
			}
		}

		_CFMutexUnlock(_HostLock);
	}

	return result;
}

/* static */ CFTypeRef
_CreateNameLookup(CFDataRef address, void* context, CFStreamError* error) {
	CFTypeRef result = NULL;

#if defined(__MACH__)
	result = _CreateNameLookup_Mach(address, context, error);
#elif defined(__linux__)
# if HAVE_ARES_INIT && 1
    result = _CreateNameLookup_Linux_Ares(address, context, error);
# else
#  error "No Linux name (Reverse DNS) lookup implementation!"
# endif /* HAVE_ARES_INIT && 1 */
#else
#warning "Platform portability issue!"
#endif /* defined(__MACH__) */

	return result;
}

#if defined(__MACH__)
/* static */ CFMachPortRef
_CreateNameLookup_Mach(CFDataRef address, void* context, CFStreamError* error) {

	mach_port_t prt = MACH_PORT_NULL;
	CFMachPortRef result = NULL;

	CFMachPortContext ctxt = {0, (void*)context, CFRetain, CFRelease, CFCopyDescription};
	struct sockaddr* sa = (struct sockaddr*)CFDataGetBytePtr(address);

	// Start the async lookup
	error->error = getnameinfo_async_start(&prt, sa, sa->sa_len, 0, _GetNameInfoCallBack, (void*)context);

	// If the callback port was created, attempt to create the CFMachPort wrapper on it.
	if (!prt ||
		!(result = CFMachPortCreateWithPort(CFGetAllocator(address), prt, _GetNameInfoMachPortCallBack, &ctxt, NULL)))
	{
		_HandleGetAddrInfoStatus(error->error, error, TRUE);
	}

	// Return the CFMachPortRef
	return result;
}
#endif /* #if defined(__MACH__) */

#if defined(__linux__)
#if HAVE_ARES_INIT && 1
/* static */ CFFileDescriptorRef
_CreateNameLookup_Linux_Ares(CFDataRef address, void* context, CFStreamError* error) {
	CFFileDescriptorRef     result = NULL;

	__CFHostTraceEnterWithFormat("address %p context %p error %p\n",
							address, context, error);

	if (error) {
		error->error = EOPNOTSUPP;
		error->domain = kCFStreamErrorDomainPOSIX;
	}

	__CFHostTraceExitWithFormat("result %p\n", result);

	return result;
}
#endif /* HAVE_ARES_INIT && 1 */
#endif /* defined(__linux__) */

#if defined(__MACH__)
/* static */ SCNetworkReachabilityRef
_CreateReachabilityLookup(CFTypeRef thing, void* context, CFStreamError* error) {

	SCNetworkReachabilityRef result = NULL;

	// If the passed in argument is a CFData, create the reachability object
	// with the address.
	if (CFGetTypeID(thing) == CFDataGetTypeID()) {
		result = SCNetworkReachabilityCreateWithAddress(CFGetAllocator(thing),
														(struct sockaddr*)CFDataGetBytePtr((CFDataRef)thing));
	}

	// A CFStringRef means to create a reachability object by name.
	else {
		const CFAllocatorRef allocator = CFGetAllocator(thing);
		UInt8* buffer;

		buffer = _CreateLookup_Common(thing, error);

		if (!buffer)
			return result;

		// Got a good name to send to lookup.
		else {

			// Create the reachability lookup
			result = SCNetworkReachabilityCreateWithName(allocator, (const char*)buffer);
		}

		// Release the buffer that was allocated for the name
		CFAllocatorDeallocate(allocator, buffer);
	}

	// If the reachability object was created, need to set the callback context.
	if (result) {
		SCNetworkReachabilityContext ctxt = {0, (void*)context, CFRetain, CFRelease, CFCopyDescription};

		// Set the callback information
		SCNetworkReachabilitySetCallback(result, _NetworkReachabilityCallBack, &ctxt);
	}

	// If no reachability was created, make sure the error is set.
	else if (!error->error) {

		// Set it to errno
		error->error = errno;

		// If errno was set, place in the POSIX error domain.
		if (error->error)
			error->domain = (CFStreamErrorDomain)kCFStreamErrorDomainPOSIX;
	}

	return result;
}
#endif /* defined(__MACH__) */

#if defined(__MACH__)
/* static */ CFMachPortRef
_CreateDNSLookup(CFTypeRef thing, CFHostInfoType type, void* context, CFStreamError* error) {
	const CFAllocatorRef allocator = CFGetAllocator(thing);
	UInt8* buffer;
	CFMachPortRef result = NULL;

	buffer = _CreateLookup_Common(thing, error);

	if (!buffer)
		return result;

	// Got a good name to send to lookup.
	else {

		mach_port_t prt = MACH_PORT_NULL;
		CFMachPortContext ctxt = {0, (void*)context, CFRetain, CFRelease, CFCopyDescription};

		// Start the async lookup
		error->error = dns_async_start(&prt, (const char*)buffer, ((type & 0xFFFF0000) >> 16), (type & 0x0000FFFF), 1, _DNSCallBack, (void*)context);

		// If the callback port was created, attempt to create the CFMachPort wrapper on it.
		if (!prt ||
			!(result = CFMachPortCreateWithPort(allocator, prt, _DNSMachPortCallBack, &ctxt, NULL)))
		{
			_HandleGetAddrInfoStatus(error->error, error, TRUE);
		}
	}

	// Release the buffer that was allocated for the name
	CFAllocatorDeallocate(allocator, buffer);

	return result;
}
#endif /* defined(__MACH__) */

#if defined(__linux__)
/* static */ CFFileDescriptorRef
_CreateDNSLookup(CFTypeRef thing, CFHostInfoType type, void* context, CFStreamError* error) {
	const CFAllocatorRef    allocator = CFGetAllocator(thing);
	UInt8*                  buffer;
	const int               signal = __kCFHostLinuxSignalFdSignal;
	CFFileDescriptorRef     result = NULL;
	int                     status;

	__CFHostTraceEnterWithFormat("thing %p type %x context %p error %p\n",
								 thing, type, context, error);

	CFShow(thing);

	// Create a CFString representation of the lookup by converting it
	// into a null-terminated C string buffer consumable by
	// getaddrinfo_a.

	buffer = _CreateLookup_Common(thing, error);
	__Require(buffer != NULL, done);

	// Create the CFFileDescriptor-based lookup source that will
	// handle the I/O for the asynchronous getaddrinfo_a call.

	result = _CreateAddressLookupSource_GetAddrInfo_A(signal, context, error);
	__Require_Action(result != NULL, done, CFAllocatorDeallocate(allocator, buffer));

	status = _CreateAddressLookupRequest((const char *)buffer, type, signal, error);
	if (status != 0) {
		_HandleGetAddrInfoStatus(status, error, TRUE);

		CFAllocatorDeallocate(allocator, buffer);
		CFFileDescriptorInvalidate(result);
		CFRelease(result);

		result = NULL;
	}

 done:
	__CFHostTraceExitWithFormat("result %p\n", result);

	return result;
}
#endif /* defined(__linux__) */

typedef void (*FreeAddrInfoCallBack)(struct addrinfo *res);

/* static */ void
_GetAddrInfoCallBackWithFree(int eai_status, const struct addrinfo *res, void *ctxt, FreeAddrInfoCallBack freeaddrinfo_cb) {
	_CFHost* host = (_CFHost*)ctxt;
	CFHostClientCallBack cb = NULL;
	CFStreamError error;
	void* info = NULL;
	CFHostInfoType type = _kCFNullHostInfoType;

	// Retain here to guarantee safety really after the lookups release,
	// but definitely before the callback.
	CFRetain((CFHostRef)host);

	// Lock the host
	__CFSpinLock(&host->_lock);

	// If the lookup canceled, don't need to do any of this.
	if (host->_lookup) {

		// Make sure to toss the cached info now.
		CFDictionaryRemoveValue(host->_info, (const void*)(host->_type));

		// Set the error if got one back from getaddrinfo
		if (eai_status) {
			_HandleGetAddrInfoStatus(eai_status, &host->_error, FALSE);

			// Mark to indicate the resolution was performed.
			CFDictionaryAddValue(host->_info, (const void*)(host->_type), kCFNull);
		}

		else {

			CFMutableArrayRef addrs;
			CFAllocatorRef allocator = CFGetAllocator((CFHostRef)host);

			// This is the list of new addresses to be saved.
			addrs = CFArrayCreateMutable(allocator, 0, &kCFTypeArrayCallBacks);

			// Save the memory error if the address cache failed to create.
			if (!addrs) {
				host->_error.error = ENOMEM;
				host->_error.domain = kCFStreamErrorDomainPOSIX;

				// Mark to indicate the resolution was performed.
				CFDictionaryAddValue(host->_info, (const void*)(host->_type), kCFNull);
			}

			else {
				const struct addrinfo* i;
				
				// Loop through all of the addresses saving them in the array.
				for (i = res; i; i = i->ai_next) {

					CFDataRef data = NULL;
					CFIndex length = 0;
					
					// Bypass any address families that are not understood by CFSocketStream
					if (i->ai_addr->sa_family != AF_INET && i->ai_addr->sa_family != AF_INET6)
						continue;

					// Wrap the address in a CFData
#if defined(__MACH__)
					length = i->ai_addr->sa_len;
#else
					if (i->ai_addr->sa_family == AF_INET)
						length = sizeof(struct sockaddr_in);
					else if (i->ai_addr->sa_family == AF_INET6)
						length = sizeof(struct sockaddr_in6);
#endif /* defined(__MACH__) */
					if (length > 0) {
						data = CFDataCreate(allocator, (UInt8*)(i->ai_addr), length);
					}

					// Fail with a memory error if the address wouldn't wrap.
					if (!data) {

						host->_error.error = ENOMEM;
						host->_error.domain = kCFStreamErrorDomainPOSIX;

						// Release the addresses and mark as NULL so as not to save later.
						CFRelease(addrs);
						addrs = NULL;

						// Just fail now.
						break;
					}

					// Add the address and continue on to the next.
					CFArrayAppendValue(addrs, data);
					CFRelease(data);
				}

				// If the list is still good, need to save it.
				if (addrs) {

					// Save the list of address on the host.
					CFDictionaryAddValue(host->_info, (const void*)(host->_type), addrs);
					CFRelease(addrs);
				}
			}
		}

		// Save the callback if there is one at this time.
		cb = host->_callback;

		type = host->_type;

		// Save the error and client information for the callback
		memmove(&error, &(host->_error), sizeof(error));
		info = host->_client.info;

		// Remove the lookup from run loops and modes
		_CFTypeUnscheduleFromMultipleRunLoops(host->_lookup, host->_schedules);

		// Go ahead and invalidate the lookup
		_CFTypeInvalidate(host->_lookup);

		// Release the lookup now.
		CFRelease(host->_lookup);
		host->_lookup = NULL;
		host->_type = _kCFNullHostInfoType;
	}

	// Unlock the host so the callback can be made safely.
	__CFSpinUnlock(&host->_lock);

	// Release the results if some were received.
    if (res) {
        if (freeaddrinfo_cb) {
            freeaddrinfo_cb((struct addrinfo *)res);
        }
	}

	// If there is a callback, inform the client of the finish.
	if (cb) {
		cb((CFHostRef)host, type, &error, info);
	}

	// Go ahead and release now that the callback is done.
	CFRelease((CFHostRef)host);
}

/* static */ void
_GetAddrInfoCallBack(int eai_status, const struct addrinfo* res, void* ctxt) {
    _GetAddrInfoCallBackWithFree(eai_status, res, ctxt, freeaddrinfo);
}

#if defined(__MACH__)
/* static */ void
_GetAddrInfoMachPortCallBack(CFMachPortRef port, void* msg, CFIndex size, void* info) {

	getaddrinfo_async_handle_reply(msg);
}

/* static */ void
_GetNameInfoCallBackWithFree(int eai_status, char *hostname, char *serv, void* ctxt, FreeNameInfoCallBack freenameinfo_cb) {

	_CFHost* host = (_CFHost*)ctxt;
	CFHostClientCallBack cb = NULL;
	CFStreamError error;
	void* info = NULL;

	// Retain here to guarantee safety really after the lookups release,
	// but definitely before the callback.
	CFRetain((CFHostRef)host);

	// Lock the host
	__CFSpinLock(&host->_lock);

	// If the lookup canceled, don't need to do any of this.
	if (host->_lookup) {

		// Make sure to toss the cached info now.
		CFDictionaryRemoveValue(host->_info, (const void*)kCFHostNames);

		// Set the error if got one back from getnameinfo
		if (eai_status) {
			_HandleGetAddrInfoStatus(eai_status, &host->_error, FALSE);

			// Mark to indicate the resolution was performed.
			CFDictionaryAddValue(host->_info, (const void*)kCFHostNames, kCFNull);
		}

		else {

			CFAllocatorRef allocator = CFGetAllocator((CFHostRef)host);

			// Create the name from the given response.
			CFStringRef name = CFStringCreateWithCString(allocator, hostname, kCFStringEncodingUTF8);

			// If didn't create the name, fail with out of memory.
			if (!name) {
				host->_error.error = ENOMEM;
				host->_error.domain = kCFStreamErrorDomainPOSIX;
			}

			else {
				// Create the list to hold the name.
				CFArrayRef names = CFArrayCreate(allocator, (const void**)(&name), 1, &kCFTypeArrayCallBacks);

				// Don't need the retain anymore
				CFRelease(name);

				// Failed to create the list of names so mark out of memory.
				if (!names) {
					host->_error.error = ENOMEM;
					host->_error.domain = kCFStreamErrorDomainPOSIX;
				}

				// Save the list of names on the host.
				else {
					CFDictionaryAddValue(host->_info, (const void*)kCFHostNames, names);
					CFRelease(names);
				}
			}
		}

		// Save the callback if there is one at this time.
		cb = host->_callback;

		// Save the error and client information for the callback
		memmove(&error, &(host->_error), sizeof(error));
		info = host->_client.info;

		// Remove the lookup from run loops and modes
		_CFTypeUnscheduleFromMultipleRunLoops(host->_lookup, host->_schedules);

		// Go ahead and invalidate the lookup
		CFMachPortInvalidate((CFMachPortRef)(host->_lookup));

		// Release the lookup now.
		CFRelease(host->_lookup);
		host->_lookup = NULL;
		host->_type = _kCFNullHostInfoType;
	}

	// Unlock the host so the callback can be made safely.
	__CFSpinUnlock(&host->_lock);

	// Release the results if there were any.
    if (freenameinfo_cb) {
        freenameinfo_cb(hostname, serv);
    }

	// If there is a callback, inform the client of the finish.
	if (cb)
		cb((CFHostRef)host, kCFHostNames, &error, info);

	// Go ahead and release now that the callback is done.
	CFRelease((CFHostRef)host);
}

/* static */ void
_FreeNameInfoCallBack_Mach(char *hostname, char *serv) {
    if (hostname) free(hostname);
    if (serv) free (serv);
}

/* static */ void
_GetNameInfoCallBack(int eai_status, char *hostname, char *serv, void* ctxt) {
    _GetNameInfoCallBackWithFree(eai_status, hostname, serv, ctxt, _FreeNameInfoCallBack_Mach, TRUE);
}

/* static */ void
_GetNameInfoMachPortCallBack(CFMachPortRef port, void* msg, CFIndex size, void* info) {

	getnameinfo_async_handle_reply(msg);
}

/* static */ void
_NetworkReachabilityCallBack(SCNetworkReachabilityRef target, SCNetworkConnectionFlags flags, void* ctxt) {

	_CFHost* host = (_CFHost*)ctxt;
	CFHostClientCallBack cb = NULL;
	CFStreamError error;
	void* info = NULL;

	// Retain here to guarantee safety really after the lookups release,
	// but definitely before the callback.
	CFRetain((CFHostRef)host);

	// Lock the host
	__CFSpinLock(&host->_lock);

	// If the lookup canceled, don't need to do any of this.
	if (host->_lookup) {

		// Create the data for hanging off the host info dictionary
		CFDataRef reachability = CFDataCreate(CFGetAllocator(target), (const UInt8*)&flags, sizeof(flags));

		// Make sure to toss the cached info now.
		CFDictionaryRemoveValue(host->_info, (const void*)kCFHostReachability);

		// If didn't create the data, fail with out of memory.
		if (!reachability) {
			host->_error.error = ENOMEM;
			host->_error.domain = kCFStreamErrorDomainPOSIX;
		}

		else {
			// Save the reachability information
			CFDictionaryAddValue(host->_info, (const void*)kCFHostReachability, reachability);
			CFRelease(reachability);
		}

		// Save the callback if there is one at this time.
		cb = host->_callback;

		// Save the error and client information for the callback
		memmove(&error, &(host->_error), sizeof(error));
		info = host->_client.info;

		// Remove the lookup from run loops and modes
		_CFTypeUnscheduleFromMultipleRunLoops(host->_lookup, host->_schedules);

		// "Invalidate" the reachability object by removing the client
		SCNetworkReachabilitySetCallback((SCNetworkReachabilityRef)(host->_lookup), NULL, NULL);

		// Release the lookup now.
		CFRelease(host->_lookup);
		host->_lookup = NULL;
		host->_type = _kCFNullHostInfoType;
	}

	// Unlock the host so the callback can be made safely.
	__CFSpinUnlock(&host->_lock);

	// If there is a callback, inform the client of the finish.
	if (cb)
		cb((CFHostRef)host, kCFHostReachability, &error, info);

	// Go ahead and release now that the callback is done.
	CFRelease((CFHostRef)host);
}
#endif /* #if defined(__MACH__) */

#if defined(__MACH__)
/* static */ void
_NetworkReachabilityByIPCallBack(_CFHost* host) {

	CFHostClientCallBack cb = NULL;
	CFStreamError error;
	void* info = NULL;

	// Retain here to guarantee safety really after the lookups release,
	// but definitely before the callback.
	CFRetain((CFHostRef)host);

	// Lock the host
	__CFSpinLock(&host->_lock);

	// If the lookup canceled, don't need to do any of this.
	if (host->_lookup) {

		// Save the callback if there is one at this time.
		cb = host->_callback;

		// Save the error and client information for the callback
		memmove(&error, &(host->_error), sizeof(error));
		info = host->_client.info;

		// Remove the lookup from run loops and modes
		_CFTypeUnscheduleFromMultipleRunLoops(host->_lookup, host->_schedules);

		// Invalidate the run loop source that got here
		CFRunLoopSourceInvalidate((CFRunLoopSourceRef)(host->_lookup));

		// Release the lookup now.
		CFRelease(host->_lookup);
		host->_lookup = NULL;
		host->_type = _kCFNullHostInfoType;
	}

	// Unlock the host so the callback can be made safely.
	__CFSpinUnlock(&host->_lock);

	// If there is a callback, inform the client of the finish.
	if (cb)
		cb((CFHostRef)host, kCFHostReachability, &error, info);

	// Go ahead and release now that the callback is done.
	CFRelease((CFHostRef)host);
}
#endif /* defined(__MACH__) */

#if defined(__MACH__)
/* static */ void
_DNSCallBack(int32_t status, char *buf, uint32_t len, struct sockaddr *from, int fromlen, void *context) {

	_CFHost* host = (_CFHost*)context;
	CFHostClientCallBack cb = NULL;
	CFStreamError error;
	void* info = NULL;
	CFHostInfoType type = _kCFNullHostInfoType;

	// Retain here to guarantee safety really after the lookups release,
	// but definitely before the callback.
	CFRetain((CFHostRef)context);

	// Lock the host
	__CFSpinLock(&host->_lock);

	// If the lookup canceled, don't need to do any of this.
	if (host->_lookup) {

		// Make sure to toss the cached info now.
		CFDictionaryRemoveValue(host->_info, (const void*)(host->_type));

		// Set the error if got one back from the lookup
		if (status) {
			_HandleGetAddrInfoStatus(status, &host->_error, FALSE);

			// Mark to indicate the resolution was performed.
			CFDictionaryAddValue(host->_info, (const void*)(host->_type), kCFNull);
		}

		else {
			CFAllocatorRef allocator = CFGetAllocator((CFHostRef)context);

			// Wrap the reply and the source of the reply
			CFDataRef rr = CFDataCreate(allocator, (const UInt8*)buf, len);
			CFDataRef sa = CFDataCreate(allocator, (const UInt8*)from, fromlen);

			// If couldn't wrap, fail with no memory error.
			if (!rr || !sa) {
				host->_error.error = ENOMEM;
				host->_error.domain = kCFStreamErrorDomainPOSIX;
			}

			else {

				// Create the information to put in the info dictionary.
				CFTypeRef list[2] = {rr, sa};
				CFArrayRef array = CFArrayCreate(allocator, list, sizeof(list) / sizeof(list[0]), &kCFTypeArrayCallBacks);

				// Make sure it was created and add it.
				if (array) {
					CFDictionaryAddValue(host->_info, (const void*)(host->_type), array);
					CFRelease(array);
				}

				// Did make the information list so fail with out of memory
				else {
					host->_error.error = ENOMEM;
					host->_error.domain = kCFStreamErrorDomainPOSIX;
				}
			}

			// Release the reply if it was created.
			if (rr)
				CFRelease(rr);

			// Release the sockaddr wrapper if it was created
			if (sa)
				CFRelease(sa);
		}

		// Save the callback if there is one at this time.
		cb = host->_callback;

		// Save the type of lookup for the callback.
		type = host->_type;

		// Save the error and client information for the callback
		memmove(&error, &(host->_error), sizeof(error));
		info = host->_client.info;

		// Remove the lookup from run loops and modes
		_CFTypeUnscheduleFromMultipleRunLoops(host->_lookup, host->_schedules);

		// Go ahead and invalidate the lookup
		CFMachPortInvalidate((CFMachPortRef)(host->_lookup));

		// Release the lookup now.
		CFRelease(host->_lookup);
		host->_lookup = NULL;
		host->_type = _kCFNullHostInfoType;
	}

	// Unlock the host so the callback can be made safely.
	__CFSpinUnlock(&host->_lock);

	// If there is a callback, inform the client of the finish.
	if (cb)
		cb((CFHostRef)context, type, &error, info);

	// Go ahead and release now that the callback is done.
	CFRelease((CFHostRef)context);
}

/* static */ void
_DNSMachPortCallBack(CFMachPortRef port, void* msg, CFIndex size, void* info) {

	dns_async_handle_reply(msg);
}
#endif /* #if defined(__MACH__) */

/* static */ void
_MasterCallBack(CFHostRef theHost, CFHostInfoType typeInfo, const CFStreamError *error, CFStringRef name) {

	CFArrayRef list;

	/* Shut down the host lookup. */
	CFHostSetClient(theHost, NULL, NULL);

	/* Lock the host master list and cache */
	_CFMutexLock(_HostLock);

	/* Get the list of clients. */
	list = CFDictionaryGetValue(_HostLookups, name);

	if (list) {

		CFRetain(list);

		/* Remove the entry from the list of master lookups. */
		CFDictionaryRemoveValue(_HostLookups, name);
	}

	_CFMutexUnlock(_HostLock);

	if (list) {

		CFIndex i, count;
		CFArrayRef addrs = CFHostGetInfo(theHost, _kCFHostMasterAddressLookup, NULL);

		/* If no error, add the host to the cache. */
		if (!error->error) {

			/* The host will be saved for each name in the list of names for the host. */
			CFArrayRef names = CFHostGetInfo(theHost, kCFHostNames, NULL);

			if (names && ((CFTypeRef)names != kCFNull)) {

				/* Each host cache entry is a host with its fetch time. */
				CFTypeRef orig[2] = {theHost, CFDateCreate(kCFAllocatorDefault, CFAbsoluteTimeGetCurrent())};

				/* Only add the entries if the date was created. */
				if (orig[1]) {

					/* Create the CFArray to be added into the cache. */
					CFArrayRef items = CFArrayCreate(kCFAllocatorDefault, orig, sizeof(orig) / sizeof(orig[0]), &kCFTypeArrayCallBacks);

					CFRelease(orig[1]);

					/* Once again, only add if the list was created. */
					if (items) {

						/* Loop through all the names of the host. */
						count = CFArrayGetCount(names);

						/* Add an entry for each name. */
						for (i = 0; i < count; i++)
							CFDictionaryAddValue(_HostCache, CFArrayGetValueAtIndex(names, i), items);

						CFRelease(items);
					}
				}
			}
		}

		count = CFArrayGetCount(list);

		for (i = 1; i < count; i++) {

			_CFHost* client;
			CFRunLoopSourceContext ctxt = {0};
			CFRunLoopSourceRef src = (CFRunLoopSourceRef)CFArrayGetValueAtIndex(list, i);

			CFRunLoopSourceGetContext(src, &ctxt);
			client = (_CFHost*)ctxt.info;

			__CFSpinLock(&client->_lock);

			/* Make sure to toss the cached info now. */
			CFDictionaryRemoveValue(client->_info, (const void*)(client->_type));

			/* Deal with the error if there was one. */
			if (error->error) {

				/* Copy the error over to the client. */
				memmove(&client->_error, error, sizeof(error[0]));

				/* Mark to indicate the resolution was performed. */
				CFDictionaryAddValue(client->_info, (const void*)(client->_type), kCFNull);
			}

			else {

				/* Make a copy of the addresses with the client's allocator. */
				CFArrayRef cp = _CFArrayCreateDeepCopy(CFGetAllocator((CFHostRef)client), addrs);

				if (cp) {

					CFDictionaryAddValue(client->_info, (const void*)(client->_type), addrs);

					CFRelease(cp);
				}

				else {

					/* Make sure to error if couldn't create the list. */
					client->_error.error = ENOMEM;
					client->_error.domain = kCFStreamErrorDomainPOSIX;

					/* Mark to indicate the resolution was performed. */
					CFDictionaryAddValue(client->_info, (const void*)(client->_type), kCFNull);
				}
			}

			/* Signal the client for immediate attention. */
			CFRunLoopSourceSignal((CFRunLoopSourceRef)(client->_lookup));

			CFArrayRef schedules = client->_schedules;
			CFIndex j, c = CFArrayGetCount(schedules);

			/* Make sure the signal can make it through */
			for (j = 0; j < c; j += 2) {

				/* Grab the run loop for checking */
				CFRunLoopRef runloop = (CFRunLoopRef)CFArrayGetValueAtIndex(schedules, j);

				/* If it's sleeping, need to further check it. */
				if (CFRunLoopIsWaiting(runloop)) {

					/* Grab the mode for further check */
					CFStringRef mode = CFRunLoopCopyCurrentMode(runloop);

					if (mode) {

						/* If the lookup is in the right mode, need to wake up the run loop. */
						if (CFRunLoopContainsSource(runloop, (CFRunLoopSourceRef)(client->_lookup), mode)) {
							CFRunLoopWakeUp(runloop);
						}

						/* Don't need this anymore. */
						CFRelease(mode);
					}
				}
			}

			__CFSpinUnlock(&client->_lock);
		}

		CFRelease(list);
	}
}


/* static */ void
_AddressLookupSchedule_NoLock(_CFHost* host, CFRunLoopRef rl, CFStringRef mode) {

	CFArrayRef list;
	CFArrayRef names = (CFArrayRef)CFDictionaryGetValue(host->_info, (const void*)kCFHostNames);
	CFStringRef name = (CFStringRef)CFArrayGetValueAtIndex(names, 0);

	/* Lock the list of master lookups and cache */
	_CFMutexLock(_HostLock);

	list = CFDictionaryGetValue(_HostLookups, name);

	if (list) {
		CFHostScheduleWithRunLoop((CFHostRef)CFArrayGetValueAtIndex(list, 0), rl, mode);
	}

	_CFMutexUnlock(_HostLock);
}


/* static */ void
_AddressLookupPerform(_CFHost* host) {

	CFHostClientCallBack cb = NULL;
	CFStreamError error;
	void* info = NULL;

	// Retain here to guarantee safety really after the lookups release,
	// but definitely before the callback.
	CFRetain((CFHostRef)host);

	// Lock the host
	__CFSpinLock(&host->_lock);

	// Save the callback if there is one at this time.
	cb = host->_callback;

	// Save the error and client information for the callback
	memmove(&error, &(host->_error), sizeof(error));
	info = host->_client.info;

	// Remove the lookup from run loops and modes
	_CFTypeUnscheduleFromMultipleRunLoops(host->_lookup, host->_schedules);

	// Go ahead and invalidate the lookup
	CFRunLoopSourceInvalidate((CFRunLoopSourceRef)(host->_lookup));

	// Release the lookup now.
	CFRelease(host->_lookup);
	host->_lookup = NULL;
	host->_type = _kCFNullHostInfoType;

	// Unlock the host so the callback can be made safely.
	__CFSpinUnlock(&host->_lock);

	// If there is a callback, inform the client of the finish.
	if (cb)
		cb((CFHostRef)host, kCFHostAddresses, &error, info);

	// Go ahead and release now that the callback is done.
	CFRelease((CFHostRef)host);
}


/* static */ void
_ExpireCacheEntries(void) {

	CFIndex count;

	CFStringRef keys_buffer[_kCFHostCacheMaxEntries];
	CFArrayRef values_buffer[_kCFHostCacheMaxEntries];

	CFStringRef* keys = &keys_buffer[0];
	CFArrayRef* values = &values_buffer[0];

	/* Lock the cache */
	_CFMutexLock(_HostLock);

	if (_HostCache) {

		/* Get the count for proper allocation if needed and for iteration. */
		count = CFDictionaryGetCount(_HostCache);

		/* Allocate buffers for keys and values if don't have large enough static buffers. */
		if (count > _kCFHostCacheMaxEntries) {

			keys = (CFStringRef*)CFAllocatorAllocate(kCFAllocatorDefault, sizeof(keys[0]) * count, 0);
			values = (CFArrayRef*)CFAllocatorAllocate(kCFAllocatorDefault, sizeof(values[0]) * count, 0);
		}

		/* Only iterate if buffers were allocated. */
		if (keys && values) {

			CFIndex i, j = 0;
			CFTimeInterval oldest = 0.0;

			/* Get "now" for comparison for freshness. */
			CFDateRef now = CFDateCreate(kCFAllocatorDefault, CFAbsoluteTimeGetCurrent());

			/* Get all the hosts in the cache */
			CFDictionaryGetKeysAndValues(_HostCache, (const void **)keys, (const void **)values);

			/* Iterate through and get rid of expired ones. */
			for (i = 0; i < count; i++) {

				/* How long since now?  Use abs in order to handle clock changes. */
				CFTimeInterval since = fabs(CFDateGetTimeIntervalSinceDate(now, (CFDateRef)CFArrayGetValueAtIndex(values[i], 1)));

				/* If timeout, remove the entry. */
				if (since >= _kCFHostCacheTimeout)
					CFDictionaryRemoveValue(_HostCache, keys[i]);

				/* If this one is older than the oldest, save it's index. */
				else if (since > oldest) {
					j = i;
					oldest = since;
				}
			}

			CFRelease(now);

			/* If the count still isn't in the bounds of maximum number of entries, remove the oldest. */
			if (CFDictionaryGetCount(_HostCache) >= _kCFHostCacheMaxEntries)
				CFDictionaryRemoveValue(_HostCache, keys[j]);
		}

		/* If space for keys was made, deallocate it. */
		if (keys && (keys != &keys_buffer[0]))
			CFAllocatorDeallocate(kCFAllocatorDefault, keys);

		/* If space for values was made, deallocate it. */
		if (values && (values != &values_buffer[0]))
			CFAllocatorDeallocate(kCFAllocatorDefault, values);
	}

	_CFMutexUnlock(_HostLock);
}


/* static */ CFArrayRef
_CFArrayCreateDeepCopy(CFAllocatorRef alloc, CFArrayRef array) {

    CFArrayRef result = NULL;
    CFIndex i, c = CFArrayGetCount(array);
    CFTypeRef *values;
    if (c == 0) {
        result = CFArrayCreate(alloc, NULL, 0, &kCFTypeArrayCallBacks);
    } else if ((values = (CFTypeRef*)CFAllocatorAllocate(alloc, c*sizeof(CFTypeRef), 0)) != NULL) {
        CFArrayGetValues(array, CFRangeMake(0, c), values);
        if (CFGetTypeID(values[0]) == CFStringGetTypeID()) {
            for (i = 0; i < c; i ++) {
                values[i] = CFStringCreateCopy(alloc, (CFStringRef)values[i]);
                if (values[i] == NULL) {
                    break;
                }
            }
        }
        else if (CFGetTypeID(values[0]) == CFDataGetTypeID()) {
            for (i = 0; i < c; i ++) {
                values[i] = CFDataCreateCopy(alloc, (CFDataRef)values[i]);
                if (values[i] == NULL) {
                    break;
                }
            }
        }
        else {
            for (i = 0; i < c; i ++) {
                values[i] = CFPropertyListCreateDeepCopy(alloc, values[i], kCFPropertyListImmutable);
                if (values[i] == NULL) {
                    break;
                }
            }
        }

        result = (i == c) ? CFArrayCreate(alloc, values, c, &kCFTypeArrayCallBacks) : NULL;
        c = i;
        for (i = 0; i < c; i ++) {
            CFRelease(values[i]);
        }
        CFAllocatorDeallocate(alloc, values);
    }
    return result;
}

#if defined(__MACH__)
/* static */ Boolean
_IsDottedIp(CFStringRef name) {

	Boolean result = FALSE;
	UInt8 stack_buffer[1024];
	UInt8* buffer = stack_buffer;
	CFIndex length = sizeof(stack_buffer);
	CFAllocatorRef alloc = CFGetAllocator(name);

	buffer = _CFStringGetOrCreateCString(alloc, name, buffer, &length, kCFStringEncodingASCII);

	if (buffer) {

		struct addrinfo hints;
		struct addrinfo* results = NULL;

		memset(&hints, 0, sizeof(hints));
		hints.ai_flags = AI_NUMERICHOST;

		if (!getaddrinfo((const char*)buffer, NULL, &hints, &results)) {

			if (results) {

				if (results->ai_addr)
					result = TRUE;

				freeaddrinfo(results);
			}
		}
	}

	if (buffer != stack_buffer)
		CFAllocatorDeallocate(alloc, buffer);

	return result;
}
#endif /* defined(__MACH__) */

#if 0
#pragma mark -
#pragma mark Extern Function Definitions (API)
#endif

/* extern */ CFTypeID
CFHostGetTypeID(void) {

    _CFDoOnce(&_kCFHostRegisterClass, _CFHostRegisterClass);
#if defined(__linux__)
    _CFDoOnce(&_kCFHostInitializeAres, _CFHostInitializeAres);
#endif /* defined(__linux__) */

    return _kCFHostTypeID;
}


/* extern */ CFHostRef
CFHostCreateWithName(CFAllocatorRef allocator, CFStringRef hostname) {

	// Create the base object
	_CFHost* result = _HostCreate(allocator);

	// Set the names only if succeeded
	if (result) {

		// Create the list of names
		CFArrayRef names = CFArrayCreate(allocator, (const void**)(&hostname), 1, &kCFTypeArrayCallBacks);

		// Add the list to the info if it succeeded
		if (names) {
			CFDictionaryAddValue(result->_info, (const void*)kCFHostNames, names);
			CFRelease(names);
		}

		// Failed so release the new host and return null
		else {
			CFRelease((CFTypeRef)result);
			result = NULL;
		}
	}

	return (CFHostRef)result;
}


/* extern */ CFHostRef
CFHostCreateWithAddress(CFAllocatorRef allocator, CFDataRef addr) {

	// Create the base object
	_CFHost* result = _HostCreate(allocator);

	// Set the names only if succeeded
	if (result) {

		// Create the list of addresses
		CFArrayRef addrs = CFArrayCreate(allocator, (const void**)(&addr), 1, &kCFTypeArrayCallBacks);

		// Add the list to the info if it succeeded
		if (addrs) {
			CFDictionaryAddValue(result->_info, (const void*)kCFHostAddresses, addrs);
			CFRelease(addrs);
		}

		// Failed so release the new host and return null
		else {
			CFRelease((CFTypeRef)result);
			result = NULL;
		}
	}

	return (CFHostRef)result;
}


/* extern */ CFHostRef
CFHostCreateCopy(CFAllocatorRef allocator, CFHostRef h) {

	_CFHost* host = (_CFHost*)h;

	// Create the base object
	_CFHost* result = _HostCreate(allocator);

	// Set the names only if succeeded
	if (result) {

		// Release the current, because a new one will be laid down
		CFRelease(result->_info);

		// Lock original before going to town on it
		__CFSpinLock(&(host->_lock));

		// Just make a copy of all the information
		result->_info = CFDictionaryCreateMutableCopy(allocator, 0, host->_info);

		// Let the original go
		__CFSpinUnlock(&(host->_lock));

		// If it failed, release the new host and return null
		if (!result->_info) {
			CFRelease((CFTypeRef)result);
			result = NULL;
		}
	}

	return (CFHostRef)result;
}

/**
 *  @brief
 *    Starts resolution for a host object.
 *
 *  This function retrieves the information specified by @a info and
 *  stores it in the host.
 *
 * In synchronous mode, this function blocks until the resolution has
 * completed, in which case this function returns TRUE, until the
 * resolution is stopped by calling #CFHostCancelInfoResolution from
 * another thread, in which case this function returns FALSE, or until
 * an error occurs.
 *
 *  @param[in]      theHost  The host, obtained by previously calling
 *                           #CFHostCreateCopy,
 *                           #CFHostCreateWithAddress, or
 *                           #CFHostCreateWithName, that is to be
 *                           resolved. This value must not be NULL.
 *  @param[in]      info     A value of type CFHostInfoType
 *                           specifying the type of information that
 *                           is to be retrieved. See #CFHostInfoType
 *                           for possible values.
 *  @param[in,out]  error    A pointer to a #CFStreamError structure,
 *                           that if an error occurs, is set to the
 *                           error and the error's domain. In
 *                           synchronous mode, the error indicates why
 *                           resolution failed, and in asynchronous
 *                           mode, the error indicates why resolution
 *                           failed to start.
 *
 *  @returns
 *    TRUE if the resolution was started (asynchronous mode); FALSE if
 *    another resolution is already in progress for @a theHost or if an
 *    error occurred.
 *
 */
/* extern */ Boolean
CFHostStartInfoResolution(CFHostRef theHost, CFHostInfoType info, CFStreamError* error) {

	_CFHost* host = (_CFHost*)theHost;
	CFStreamError extra;
	Boolean result = FALSE;

	if (!error)
		error = &extra;

	memset(error, 0, sizeof(error[0]));

	// Retain so it doesn't go away underneath in the case of a callout.  This is really
	// no worry for async, but makes the memmove for the error more difficult to place
	// for synchronous without it being here.
	CFRetain(theHost);

	// Lock down the host to grab the info
	__CFSpinLock(&(host->_lock));

	do {

		Boolean wakeup = FALSE;

		// Create lookup.  Bail if it fails.
		if (!_CreateLookup_NoLock(host, info, &wakeup))
			break;

		// Async mode is complete at this point
		if (host->_callback) {

			// Schedule the lookup on the run loops and modes.
			_CFTypeScheduleOnMultipleRunLoops(host->_lookup, host->_schedules);

			// 4012176 If the source was signaled, wake up the run loop.
			if (wakeup) {

				CFArrayRef schedules = host->_schedules;
				CFIndex i, count = CFArrayGetCount(schedules);

				// Make sure the signal can make it through
				for (i = 0; i < count; i += 2) {

					// Wake up run loop
					CFRunLoopWakeUp((CFRunLoopRef)CFArrayGetValueAtIndex(schedules, i));
				}
			}

			// It's now succeeded.
			result = TRUE;
		}

		// If there is no callback, go into synchronous mode.
		else {

			// Unlock the host
			__CFSpinUnlock(&(host->_lock));

			// Wait for synchronous return
			result = _HostBlockUntilComplete(host);

			// Lock down the host to grab the info
			__CFSpinLock(&(host->_lock));
		}

	} while (0);

	// Copy the error.
	memmove(error, &host->_error, sizeof(error[0]));

	// Unlock the host
	__CFSpinUnlock(&(host->_lock));

	// Release the earlier retain.
	CFRelease(theHost);

	return result;
}


/* extern */ CFTypeRef
CFHostGetInfo(CFHostRef theHost, CFHostInfoType info, Boolean* hasBeenResolved) {

	_CFHost* host = (_CFHost*)theHost;
	Boolean extra;
	CFTypeRef result = NULL;

	// Just make sure there is something to dereference.
	if (!hasBeenResolved)
		hasBeenResolved = &extra;

	// By default, it hasn't been resolved.
	*hasBeenResolved = FALSE;

	// Lock down the host to grab the info
	__CFSpinLock(&(host->_lock));

	// Grab the requested information
	result = (CFTypeRef)CFDictionaryGetValue(host->_info, (const void*)info);

	// If there was a result, mark it as being resolved.
	if (result) {

		// If it was NULL, that means resolution actually returned nothing.
		if (CFEqual(result, kCFNull))
			result = NULL;

		// It's been resolved.
		*hasBeenResolved = TRUE;
	}

	// Unlock the host
	__CFSpinUnlock(&(host->_lock));

	return result;
}


/* extern */ CFArrayRef
CFHostGetAddressing(CFHostRef theHost, Boolean* hasBeenResolved) {

	return (CFArrayRef)CFHostGetInfo(theHost, kCFHostAddresses, hasBeenResolved);
}


/* extern */ CFArrayRef
CFHostGetNames(CFHostRef theHost, Boolean* hasBeenResolved) {

	return (CFArrayRef)CFHostGetInfo(theHost, kCFHostNames, hasBeenResolved);
}


#if defined(__MACH__)
/* extern */ CFDataRef
CFHostGetReachability(CFHostRef theHost, Boolean* hasBeenResolved) {

	return (CFDataRef)CFHostGetInfo(theHost, kCFHostReachability, hasBeenResolved);
}
#endif


/* extern */ void
CFHostCancelInfoResolution(CFHostRef theHost, CFHostInfoType info) {

	_CFHost* host = (_CFHost*)theHost;

	// Lock down the host
	__CFSpinLock(&(host->_lock));

	// Make sure there is something to cancel.
	if (host->_lookup) {

		CFRunLoopSourceContext ctxt = {
			0,								// version
			NULL,							// info
			NULL,							// retain
			NULL,							// release
			NULL,							// copyDescription
			NULL,							// equal
			NULL,							// hash
			NULL,							// schedule
			NULL,							// cancel
			(void(*)(void*))(&_HostCancel)  // perform
		};

		// Remove the lookup from run loops and modes
		_CFTypeUnscheduleFromMultipleRunLoops(host->_lookup, host->_schedules);

		// Go ahead and invalidate the lookup
		_CFTypeInvalidate(host->_lookup);

		// Pull the lookup out of the list in the master list.
		if (host->_type == kCFHostAddresses) {

			CFMutableArrayRef list;
			CFArrayRef names = (CFArrayRef)CFDictionaryGetValue(host->_info, (const void*)kCFHostNames);
			CFStringRef name = (CFStringRef)CFArrayGetValueAtIndex(names, 0);

			/* Lock the master lookup list and cache */
			_CFMutexLock(_HostLock);

			/* Get the list of pending clients */
			list = (CFMutableArrayRef)CFDictionaryGetValue(_HostLookups, name);

			if (list) {

				/* Try to find this lookup in the list of clients. */
				CFIndex count = CFArrayGetCount(list);
				CFIndex idx = CFArrayGetFirstIndexOfValue(list, CFRangeMake(0, count), host->_lookup);

				if (idx != kCFNotFound) {

					/* Remove this lookup. */
					CFArrayRemoveValueAtIndex(list, idx);

					/* If this was the last client, kill the lookup. */
					if (count == 2) {

						CFHostRef lookup = (CFHostRef)CFArrayGetValueAtIndex(list, 0);

						/* NULL the client for the master lookup and cancel it. */
						CFHostSetClient(lookup, NULL, NULL);
						CFHostCancelInfoResolution(lookup, _kCFHostMasterAddressLookup);

						/* Remove it from the list of pending lookups and clients. */
						CFDictionaryRemoveValue(_HostLookups, name);
					}
				}
			}

			_CFMutexUnlock(_HostLock);
		}

		// Release the lookup now.
		CFRelease(host->_lookup);

		// Create the cancel source
		host->_lookup = CFRunLoopSourceCreate(CFGetAllocator(theHost), 0, &ctxt);

		// If the cancel was created, need to schedule and signal it.
		if (host->_lookup) {

			CFArrayRef schedules = host->_schedules;
			CFIndex i, count = CFArrayGetCount(schedules);

			// Schedule the new lookup
			_CFTypeScheduleOnMultipleRunLoops(host->_lookup, schedules);

			// Signal the cancel for immediate attention.
			CFRunLoopSourceSignal((CFRunLoopSourceRef)(host->_lookup));

			// Make sure the signal can make it through
			for (i = 0; i < count; i += 2) {

				// Grab the run loop for checking
				CFRunLoopRef runloop = (CFRunLoopRef)CFArrayGetValueAtIndex(schedules, i);

				// If it's sleeping, need to further check it.
				if (CFRunLoopIsWaiting(runloop)) {

					// Grab the mode for further check
					CFStringRef mode = CFRunLoopCopyCurrentMode(runloop);

					if (mode) {

						// If the lookup is in the right mode, need to wake up the run loop.
						if (CFRunLoopContainsSource(runloop, (CFRunLoopSourceRef)(host->_lookup), mode)) {
							CFRunLoopWakeUp(runloop);
						}

						// Don't need this anymore.
						CFRelease(mode);
					}
				}
			}
		}
	}

	// Unlock the host
	__CFSpinUnlock(&(host->_lock));
}


/* extern */ Boolean
CFHostSetClient(CFHostRef theHost, CFHostClientCallBack clientCB, CFHostClientContext* clientContext) {

	_CFHost* host = (_CFHost*)theHost;

	// Lock down the host
	__CFSpinLock(&(host->_lock));

	// Release the user's context info if there is some and a release method
	if (host->_client.info && host->_client.release)
		host->_client.release(host->_client.info);

	// NULL callback or context signals to remove the client
	if (!clientCB || !clientContext) {

		// Cancel the outstanding lookup
		if (host->_lookup) {

			// Remove the lookup from run loops and modes
			_CFTypeUnscheduleFromMultipleRunLoops(host->_lookup, host->_schedules);

			// Go ahead and invalidate the lookup
			_CFTypeInvalidate(host->_lookup);

			// Pull the lookup out of the master lookups.
			if (host->_type == kCFHostAddresses) {

				CFMutableArrayRef list;
				CFArrayRef names = (CFArrayRef)CFDictionaryGetValue(host->_info, (const void*)kCFHostNames);
				CFStringRef name = (CFStringRef)CFArrayGetValueAtIndex(names, 0);

				/* Lock the masters list and cache */
				_CFMutexLock(_HostLock);

				/* Get the list of pending clients */
				list = (CFMutableArrayRef)CFDictionaryGetValue(_HostLookups, name);

				if (list) {

					/* Try to find this lookup in the list of clients. */
					CFIndex count = CFArrayGetCount(list);
					CFIndex idx = CFArrayGetFirstIndexOfValue(list, CFRangeMake(0, count), host->_lookup);

					if (idx != kCFNotFound) {

						/* Remove this lookup. */
						CFArrayRemoveValueAtIndex(list, idx);

						/* If this was the last client, kill the lookup. */
						if (count == 2) {

							CFHostRef lookup = (CFHostRef)CFArrayGetValueAtIndex(list, 0);

							/* NULL the client for the master lookup and cancel it. */
							CFHostSetClient(lookup, NULL, NULL);
							CFHostCancelInfoResolution(lookup, _kCFHostMasterAddressLookup);

							/* Remove it from the list of pending lookups and clients. */
							CFDictionaryRemoveValue(_HostLookups, name);
						}
					}
				}

				_CFMutexUnlock(_HostLock);
			}

			// Release the lookup now.
			CFRelease(host->_lookup);
			host->_lookup = NULL;
			host->_type = _kCFNullHostInfoType;
		}

		// Zero out the callback and client context.
		host->_callback = NULL;
		memset(&(host->_client), 0, sizeof(host->_client));
	}

	//
	else {

		// Schedule any lookup on the run loops and modes if it hasn't been scheduled
		// already.  If there had previously been a callback, the lookup will have
		// already been scheduled.
		if (!host->_callback && host->_lookup)
			_CFTypeScheduleOnMultipleRunLoops(host->_lookup, host->_schedules);

		// Save the client's new callback
		host->_callback = clientCB;

		// Copy the client's context
		memmove(&(host->_client), clientContext, sizeof(host->_client));

		// If there is user data and a retain method, call it.
		if (host->_client.info && host->_client.retain)
			host->_client.info = (void*)(host->_client.retain(host->_client.info));
	}

	// Unlock the host
	__CFSpinUnlock(&(host->_lock));

	return TRUE;
}


/* extern */ void
CFHostScheduleWithRunLoop(CFHostRef theHost, CFRunLoopRef runLoop, CFStringRef runLoopMode) {

	_CFHost* host = (_CFHost*)theHost;

	/* Lock down the host before work */
	__CFSpinLock(&(host->_lock));

	/* Try adding the schedule to the list.  If it's added, need to do more work. */
	if (_SchedulesAddRunLoopAndMode(host->_schedules, runLoop, runLoopMode)) {

		/* If there is a current lookup, need to schedule it. */
		if (host->_lookup) {
			_CFTypeScheduleOnRunLoop(host->_lookup, runLoop, runLoopMode);
		}
	}

	/* Unlock the host */
	__CFSpinUnlock(&(host->_lock));
}


/* extern */ void
CFHostUnscheduleFromRunLoop(CFHostRef theHost, CFRunLoopRef runLoop, CFStringRef runLoopMode) {

	_CFHost* host = (_CFHost*)theHost;

	/* Lock down the host before work */
	__CFSpinLock(&(host->_lock));

	/* Try to remove the schedule from the list.  If it is removed, need to do more. */
	if (_SchedulesRemoveRunLoopAndMode(host->_schedules, runLoop, runLoopMode)) {

		/* If there is a current lookup, need to unschedule it. */
		if (host->_lookup) {
			_CFTypeUnscheduleFromRunLoop(host->_lookup, runLoop, runLoopMode);
		}
	}

	/* Unlock the host */
	__CFSpinUnlock(&(host->_lock));
}

