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
#define LOG_CFHOST 0
#endif

#if LOG_CFHOST
#define __CFHostMaybeLog(format, ...)  do { fprintf(stderr, format, ##__VA_ARGS__); fflush(stderr); } while (0)
#else
#define __CFHostMaybeLog(format, ...)
#endif

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
#if (HAVE_GETADDRINFO_A && 0)
/**
 *  @brief
 *    The active heap-based object used to manage forward DNS look-ups
 *    with Linux, glibc, and getaddrinfo_a.
 *
 *  This represents all of the active state needed to manage
 *  outstanding forward DNS look-ups using Linux, glibc, and
 *  getaddrinfo_a.
 *
 *  @note
 *    Since there is no equivalent getnameinfo_a in Linux with glibc,
 *    this portability approach for CFHost on Linux is, for now, a
 *    dead end and undesirable.
 *
 */
typedef struct {
	struct gaicb    _request_gaicb;
	struct addrinfo _request_hints;
	struct gaicb *  _request_list[1];
} _CFHostGAIARequest;
#endif /* (HAVE_GETADDRINFO_A && 0) */

#if (HAVE_ARES_INIT && 1)
/**
 *  @brief
 *    The active heap-based object used to manage forward- and
 *    reverse- DNS look-ups with c-ares.
 *
 *  This represents all of the active state needed to manage
 *  outstanding forward- and reverse DNS look-ups using c-ares.
 *
 *  @note
 *    c-ares supports the notion of an effective timeout for lookup
 *    channel, via 'ares_timeout'. However, the CFileDescriptor
 *    object, while simpler for this application, does not effecitvely
 *    support a timeout for watched descriptors and implies a higher
 *    poll/select rate for a pending request than were a timeout
 *    supported. If timeout behavior is desired, CFFileDescriptor
 *    could be unilaterally changed with CFSocket.
 *
 */
typedef struct {
    ares_channel        _request_channel;  //!< The c-ares name service
                                           //!< channel used to initiate
                                           //!< requests and receive responses.
    size_t              _request_pending;  //!< The number of channel requests
    const char *        _request_name;     //!< The lookup name for forward
                                           //!< DNS (that is, name-to-address)
                                           //!< requests.
    CFFileDescriptorRef _request_lookup;   //!< The run loop schedulable
                                           //!< object that will be poll/
                                           //!< select'd for request/response
                                           //!< activity.
    CFHostInfoType      _request_type;     //!< The type of data that is to be
                                           //!< resolved for the resolution
                                           //!< request.
    uint16_t            _request_events;   //!< The poll/select events
                                           //!< currently desired for
                                           //!< _request_lookup.
    CFStreamError *     _request_error;    //!< A pointer to the stream error
                                           //!< for the most recent request.
    int                 _request_status;   //!< The stream status for the most
                                           //!< recent request.
    struct addrinfo *   _request_addrinfo; //!< A pointer to the synthesized
                                           //!< and accumulated head-based
                                           //!< addrinfo as successful request
                                           //!< responses are processed.
    _CFHost *           _request_host;     //!< A pointer to the host object
                                           //!< associated with the request(s).
} _CFHostAresRequest;
#endif /* (HAVE_ARES_INIT && 1) */
#endif /* defined(__linux__) */

/**
 *  The callback type used for deallocating addrinfo.
 *
 */
typedef void (*FreeAddrInfoCallBack)(struct addrinfo *res);
typedef void (*FreeNameInfoCallBack)(char *node, char *service);


#if 0
#pragma mark -
#pragma mark Static Function Declarations
#endif

// Common Functions

static void                     _AddressLookupPerform(_CFHost* host);
static void                     _AddressLookupSchedule_NoLock(_CFHost* host, CFRunLoopRef rl, CFStringRef mode);
static size_t                   _AddressSizeForSupportedFamily(int family);
static CFArrayRef               _CFArrayCreateDeepCopy(CFAllocatorRef alloc, CFArrayRef array);
static void                     _CFHostRegisterClass(void);
static UInt8*                   _CFStringToCStringWithError(CFTypeRef thing, CFStreamError* error);
static CFTypeRef                _CreateAddressLookup(CFStringRef name, CFHostInfoType info, void* context, CFStreamError* error);
static CFTypeRef                _CreateDNSLookup(CFTypeRef thing, CFHostInfoType info, void* context, CFStreamError* error);
static Boolean                  _CreateLookup_NoLock(_CFHost* host, CFHostInfoType info, Boolean* _Radar4012176);
static CFTypeRef                _CreateMasterAddressLookup(CFStringRef name, CFHostInfoType info, CFTypeRef context, CFStreamError* error);
static CFTypeRef                _CreateNameLookup(CFDataRef address, void* context, CFStreamError* error);
static void                     _ExpireCacheEntries(void);
#if defined(__MACH__) || (HAVE_GETADDRINFO_A && 0)
static void                     _GetAddrInfoCallBack(int eai_status, const struct addrinfo* res, void* ctxt);
#endif
static void                     _GetAddrInfoCallBackWithFree(int eai_status, const struct addrinfo *res, void *ctxt, FreeAddrInfoCallBack freeaddrinfo_cb);
static void                     _GetNameInfoCallBackWithFreeAndWithShouldLock(int eai_status, char *hostname, char *serv, void* ctxt, FreeNameInfoCallBack freenameinfo_cb, Boolean should_lock);
static void                     _GetNameInfoCallBackWithFree_NoLock(int eai_status, char *hostname, char *serv, _CFHost * host, CFHostClientCallBack *cb, void **info, CFStreamError *error);
static void                     _HandleGetAddrInfoStatus(int eai_status, CFStreamError* error, Boolean intuitStatus);
static Boolean                  _HostBlockUntilComplete(_CFHost* host);
static void                     _HostCancel(_CFHost* host);
static _CFHost*                 _HostCreate(CFAllocatorRef allocator);
static void                     _HostDestroy(_CFHost* host);
static CFStringRef              _HostDescribe(_CFHost* host);
static void                     _HostLookupCancel_NoLock(_CFHost* host);
#if defined(__MACH__) || (HAVE_GETADDRINFO_A && 0)
static void                     _InitGetAddrInfoHints(CFHostInfoType info, struct addrinfo *hints);
#endif
static void                     _PrimaryLookupCallBack(CFHostRef theHost, CFHostInfoType typeInfo, const CFStreamError *error, CFStringRef name);

// Linux-specific Functions

#if defined(__linux__)
static CFFileDescriptorRef      _CreateDNSLookup_Linux(CFTypeRef thing, CFHostInfoType type, void* context, CFStreamError* error);

// Linux / glibc getaddrinfo_a-specific Functions

#if HAVE_GETADDRINFO_A && 0
static int                      _CreateAddressLookupRequest(const char *name, CFHostInfoType info, int signal, CFStreamError *error);
static CFFileDescriptorRef      _CreateAddressLookupSource_GetAddrInfo_A(int signal, CFTypeRef context, CFStreamError *error);
static CFFileDescriptorRef      _CreatePrimaryAddressLookup_Linux_GetAddrInfo_A(CFStringRef name, CFHostInfoType info, CFTypeRef context, CFStreamError* error);
static int                      _CreateSignalFd(int signal, CFStreamError *error);
static void                     _PrimaryAddressLookupCallBack_Linux_GetAddrInfo_A(CFFileDescriptorRef fdref, CFOptionFlags callBackTypes, void *info);
static int                      _SignalFdClearGetAddrInfoSignalWithHost(_CFHost *host);
static int                      _SignalFdClearSignalWithError(int signal, sigset_t *set, CFStreamError *error);
static int                      _SignalFdModifySignalWithError(int how, int signal, sigset_t *set, CFStreamError *error);
static int                      _SignalFdSetSignalWithError(int signal, sigset_t *set, CFStreamError *error);
static struct gaicb *           _SignalFdGetAddrInfoResult(CFFileDescriptorRef fdref);
#endif /* HAVE_GETADDRINFO_A && 0 */

// Linux / c-ares-specific Functions

#if HAVE_ARES_INIT && 1
static void                     _AresAccumulateAddrInfo(_CFHostAresRequest *ares_request, struct addrinfo *ai);
static void                     _AresClearOrSetRequestEvents(_CFHostAresRequest *ares_request,
                                                             uint16_t event,
                                                             Boolean set);
static CFTypeRef                _AresCreateNullLookup(_CFHostAresRequest *ares_request);
static _CFHostAresRequest *     _AresCreateRequestAndChannel(_CFHost *host, CFHostInfoType type, ares_sock_state_cb sock_state_cb, CFStreamError *error);
static void                     _AresDestroyRequestAndChannel(_CFHostAresRequest *ares_request);
static Boolean                  _AresIsNullLookup(const _CFHostAresRequest *ares_request);
static void                     _AresSocketDataCallBack(CFFileDescriptorRef fdref, CFOptionFlags callBackTypes, void *info);
static void                     _AresFreeAddrInfo(struct addrinfo *res);
static void                     _AresHostByCompletedCallBack(void *arg,
                                                             int status,
                                                             int timeouts,
                                                             struct hostent *hostent);
static void                     _AresNameInfoCompletedCallBack(void *arg,
                                                               int status,
                                                               int timeouts,
                                                               char *node,
                                                               char *service);
static void                     _AresSocketStateCallBack(void *data, ares_socket_t socket_fd, int readable, int writable);
static int                      _AresStatusMapToAddrInfoError(int ares_status);
static struct addrinfo * _AresHostentToAddrInfo(const struct hostent *hostent, CFStreamError *error);
static void                     _AresStatusMapToStreamError(int status, CFStreamError *error);
static void                     _AresUpdateLastStatus(_CFHostAresRequest *ares_request, int status);
static void                     _CFHostInitializeAres(void);
static void                     _CopyHostentAddrToAddrInfo(int family, struct addrinfo *ai, const char *data);
static CFFileDescriptorRef      _CreateNameLookup_Ares(CFDataRef address, void* context, CFStreamError* error);
static CFFileDescriptorRef      _CreatePrimaryAddressLookup_Ares(CFStringRef name, CFHostInfoType info, CFTypeRef context, CFStreamError* error);
#if LOG_CFHOST
static void                     _LogAddress(int aFamily, const char *aData);
static void                     _LogHostent(const struct hostent *hostent);
static void                     _LogName(const char *aType, const char *aName);
#endif /* LOG_CFHOST */
static void                     _MaybeReenableRequestCallBacks(_CFHostAresRequest *aRequest);
#endif /* HAVE_ARES_INIT && 1 */

#endif /* defined(__linux__) */

// Mach-specific Functions

#if defined(__MACH__)
static CFMachPortRef            _CreateDNSLookup_Mach(CFTypeRef thing, CFHostInfoType type, void* context, CFStreamError* error);
static CFMachPortRef            _CreatePrimaryAddressLookup_Mach(CFStringRef name, CFHostInfoType info, CFTypeRef context, CFStreamError* error);
static CFMachPortRef            _CreateNameLookup_Mach(CFDataRef address, void* context, CFStreamError* error);
static SCNetworkReachabilityRef _CreateReachabilityLookup(CFTypeRef thing, void* context, CFStreamError* error);
static void                     _DNSCallBack(int32_t status, char *buf, uint32_t len, struct sockaddr *from, int fromlen, void *context);
static void                     _DNSMachPortCallBack(CFMachPortRef port, void* msg, CFIndex size, void* info);
static void                     _GetAddrInfoMachPortCallBack(CFMachPortRef port, void* msg, CFIndex size, void* info);
static void                     _GetNameInfoCallBack(int eai_status, char *hostname, char *serv, void* ctxt);
static void                     _GetNameInfoMachPortCallBack(CFMachPortRef port, void* msg, CFIndex size, void* info);
static Boolean                  _IsDottedIp(CFStringRef name);
static void                     _NetworkReachabilityByIPCallBack(_CFHost* host);
static void                     _NetworkReachabilityCallBack(SCNetworkReachabilityRef target, SCNetworkConnectionFlags flags, void* ctxt);
typedef void (*FreeNameInfoCallBack)(char *hostname, char *serv);
#endif /* defined(__MACH__) */


#if 0
#pragma mark -
#pragma mark Globals
#endif

static _CFOnceLock _kCFHostRegisterClass = _CFOnceInitializer;
#if defined(__linux__)
#if (HAVE_ARES_INIT && 1)
static _CFOnceLock _kCFHostInitializeAres = _CFOnceInitializer;
#endif /* (HAVE_ARES_INIT && 1) */
#endif /* defined(__linux__) */
static CFTypeID _kCFHostTypeID = _kCFRuntimeNotATypeID;

static _CFMutex* _HostLock;						/* Lock used for cache and master list */
static CFMutableDictionaryRef _HostLookups;		/* Active hostname lookups; for duplicate supression */
static CFMutableDictionaryRef _HostCache;		/* Cached hostname lookups (successes only) */


#if 0
#pragma mark -
#pragma mark Inline Function Definitions
#endif

CF_INLINE void _CFHostLock(_CFHost *host) {
    __CFSpinLock(&(host->_lock));
}

CF_INLINE void _CFHostUnlock(_CFHost *host) {
    __CFSpinUnlock(&(host->_lock));
}

#if 0
#pragma mark -
#pragma mark Static Function Definitions
#endif

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
	_CFHostLock(host);

	// Release the user's context info if there is some and a release method
	if (host->_client.info && host->_client.release)
		host->_client.release(host->_client.info);

	// If there is a lookup, release it.
	if (host->_lookup) {
        _HostLookupCancel_NoLock(host);
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

	_CFHostLock(host);

	result = CFStringCreateWithFormat(CFGetAllocator((CFHostRef)host),
									  NULL,
									  _kCFHostDescribeFormat,
									  host,
									  host->_info);

	_CFHostUnlock(host);

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
	_CFHostLock(host);

	// If the lookup canceled, don't need to do any of this.
	if (host->_lookup) {

		// Save the callback if there is one at this time.
		cb = host->_callback;

		// Save the type of lookup for the callback.
		type = host->_type;

		// Save the error and client information for the callback
		memmove(&error, &(host->_error), sizeof(error));
		info = host->_client.info;

        _HostLookupCancel_NoLock(host);
	}

	// Unlock the host so the callback can be made safely.
	_CFHostUnlock(host);

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
	_CFHostLock(host);

	// Check that lookup exists.
	while (host->_lookup) {

		// Unlock again so the host can continue to be processed.
		_CFHostUnlock(host);

		// Run the loop in a private mode with it returning whenever a source
		// has been handled.
		CFRunLoopRunInMode(_kCFHostBlockingMode, DBL_MAX, TRUE);

		// Lock again in preparation for lookup check
		_CFHostLock(host);
	}

	// Fail if there was an error.
	if (host->_error.error)
		result = FALSE;

	// Unlock the host again.
	_CFHostUnlock(host);

	// Unschedule from the blocking mode
	CFHostUnscheduleFromRunLoop((CFHostRef)host, rl, _kCFHostBlockingMode);

	return result;
}

/* static */ void
_HostLookupCancel_NoLock(_CFHost* host) {

    __Require(host != NULL, done);

    // Remove the lookup from run loops and modes
    _CFTypeUnscheduleFromMultipleRunLoops(host->_lookup, host->_schedules);

    // Invalidate the lookup
    _CFTypeInvalidate(host->_lookup);

    // Release the lookup.
    CFRelease(host->_lookup);
    host->_lookup = NULL;
    host->_type = _kCFNullHostInfoType;

 done:
    return;
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
#else /* #if defined(__MACH__) */
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
_CFStringToCStringWithError(CFTypeRef thing, CFStreamError* error) {
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

/**
 *  @brief
 *    Handle and map status returned by 'getaddrinfo*' to a
 *    CFStreamError.
 *
 *  This maps status returned by 'getaddrinfo*' and friends to
 *  a CFStreamError object.
 *
 *  @param[in]   eai_status    The getaddrinfo status to map.
 *  @param[out]  error         A pointer to the CFStreamError object
 *                             to map the getaddrinfo status to.
 *  @param[in]   intuitStatus  If the @a eai_status is zero (0), when
 *                             asserted attempt to further intuit the
 *                             status based on 'errno'.
 *
 */
/* static */ void
_HandleGetAddrInfoStatus(int eai_status, CFStreamError* error, Boolean intuitStatus) {
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

#if defined(__MACH__) || (HAVE_GETADDRINFO_A && 0)
/**
 *  @brief
 *    Establish the hint data passed to 'getaddrinfo*' and friends.
 *
 *  This establishes the hint data passed to 'getaddrinfo*' and
 *  friends for a forward DNS (that is, name-to-address) lookup based
 *  on the specified host info query type.
 *
 *  @param[in]      info     A value of type CFHostInfoType
 *                           specifying the type of information that
 *                           is to be retrieved. See #CFHostInfoType
 *                           for possible values.
 *  @param[out]     hints    A pointer to storage in which to store
 *                           the hint data.
 *
 */
/* static */ void
_InitGetAddrInfoHints(CFHostInfoType info, struct addrinfo *hints) {
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
#endif /* defined(__MACH__) || (HAVE_GETADDRINFO_A && 0) */

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
    result = _CreatePrimaryAddressLookup_Ares(name, info, context, error);
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
_CreatePrimaryAddressLookup_Mach(CFStringRef name, CFHostInfoType info, CFTypeRef context, CFStreamError* error) {

	const CFAllocatorRef allocator = CFGetAllocator(name);
	UInt8* buffer;
	CFMachPortRef result = NULL;

	buffer = _CFStringToCStringWithError(name, error);

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
#if HAVE_GETADDRINFO_A && 0
/* static */ int
_SignalFdModifySignalWithError(int how, int signal, sigset_t *set, CFStreamError *error) {
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

	_CFHostLock(host);

	result = _SignalFdClearSignalWithError(signal, &sigset, &host->_error);

	_CFHostUnlock(host);

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
_PrimaryAddressLookupCallBack_Linux_GetAddrInfo_A(CFFileDescriptorRef fdref, CFOptionFlags callBackTypes, void *info) {
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
									_PrimaryAddressLookupCallBack_Linux_GetAddrInfo_A,
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

	buffer = _CFStringToCStringWithError(name, error);
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
#endif /* HAVE_GETADDRINFO_A && 0 */

#if HAVE_ARES_INIT && 1
/**
 *  Initialize the c-ares library.
 *
 */
/* static */ void
_CFHostInitializeAres(void) {
    int status = ares_library_init(ARES_LIB_INIT_ALL);
    __Verify_Action(status == ARES_SUCCESS, abort());
}

/* static */ int
_AresStatusMapToAddrInfoError(int ares_status) {
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

/* static */ void
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

/* static */ void
_MaybeReenableRequestCallBacks(_CFHostAresRequest *aRequest) {
    __Require(aRequest != NULL, done);

    if (aRequest->_request_lookup != NULL) {
        if (aRequest->_request_events & POLLIN) {
            CFFileDescriptorEnableCallBacks(aRequest->_request_lookup,
                                            kCFFileDescriptorReadCallBack);
        }

        if (aRequest->_request_events & POLLOUT) {
            CFFileDescriptorEnableCallBacks(aRequest->_request_lookup,
                                            kCFFileDescriptorWriteCallBack);
        }
    }

 done:
    return;
}

/* static */ void
_AresSocketDataCallBack(CFFileDescriptorRef fdref, CFOptionFlags callBackTypes, void *info) {
    _CFHostAresRequest *             ares_request = (_CFHostAresRequest *)(info);
    CFFileDescriptorNativeDescriptor fd;
    ares_socket_t                    readfd;
    ares_socket_t                    writefd;

    // Get the platform-native descriptor associated with the
    // descriptor object.

    fd = CFFileDescriptorGetNativeDescriptor(fdref);
    __Require(fd != -1, done);

    // Determine, based on flags established in the c-ares descriptor
    // callback, whether that platform-native descriptor will be used
    // for reading and/or writing.

    readfd = (ares_request->_request_events & POLLIN) ? fd : ARES_SOCKET_BAD;
    writefd = (ares_request->_request_events & POLLOUT) ? fd : ARES_SOCKET_BAD;

    // Request c-ares to process any data pending in the channel on
    // the platform-native descriptor for reading and/or writing.

    ares_process_fd(ares_request->_request_channel,
                    readfd,
                    writefd);

    // If there are no further requests pending, destroy the channel
    // and dealloctae the request object that contains it. The
    // descriptor object itself will be deallocated when it is removed
    // from the run loop.
    //
    // Otherwise, re-enable the one-shot callback(s) for this
    // descriptor object so that additional data can be processed, if
    // necessary.

    if (ares_request->_request_pending == 0) {
        _AresDestroyRequestAndChannel(ares_request);
    } else {
        _MaybeReenableRequestCallBacks(ares_request);
    }

 done:
    return;
}

/* static */ void
_AresClearOrSetRequestEvents(_CFHostAresRequest * ares_request,
                             uint16_t event,
                             Boolean set) {
    if (set) {
        ares_request->_request_events |= event;
    } else {
        ares_request->_request_events &= ~event;
    }
}

/**
 *  @brief
 *    Call back invoked whenever a socket associated with a c-ares
 *    channel request changes state.
 *
 *  This is the call back handler invoked whenever a socket associated
 *  with a c-ares channel request changes state, including first-time
 *  creation as well as transitioning into and out of read- or
 *  writability.
 *
 *  @note
 *    As noted above in the description of #_CFHostAresRequest, c-ares
 *    supports the notion of an effective timeout for lookup channel,
 *    via 'ares_timeout'. However, the CFileDescriptor object, while
 *    simpler for this application, does not effecitvely support a
 *    timeout for watched descriptors and implies a higher poll/select
 *    rate for a pending request than were a timeout supported. If
 *    timeout behavior is desired, CFFileDescriptor could be
 *    unilaterally changed to CFSocket.
 *
 *  @param[in,out]  data       A pointer to call back-specific data
 *                             registered with 'ares_init_options';
 *                             here, a pointer to the heap-based
 *                             object associated with the socket and
 *                             its associated c-ares request channel.
 *  @param[in]      socket_fd  The socket associated with the c-ares
 *                             request channel whose state has
 *                             changed.
 *  @param[in]      readable   True if the socket should listen for
 *                             read events.
 *  @param[in]      writable   True if the socket should listen for
 *                             write events.
 *
 */
/* static */ void
_AresSocketStateCallBack(void *data,
                         ares_socket_t socket_fd,
                         int readable,
                         int writable) {
	const Boolean           kCloseOnInvalidate = TRUE;
    _CFHostAresRequest *    ares_request = (_CFHostAresRequest *)(data);
    CFFileDescriptorContext fdContext    = { 0, (void *)ares_request, NULL, NULL, NULL };
	CFFileDescriptorRef     fdref        = NULL;

    __Require(ares_request != NULL, done);
    __Require_Action(socket_fd != ARES_SOCKET_BAD,
                     done,
                     ares_request->_request_error->error = EBADF;
                     ares_request->_request_error->domain = kCFStreamErrorDomainPOSIX);

    // As with the larger CFHost object, the 'lookup' is the
    // CoreFoundation-compatible and run loop-schedulable object that
    // will handle asynchronous I/O activity for the host lookup
    // request.
    //
    // So, first, if no descriptor object has yet been associated with
    // this c-ares channel socket, attempt to allocate one. Do NOT
    // close the socket on invalidation of the descriptor object;
    // c-ares will handle that when the channel is destroyed.

    if (ares_request->_request_lookup == NULL) {
        fdref = CFFileDescriptorCreate(kCFAllocatorDefault,
                                       socket_fd,
                                       !kCloseOnInvalidate,
                                       _AresSocketDataCallBack,
                                       &fdContext);

        if (fdref == NULL) {
            ares_request->_request_error->error = ENOMEM;
            ares_request->_request_error->domain = kCFStreamErrorDomainPOSIX;

        } else {
            ares_request->_request_lookup = fdref;

        }
    }

    // Second, whether we just allocated it anew or are back here due
    // to subsequent socket state changes, update the poll/select
    // event flags appropriate for this socket which will ultimately
    // determine--in various locations, including here--which
    // descriptor object call backs are enabled.

    if (ares_request->_request_lookup != NULL) {
        _AresClearOrSetRequestEvents(ares_request, POLLIN,  readable);
        _AresClearOrSetRequestEvents(ares_request, POLLOUT, writable);

        _MaybeReenableRequestCallBacks(ares_request);
    }

 done:
    return;
}

#if LOG_CFHOST
/* static */ void
_LogAddress(int aFamily, const char *aData) {
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

        if (result) {
            __CFHostMaybeLog("%s\n", buffer);
        }
    }
}

/* static */ void
_LogHostent(const struct hostent *hostent) {
    int   i;
    char *current;

    __Require(hostent != NULL, done);

    if (hostent->h_name != NULL) {
        _LogName("hostname", hostent->h_name);
    }

    if (hostent->h_aliases != NULL) {
        for (i = 0; ((current = hostent->h_aliases[i]) != NULL); i++) {
            _LogName("alias", current);
        }
    }

    if (hostent->h_addr_list != NULL) {
        for (i = 0; ((current = hostent->h_addr_list[i]) != NULL); i++) {
            _LogAddress(hostent->h_addrtype,
                        current);
        }
    }

 done:
    return;
}

/* static */ void
_LogName(const char *aType, const char *aName) {
    __CFHostMaybeLog("%s: %s\n", aType, aName);
}
#endif /* LOG_CFHOST */

/**
 *  Deallocate addrinfo created by #_AresHostentToAddrInfo.
 *
 *  @note
 *    Do NOT call freeaddrinfo instead of this for addrinfo that was
 *    created by #_AresHostentToAddrInfo! At best, undefined behavior
 *    will result. At worst, it will result in a crash or data
 *    corruption.
 *
 *  @param[in]  res  A pointer to the addrinfo to deallocate.
 *
 */
/* static */ void
_AresFreeAddrInfo(struct addrinfo *res) {
    struct addrinfo *          current;
    struct addrinfo * volatile next;

    for (current = res; current != NULL; current = next) {
        next = current->ai_next;
        CFAllocatorDeallocate(kCFAllocatorDefault, current);
    }
}

/* static */ void
_CopyHostentAddrToAddrInfo(int family, struct addrinfo *ai, const char *data) {
    struct sockaddr_in *  saddr;
    struct sockaddr_in6 * saddr6;

    switch (family) {

    case AF_INET:
        saddr = (struct sockaddr_in *)ai->ai_addr;

        memcpy(&saddr->sin_addr, data, sizeof(struct in_addr));
        saddr->sin_family = family;
        break;

    case AF_INET6:
        saddr6 = (struct sockaddr_in6 *)ai->ai_addr;

        memcpy(&saddr6->sin6_addr, data, sizeof(struct in6_addr));
        saddr6->sin6_family = family;
        break;

    }
}

/* static */ struct addrinfo *
_AresHostentToAddrInfo(const struct hostent *hostent, CFStreamError *error) {
    int               i;
    int               status   = 0;
    const char *      data     = NULL;
    struct addrinfo * result   = NULL;
    struct addrinfo * previous = NULL;
    struct addrinfo * current  = NULL;

    __Require_Action(hostent != NULL, map_status, status = EINVAL);
    __Require_Action(hostent->h_name != NULL, map_status, status = EINVAL);
    __Require_Action(hostent->h_addr_list != NULL, map_status, status = EINVAL);
    __Require_Action(error != NULL, map_status, status = EINVAL);

    // Loop over each hostent address and create and map it into an
    // addrinfo.

    for (i = 0; ((data = hostent->h_addr_list[i]) != NULL); i++)
    {
        const char   kNull         = '\0';
        const int    family        = hostent->h_addrtype;
        const size_t canonname_len = strlen(hostent->h_name) + sizeof(kNull);
        size_t       addr_size;
        size_t       total_size;

        // Determine how large the socket address data at the tail of
        // the allocated addrinfo block should be.
        //
        // If the size is not greater than zero, it is not an
        // Internet-based address family; there's nothing more to do
        // here. Set the error and bail out.

        addr_size = _AddressSizeForSupportedFamily(family);
        __Require_Action(addr_size > 0,
                         done,
                         error->error  = EAI_ADDRFAMILY;
                         error->domain = (CFStreamErrorDomain)kCFStreamErrorDomainNetDB);

        // Allocate the addrinfo block as well as enough data to
        // contain both the trailing-but-inlined socket address and
        // canonical name data. This inlining simplifies having to
        // maintain and manage two additional dangling heap pointers
        // for this information.

        total_size = sizeof(struct addrinfo) + addr_size + canonname_len;

        current = CFAllocatorAllocate(kCFAllocatorDefault, total_size, 0);
        __Require_Action(current != NULL, map_status, status = ENOMEM);

        memset(current, 0, total_size);

        // Set the addrinfo address pointer to the
        // trailing-but-inlined socket address and canonical name
        // data. The socket address is set first to avoid any
        // alignment issues with that structure that might otherwise
        // result from following an arbitrarily-sized canonical name.

        current->ai_addr      = (struct sockaddr *)((uint8_t *)current + sizeof(struct addrinfo));
        current->ai_canonname = (char *)((uint8_t *)current->ai_addr + addr_size);

        current->ai_family   = family;
        current->ai_socktype = SOCK_STREAM;
        current->ai_addrlen  = addr_size;

        memcpy(current->ai_canonname, hostent->h_name, canonname_len);

        // Copy the actual address data from the current hostent
        // address to the addrinfo socket address.

        _CopyHostentAddrToAddrInfo(family, current, data);

        // Chain up the addrinfo data, as created.

        if (result == NULL)
            result = current;

        if (previous != NULL)
            previous->ai_next = current;

        previous = current;
    }

map_status:
    if (status != 0) {
        error->error = status;
        error->domain = kCFStreamErrorDomainPOSIX;

        if (result != NULL) {
            _AresFreeAddrInfo(result);
            result = NULL;
        }
    }

done:
    return result;
}

/* static */ void
_AresAccumulateAddrInfo(_CFHostAresRequest *ares_request, struct addrinfo *ai) {
    struct addrinfo *tail;

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
    return;
}

/**
 *  @brief
 *    Create and return a null lookup object.
 *
 *  This creates and returns a null lookup object to be used when
 *  c-ares is able to "fallthrough" and satisfy a lookup request
 *  without creating a poll/select-able run loop source (such as from
 *  a cache or a local, host-based file (for example, /etc/hosts). The
 *  CFHost common infrastructure expects that every successful lookup
 *  creates a lookup object.
 *
 *  @returns
 *    A pointer to the null lookup object.
 *
 */
/* static */ CFTypeRef
_AresCreateNullLookup(_CFHostAresRequest *ares_request) {
    // Implementation-wise, we choose to return the special kCFNull
    // object which is effectively invariant and will not have any
    // impact by being passed to those methods in CFNetworkSchedule.c.

    return (CFTypeRef)kCFNull;
}

/**
 *  Determine whether the specified lookup source is the special null
 *  lookup source.
 *
 *  @param[in]  lookup  The lookup source to check.
 *
 *  @returns
 *    True if @lookup is the special null lookup source; otherwise,
 *    false.
 *
 */
/* static */ Boolean
_AresIsNullLookup(const _CFHostAresRequest *ares_request) {
    return CFEqual(ares_request->_request_lookup, kCFNull);
}

/* static */ void
_AresUpdateLastStatus(_CFHostAresRequest *ares_request, int status) {
    if (ares_request->_request_status != ARES_SUCCESS) {
        ares_request->_request_status = status;
    }
}

/* static */ void
_AresHostByCompletedCallBack(void *arg,
                             int status,
                             int timeouts,
                             struct hostent *hostent) {
    _CFHostAresRequest *ares_request = (_CFHostAresRequest *)(arg);


    if (ares_request->_request_pending > 0) {
        ares_request->_request_pending--;
    }

    if (status == ARES_SUCCESS) {
        if (hostent != NULL) {
            struct addrinfo *ai;
#if LOG_CFHOST
            _LogHostent(hostent);
#endif /* LOG_CFHOST */

            ai = _AresHostentToAddrInfo(hostent, ares_request->_request_error);
            if (ai != NULL) {
                _AresAccumulateAddrInfo(ares_request, ai);
            }

            if (ares_request->_request_pending > 0) {
                _MaybeReenableRequestCallBacks(ares_request);

            } else if (ares_request->_request_pending == 0) {
                // If the lookup was a "fallthrough", socket-free
                // lookup, create a null lookup source to keep CFHost
                // common infrastructure requirements satisfied: if
                // there was a successful lookup, there must be a
                // lookup source.

                if (ares_request->_request_lookup == NULL) {
                    ares_request->_request_lookup = _AresCreateNullLookup(ares_request);
                }
            }
        }
    } else {
        __CFHostMaybeLog("Forward DNS lookup failed: %d: %s\n",
                         status, ares_strerror(status));
    }

    // If there are no further requests pending, then we are at a
    // "Happy Eyeballs" decision point. By extension of being in this
    // call back, we issued one or two lookup requests via
    // ares_gethostbyaddr: an IPv4-only request, an IPv6-only request,
    // or a parallel IPv4 and IPv6 request. One or both could have
    // succeeded or both failed. Complete the lookup accordingly and
    // release the resources associated with the original lookup.

    if (ares_request->_request_pending == 0) {
        const int last_status = ares_request->_request_status;
        const int this_status = status;
        int       final_status;

        if ((last_status == ARES_SUCCESS) || (this_status == ARES_SUCCESS)) {
            final_status = ARES_SUCCESS;
        } else {
            final_status = last_status;
        }

        // Invoke the common, shared callback for
        // {ares_,}getaddrinfo{,_a} with the derived final lookup
        // status.

        _GetAddrInfoCallBackWithFree(_AresStatusMapToAddrInfoError(final_status),
                                     ares_request->_request_addrinfo,
                                     ares_request->_request_host,
                                     _AresFreeAddrInfo);

        // Release the buffer that was previously allocated
        // for the lookup name when the request was made.

        if (ares_request->_request_name != NULL) {
            CFAllocatorDeallocate(kCFAllocatorDefault,
                                  (void *)ares_request->_request_name);
            ares_request->_request_name = NULL;
        }
    }

    _AresUpdateLastStatus(ares_request, status);
}

/* static */ void
_AresNameInfoCompletedCallBack(void *arg,
                               int status,
                               int timeouts,
                               char *node,
                               char *service) {
    _CFHostAresRequest *ares_request = (_CFHostAresRequest *)(arg);


    if (ares_request->_request_pending > 0) {
        ares_request->_request_pending--;
    }

    if (status == ARES_SUCCESS) {
        if ((node != NULL) || (service != NULL)) {
            if (ares_request->_request_pending > 0) {
                _MaybeReenableRequestCallBacks(ares_request);

            } else if (ares_request->_request_pending == 0) {
                // If the lookup was a "fallthrough", socket-free
                // lookup, create a null lookup source to keep CFHost
                // common infrastructure requirements satisfied: if
                // there was a successful lookup, there must be a
                // lookup source.

                if (ares_request->_request_lookup == NULL) {
                    ares_request->_request_lookup = _AresCreateNullLookup(ares_request);
                }
            }
        }
    }

    // With c-ares, we can get to this lookup conclusion point via one
    // of two paths. First, on a cache or local file lookup or on an
    // error path, we can arrive here without going through a socket
    // state and subsequent data event, which means we are still in
    // the context of a 'CFHostStartInfoResolution' down call. Second,
    // on a DNS server lookup, we can arrive here via a socket state
    // and subsequent data event, which means we are getting scheduled
    // and down called via the run loop dispatch.
    //
    // In the first case, the host object is locked and any call into
    // _GetNameinfoCallBackWithFree would deadlock, based on the Mach
    // legacy code base. In the second case, the host object is
    // unlocked and any such call will not deadlock.
    //
    // Consequently, we need to handle these two cases distinctly and
    // with care. The distinguishing factor will be that the first
    // case uses the special null lookup and should NOT lock; whereas,
    // the second case does not and SHOULD lock.
    //
    // The _GetNameinfoCallBackWithFree has been reworked with an
    // expanded API and renamed
    // _GetNameInfoCallBackWithFreeAndWithShouldLock to take not only
    // an optional resource deallocation callback but also a Boolean
    // to indicate whether the host object should be locked within the
    // function.
    //
    // The other nuance we need to deal with in the first case is that
    // normally host->_lookup will not get assigned until the call to
    // _CreateNameLookup unwinds in _CreateLookup_NoLock. However, the
    // down call into _GetNameInfoCallBackWithFreeAndWithShouldLock
    // (from the Mach legacy code base) assumes host->_lookup has
    // already been assigned. We will have to manually assign it in
    // that case.

    if (ares_request->_request_pending == 0) {
        const int            eai_status  = _AresStatusMapToAddrInfoError(status);
        const Boolean        is_null     = _AresIsNullLookup(ares_request);
        const Boolean        should_lock = !is_null;
        _CFHost *            host        = ares_request->_request_host;
        FreeNameInfoCallBack free_cb     = NULL;

        if (is_null) {
            ares_request->_request_host->_lookup = ares_request->_request_lookup;
        }

        _GetNameInfoCallBackWithFreeAndWithShouldLock(eai_status,
                                                      node,
                                                      service,
                                                      host,
                                                      free_cb,
                                                      should_lock);
    }

    _AresUpdateLastStatus(ares_request, status);
}

/**
 *  @brief
 *    Create and initialize a heap-based c-ares request object and
 *    channel.
 *
 *  This allocates and initializes a heap-based c-ares request object
 *  and channel for use in making forward and reverse DNS lookups via
 *  c-ares.
 *
 *  @param[in]   host           The host, obtained by previously calling
 *                              #CFHostCreateCopy,
 *                              #CFHostCreateWithAddress, or
 *                              #CFHostCreateWithName, for which the
 *                              request object and channel is to be
 *                              created. This value must not be NULL.
 *  @param[in]   sock_state_cb  A pointer to the socket state callback
 *                              that will be invoked on any state
 *                              changes in sockets associated with the
 *                              request and channel.
 *  @param[out]  error          A pointer to a #CFStreamError object
 *                              that will be set if an error occurs
 *                              during allocation and initialization
 *
 *  @returns
 *    A pointer to the allocated and initialized object on success;
 *    otherwise, null.
 *
 */
/* static */ _CFHostAresRequest *
_AresCreateRequestAndChannel(_CFHost *host, CFHostInfoType type, ares_sock_state_cb sock_state_cb, CFStreamError *error) {
    const int            optmask = ARES_OPT_SOCK_STATE_CB;
    struct ares_options  options;
    int                  status;
    _CFHostAresRequest * result = NULL;

    __Require(host != NULL, done);
    __Require(sock_state_cb != NULL, done);
    __Require(error != NULL, done);

    // Allocate the request object.

	result = (_CFHostAresRequest *)CFAllocatorAllocate(kCFAllocatorDefault, sizeof(_CFHostAresRequest), 0);
	__Require_Action(result != NULL,
					 done,
					 error->error  = ENOMEM;
					 error->domain = kCFStreamErrorDomainPOSIX);

    memset(result, 0, sizeof(_CFHostAresRequest));

    // Initialize the c-ares lookup request channel with the socket
    // state callback option.

    options.sock_state_cb      = sock_state_cb;
    options.sock_state_cb_data = result;

    status = ares_init_options(&result->_request_channel,
                               &options,
                               optmask);
    __Require_Action(status == ARES_SUCCESS,
                     done,
                     _AresStatusMapToStreamError(status, error);
                     CFAllocatorDeallocate(kCFAllocatorDefault, result));

    result->_request_error = error;
    result->_request_host  = host;
    result->_request_type  = type;

 done:
    return result;
}

/* static */ void
_AresDestroyRequestAndChannel(_CFHostAresRequest *ares_request) {
    __Require(ares_request != NULL, done);

    ares_destroy(ares_request->_request_channel);

    CFAllocatorDeallocate(kCFAllocatorDefault, ares_request);

 done:
    return;
}

/* static */ CFFileDescriptorRef
_CreatePrimaryAddressLookup_Ares(CFStringRef name, CFHostInfoType info, CFTypeRef context, CFStreamError* error) {
	const CFAllocatorRef allocator = CFGetAllocator(name);
	UInt8*               buffer;
    _CFHostAresRequest * ares_request = NULL;
    Boolean              ipv4only     = FALSE;
    Boolean              ipv6only     = FALSE;
    CFFileDescriptorRef  result       = NULL;


	// Create a CFString representation of the lookup by converting it
	// into a null-terminated C string buffer consumable by
	// getaddrinfo_a.

	buffer = _CFStringToCStringWithError(name, error);
	__Require(buffer != NULL, done);

	ares_request = _AresCreateRequestAndChannel((_CFHost *)context,
                                                kCFHostAddresses,
                                                _AresSocketStateCallBack,
                                                error);
	__Require_Action(ares_request != NULL,
					 done,
                     CFAllocatorDeallocate(allocator, buffer));

    ares_request->_request_name = (const char *)buffer;

	if (info == _kCFHostIPv4Addresses) {
        ipv4only = TRUE;
        ares_request->_request_pending = 1;
	} else if (info == _kCFHostIPv6Addresses) {
        ipv6only = TRUE;
        ares_request->_request_pending = 1;
	} else {
        ares_request->_request_pending = 2;
    }

    if (!ipv6only) {
        ares_gethostbyname(ares_request->_request_channel,
                           ares_request->_request_name,
                           AF_INET,
                           _AresHostByCompletedCallBack,
                           ares_request);
    }

    if (!ipv4only) {
        ares_gethostbyname(ares_request->_request_channel,
                           ares_request->_request_name,
                           AF_INET6,
                           _AresHostByCompletedCallBack,
                           ares_request);
    }

    result = ares_request->_request_lookup;

 done:
	return result;
}
#endif /* HAVE_ARES_INIT && 1 */
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

                    // Kick off an internal, asynchronous resolution
                    // that will nest with the external resolution. It
                    // is definitionally asynchronous because a
                    // internal asynchronous client callback is set,
                    // which may not be the case with the outer
                    // resolution that triggered this one.

					// Set the asynchronous client callback.

					CFHostSetClient(host, (CFHostClientCallBack)_PrimaryLookupCallBack, &ctxt);

					// Kick off the internal, asynchronous nested
					// resolution.

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
    result = _CreateNameLookup_Ares(address, context, error);
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
_CreateNameLookup_Ares(CFDataRef address, void* context, CFStreamError* error) {
    const struct sockaddr * sa           = (const struct sockaddr *)CFDataGetBytePtr(address);
    const ares_socklen_t    sa_len       = CFDataGetLength(address);
    _CFHost *               host         = (_CFHost *)(context);
    _CFHostAresRequest *    ares_request = NULL;
    CFFileDescriptorRef     result       = NULL;


    ares_request = _AresCreateRequestAndChannel(host,
                                                kCFHostNames,
                                                _AresSocketStateCallBack,
                                                error);
	__Require(ares_request != NULL, done);

    ares_request->_request_pending = 1;

    if (sa_len > 0) {
        const int flags = (ARES_NI_LOOKUPHOST | ARES_NI_LOOKUPSERVICE);

        ares_getnameinfo(ares_request->_request_channel,
                         sa,
                         sa_len,
                         flags,
                         _AresNameInfoCompletedCallBack,
                         ares_request);
    }

    // It is possible, whether on error or whether on cache or local
    // file-based resolution, that we will land here without either
    // _AresQueryCompletedCallBack (less likely) or
    // _AresSocketDataCallBack (more likely) being called. If either
    // of those cases is a non-success case, handle clean-up
    // appropriately.

    if (ares_request->_request_status != ARES_SUCCESS) {
        _AresStatusMapToStreamError(ares_request->_request_status, error);

        _AresDestroyRequestAndChannel(ares_request);
    } else {
        result = ares_request->_request_lookup;

        // If the result is NULL, then we had a callback-free lookup
        // and can destroy the channel and the request. Otherwise, the
        // request and channel will be deallocated in the socket state
        // callback.

        if (result == NULL || _AresIsNullLookup(ares_request)) {
            _AresDestroyRequestAndChannel(ares_request);
        }
    }

 done:
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

		buffer = _CFStringToCStringWithError(thing, error);

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

/* static */ CFTypeRef
_CreateDNSLookup(CFTypeRef thing, CFHostInfoType info, void* context, CFStreamError* error) {
	CFTypeRef result = NULL;

#if defined(__MACH__)
	result = _CreateDNSLookup_Mach(thing, info, context, error);
#elif defined(__linux__)
    result = _CreateDNSLookup_Linux(thing, info, context, error);
#else
#warning "Platform portability issue!"
#endif

	return result;
}

#if defined(__MACH__)
/* static */ CFMachPortRef
_CreateDNSLookup_Mach(CFTypeRef thing, CFHostInfoType info, void* context, CFStreamError* error) {
	const CFAllocatorRef allocator = CFGetAllocator(thing);
	UInt8* buffer;
	CFMachPortRef result = NULL;

	buffer = _CFStringToCStringWithError(thing, error);

	if (!buffer)
		return result;

	// Got a good name to send to lookup.
	else {

		mach_port_t prt = MACH_PORT_NULL;
		CFMachPortContext ctxt = {0, (void*)context, CFRetain, CFRelease, CFCopyDescription};

		// Start the async lookup
		error->error = dns_async_start(&prt, (const char*)buffer, ((info & 0xFFFF0000) >> 16), (info & 0x0000FFFF), 1, _DNSCallBack_Mach, (void*)context);

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
_CreateDNSLookup_Linux(CFTypeRef thing, CFHostInfoType info, void* context, CFStreamError* error) {
	CFFileDescriptorRef     result = NULL;

	__CFHostTraceEnterWithFormat("thing %p info %x context %p error %p\n",
								 thing, info, context, error);

	CFShow(thing);

    // It is not clear that this function is practically reachable as
    // CFHost and CFNetwork are currently implemented. Trigger an
    // unconditional assertion if any use pratically traverses this
    // path such that it can be properly documented and tested.

    assert(TRUE);

    result = (CFFileDescriptorRef)_CreateMasterAddressLookup(thing, info, context, error);

	__CFHostTraceExitWithFormat("result %p\n", result);

	return result;
}
#endif /* defined(__linux__) */

/* static */ size_t
_AddressSizeForSupportedFamily(int family) {
    size_t result;

    switch (family) {

    case AF_INET:
        result = sizeof(struct sockaddr_in);
        break;

    case AF_INET6:
        result = sizeof(struct sockaddr_in6);
        break;

    default:
        result = 0;
        break;

    }

    return result;
}

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
	_CFHostLock(host);

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
                    const int family = i->ai_addr->sa_family;
					CFDataRef data   = NULL;
					CFIndex   length = 0;

					// Bypass any address families that are not understood by CFSocketStream
					if (family != AF_INET && family != AF_INET6)
						continue;

					// Wrap the address in a CFData
#if HAVE_STRUCT_SOCKADDR_SA_LEN
					length = i->ai_addr->sa_len;
#else
                    length = _AddressSizeForSupportedFamily(family);
#endif /* HAVE_STRUCT_SOCKADDR_SA_LEN */
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

        _HostLookupCancel_NoLock(host);
	}

	// Unlock the host so the callback can be made safely.
	_CFHostUnlock(host);

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

#if defined(__MACH__) || (HAVE_GETADDRINFO_A && 0)
/* static */ void
_GetAddrInfoCallBack(int eai_status, const struct addrinfo* res, void* ctxt) {
    _GetAddrInfoCallBackWithFree(eai_status, res, ctxt, freeaddrinfo);
}
#endif /* defined(__MACH__) || (HAVE_GETADDRINFO_A && 0) */

#if defined(__MACH__)
/* static */ void
_GetAddrInfoMachPortCallBack(CFMachPortRef port, void* msg, CFIndex size, void* info) {

	getaddrinfo_async_handle_reply(msg);
}
#endif /* defined(__MACH__) */

/* static */ void
_GetNameInfoCallBackWithFree_NoLock(int eai_status, char *hostname, char *serv, _CFHost * host, CFHostClientCallBack *cb, void **info, CFStreamError *error) {
    __Require(hostname != NULL, done);
    __Require(host != NULL, done);
    __Require(cb != NULL, done);
    __Require(info != NULL, done);
    __Require(error != NULL, done);

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
		*cb = host->_callback;

		// Save the error and client information for the callback
		memmove(error, &(host->_error), sizeof(*error));
		*info = host->_client.info;

        _HostLookupCancel_NoLock(host);
	}

 done:
    return;
}

/* static */ void
_GetNameInfoCallBackWithFreeAndWithShouldLock(int eai_status, char *hostname, char *serv, void* ctxt, FreeNameInfoCallBack freenameinfo_cb, Boolean should_lock) {
	_CFHost *            host = (_CFHost*)ctxt;
	CFHostClientCallBack cb   = NULL;
	void *               info = NULL;
    CFStreamError        error;

	// Retain here to guarantee safety really after the lookups release,
	// but definitely before the callback.
	CFRetain((CFHostRef)host);

	// Lock the host, if requested.

    if (should_lock) {
        _CFHostLock(host);
    }

    _GetNameInfoCallBackWithFree_NoLock(eai_status,
                                        hostname,
                                        serv,
                                        ctxt,
                                        &cb,
                                        &info,
                                        &error);

	// Unlock the host, if previously-requested to be locked, so the
	// callback can be made safely.

    if (should_lock) {
        _CFHostUnlock(host);
    }

	// Release the results if there were any.
    if (freenameinfo_cb) {
        freenameinfo_cb(hostname, serv);
    }

    // Conversely, if no locking was requested, then the host is
    // already locked. Unlock it before the call out to the client
    // which may call back into public API functions which WILL lock
    // and, as a result, WILL deadlock if we call out with the host
    // locked.

    if (!should_lock) {
        _CFHostUnlock(host);
    }

	// If there is a callback, inform the client of the finish.
	if (cb)
		cb((CFHostRef)host, kCFHostNames, &error, info);

    // Restore the host lock state, as appropriate and requested.

    if (!should_lock) {
        _CFHostLock(host);
    }

	// Go ahead and release now that the callback is done.
	CFRelease((CFHostRef)host);
}

#if defined(__MACH__)
/* static */ void
_FreeNameInfoCallBack_Mach(char *hostname, char *serv) {
    if (hostname) free(hostname);
    if (serv) free (serv);
}

/* static */ void
_GetNameInfoCallBack(int eai_status, char *hostname, char *serv, void* ctxt) {
    static const Boolean should_lock = TRUE;
    FreeNameInfoCallBack free_cb     = _FreeNameInfoCallBack_Mach;

    _GetNameInfoCallBackWithFreeAndWithShouldLock(eai_status, hostname, serv, ctxt, free_cb, should_lock);
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
	_CFHostLock(host);

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

        _HostLookupCancel_NoLock(host);
	}

	// Unlock the host so the callback can be made safely.
	_CFHostUnlock(host);

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
	_CFHostLock(host);

	// If the lookup canceled, don't need to do any of this.
	if (host->_lookup) {

		// Save the callback if there is one at this time.
		cb = host->_callback;

		// Save the error and client information for the callback
		memmove(&error, &(host->_error), sizeof(error));
		info = host->_client.info;

        _HostLookupCancel_NoLock(host);
	}

	// Unlock the host so the callback can be made safely.
	_CFHostUnlock(host);

	// If there is a callback, inform the client of the finish.
	if (cb)
		cb((CFHostRef)host, kCFHostReachability, &error, info);

	// Go ahead and release now that the callback is done.
	CFRelease((CFHostRef)host);
}
#endif /* defined(__MACH__) */

#if defined(__MACH__)
/* static */ void
_DNSCallBack_Mach(int32_t status, char *buf, uint32_t len, struct sockaddr *from, int fromlen, void *context) {

	_CFHost* host = (_CFHost*)context;
	CFHostClientCallBack cb = NULL;
	CFStreamError error;
	void* info = NULL;
	CFHostInfoType type = _kCFNullHostInfoType;

	// Retain here to guarantee safety really after the lookups release,
	// but definitely before the callback.
	CFRetain((CFHostRef)context);

	// Lock the host
	_CFHostLock(host);

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

        _HostLookupCancel_NoLock(host);
	}

	// Unlock the host so the callback can be made safely.
	_CFHostUnlock(host);

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
_PrimaryLookupCallBack(CFHostRef theHost, CFHostInfoType typeInfo, const CFStreamError *error, CFStringRef name) {

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

			_CFHostLock(client);

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

			_CFHostUnlock(client);
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
	_CFHostLock(host);

	// Save the callback if there is one at this time.
	cb = host->_callback;

	// Save the error and client information for the callback
	memmove(&error, &(host->_error), sizeof(error));
	info = host->_client.info;

    _HostLookupCancel_NoLock(host);

	// Unlock the host so the callback can be made safely.
	_CFHostUnlock(host);

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
#if (HAVE_ARES_INIT && 1)
    _CFDoOnce(&_kCFHostInitializeAres, _CFHostInitializeAres);
#endif /* (HAVE_ARES_INIT && 1) */
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
		_CFHostLock(host);

		// Just make a copy of all the information
		result->_info = CFDictionaryCreateMutableCopy(allocator, 0, host->_info);

		// Let the original go
		_CFHostUnlock(host);

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
	_CFHostLock(host);

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
			_CFHostUnlock(host);

			// Wait for synchronous return
			result = _HostBlockUntilComplete(host);

			// Lock down the host to grab the info
			_CFHostLock(host);
		}

	} while (0);

	// Copy the error.
	memmove(error, &host->_error, sizeof(error[0]));

	// Unlock the host
	_CFHostUnlock(host);

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
	_CFHostLock(host);

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
    _CFHostUnlock(host);

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
	_CFHostLock(host);

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
	_CFHostUnlock(host);
}


/* extern */ Boolean
CFHostSetClient(CFHostRef theHost, CFHostClientCallBack clientCB, CFHostClientContext* clientContext) {

	_CFHost* host = (_CFHost*)theHost;

	// Lock down the host
	_CFHostLock(host);

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
	_CFHostUnlock(host);

	return TRUE;
}


/* extern */ void
CFHostScheduleWithRunLoop(CFHostRef theHost, CFRunLoopRef runLoop, CFStringRef runLoopMode) {

	_CFHost* host = (_CFHost*)theHost;

	/* Lock down the host before work */
	_CFHostLock(host);

	/* Try adding the schedule to the list.  If it's added, need to do more work. */
	if (_SchedulesAddRunLoopAndMode(host->_schedules, runLoop, runLoopMode)) {

		/* If there is a current lookup, need to schedule it. */
		if (host->_lookup) {
			_CFTypeScheduleOnRunLoop(host->_lookup, runLoop, runLoopMode);
		}
	}

	/* Unlock the host */
	_CFHostUnlock(host);
}


/* extern */ void
CFHostUnscheduleFromRunLoop(CFHostRef theHost, CFRunLoopRef runLoop, CFStringRef runLoopMode) {

	_CFHost* host = (_CFHost*)theHost;

	/* Lock down the host before work */
	_CFHostLock(host);

	/* Try to remove the schedule from the list.  If it is removed, need to do more. */
	if (_SchedulesRemoveRunLoopAndMode(host->_schedules, runLoop, runLoopMode)) {

		/* If there is a current lookup, need to unschedule it. */
		if (host->_lookup) {
			_CFTypeUnscheduleFromRunLoop(host->_lookup, runLoop, runLoopMode);
		}
	}

	/* Unlock the host */
	_CFHostUnlock(host);
}

