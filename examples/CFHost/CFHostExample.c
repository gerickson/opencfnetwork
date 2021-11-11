/*
 *   Copyright (c) 2021 OpenCFNetwork Authors. All Rights Reserved.
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

/**
 *   @file
 *     This file implements a "toy" demonstration of the CFNetwork
 *     CFHost object by running both synchronous (that is, blocking)
 *     and asynchronous (that is, non-blocking, run loop-based)
 *     name-to-address (kCFHostAddresses) and address-to-name
 *     (kCFHostNames) lookups of "localhost", "127.0.0.1", and "::1".
 *
 */

#include <arpa/inet.h>

#include <sys/param.h>
#include <sys/socket.h>

#include <AssertMacros.h>

#include <CFNetwork/CFNetwork.h>
#include <CoreFoundation/CoreFoundation.h>

// NOTE: At present, synchronous lookups on Darwin platforms are
// intrinsically broken, returning either an
// kCFStreamErrorDomainNetDB:NETBD_INTERNAL or
// kCFStreamErrorDomainNetDB:EAI_FAIL error. Ostensibly, this could be
// made to work on Linux with c-ares; however, it is unclear whether
// workalike behavior should be failing as things do on Darwin or
// working correctly and fixing the Darwin behavior here, even though
// that would be inconsistent with shipping Darwin platforms from
// Apple on official builds of CFNetwork.
//
// As a result of this, DEMONSTRATE_CFHOST_SYNC is zero (0) until this
// is resolved one way or another.

#define DEMONSTRATE_CFHOST_SYNC       0
#define DEMONSTRATE_CFHOST_ASYNC      1

#define DEMONSTRATE_CFHOST_ADDRESSES  1
#define DEMONSTRATE_CFHOST_NAMES      1

#define DEMONSTRATE_CFHOST_NAMES_IPV4 1
#define DEMONSTRATE_CFHOST_NAMES_IPV6 1

#define USE_LOCAL_SCOPE_LOOKUPS       1
#define USE_GLOBAL_SCOPE_LOOKUPS      1

#if !defined(LOG_CFHOSTEXAMPLE)
#define LOG_CFHOSTEXAMPLE             0
#endif

#define __CFHostExampleLog(format, ...)       do { fprintf(stderr, format, ##__VA_ARGS__); fflush(stderr); } while (0)

#if LOG_CFHOSTEXAMPLE
#define __CFHostExampleMaybeLog(format, ...)                           \
    __CFHostExampleLog(format, ##__VA_ARGS__)
#else
#define __CFHostExampleMaybeLog(format, ...)
#endif

#define __CFHostExampleMaybeTraceWithFormat(dir, name, format, ...)    \
    __CFHostExampleMaybeLog(dir " %s" format, name, ##__VA_ARGS__)
#define __CFHostExampleTraceEnterWithFormat(format, ...)               \
    __CFHostExampleMaybeTraceWithFormat("-->", __func__, " " format, ##__VA_ARGS__)
#define __CFHostExampleTraceExitWithFormat(format, ...)                \
    __CFHostExampleMaybeTraceWithFormat("<--", __func__, " " format, ##__VA_ARGS__)
#define __CFHostExampleTraceEnter()                                    \
    __CFHostExampleTraceEnterWithFormat("\n")
#define __CFHostExampleTraceExit()                                     \
    __CFHostExampleTraceExitWithFormat("\n")

// Type Declarations

typedef struct {
    Boolean       mAsync;
    CFStreamError mStreamError;
} _CFHostExampleContext;

typedef struct {
    const char *   mLookupName;
    const char *   mLookupIPv4Address;
    const char *   mLookupIPv6Address;
} _CFHostExampleLookups;

// Global Variables

#if USE_LOCAL_SCOPE_LOOKUPS
static const _CFHostExampleLookups sNameAndAddressLocalScopeLookups = {
    "localhost",
    "127.0.0.1",
    "::1"
};

static const _CFHostExampleLookups sIPv4NumericHostLocalScopeLookups = {
    "127.0.0.1",
    NULL,
    NULL
};

static const _CFHostExampleLookups sIPv6NumericHostLocalScopeLookups = {
    "::1",
    NULL,
    NULL
};
#endif // USE_LOCAL_SCOPE_LOOKUPS

#if USE_GLOBAL_SCOPE_LOOKUPS
static const _CFHostExampleLookups sGlobalScopeLookups = {
    "dns.google",
    "8.8.8.8",
    "2001:4860:4860::8888"
};
#endif // USE_GLOBAL_SCOPE_LOOKUPS

static const _CFHostExampleLookups * sLookups[] = {
#if USE_LOCAL_SCOPE_LOOKUPS
    &sNameAndAddressLocalScopeLookups,
    &sIPv4NumericHostLocalScopeLookups,
    &sIPv6NumericHostLocalScopeLookups,
#endif // USE_LOCAL_SCOPE_LOOKUPS
#if USE_GLOBAL_SCOPE_LOOKUPS
    &sGlobalScopeLookups,
#endif // USE_GLOBAL_SCOPE_LOOKUPS
    NULL
};

#if (!USE_LOCAL_SCOPE_LOOKUPS && !USE_GLOBAL_SCOPE_LOOKUPS)
#error "Choose one or both of USE_LOCAL_SCOPE_LOOKUPS or USE_GLOBAL_SCOPE_LOOKUPS."
#endif // (!USE_LOCAL_SCOPE_LOOKUPS && !USE_GLOBAL_SCOPE_LOOKUPS)

static void
LogHostExampleError(const CFStreamError *aError)
{
    __CFHostExampleLog("Resolution failed with stream error %ld.%d",
                       aError->domain, aError->error);

    if (aError->domain == kCFStreamErrorDomainPOSIX)
    {
        __CFHostExampleLog(": %s", strerror(aError->error));
    }

    __CFHostExampleLog("\n");
}


static void
LogResolutionStatus(Boolean aResolved, const char *aWhat)
{
	__CFHostExampleLog("    %sesolved %s:\n", aResolved ? "R" : "Unr", aWhat);
}

static void
LogResult(CFIndex aIndex, const char *aResult)
{
    __CFHostExampleLog("        %lu: %s\n",
                       aIndex,
                       (aResult != NULL) ? aResult : "<\?\?\?>");
}

static void
GetAndLogAddresses(CFHostRef aHost, Boolean aAsync)
{
    Boolean    resolved;
    CFArrayRef addresses = NULL;

    addresses = CFHostGetAddressing(aHost, &resolved);


    if (addresses != NULL) {
        const CFIndex count = CFArrayGetCount(addresses);
        CFIndex       i;

        if (count > 0) {
            LogResolutionStatus(resolved, "addresses");

            for (i = 0; i < count; i++) {
                CFDataRef data = CFArrayGetValueAtIndex(addresses, i);

                if (data != NULL) {
                    const UInt8 * p   = CFDataGetBytePtr(data);
                    const CFIndex len = CFDataGetLength(data);
                    void *        addr;
                    socklen_t     addrlen;

                    if ((p != NULL) && (len > 0)) {
                        const int    family = ((struct sockaddr *)(p))->sa_family;
                        const size_t buflen = INET6_ADDRSTRLEN;
                        char         buffer[INET6_ADDRSTRLEN];
                        const char * result;

                        switch (family) {

                        case AF_INET:
                            addr = &(((struct sockaddr_in *)(p))->sin_addr);
                            addrlen = sizeof(struct in_addr);
                            break;

                        case AF_INET6:
                            addr = &(((struct sockaddr_in6 *)(p))->sin6_addr);
                            addrlen = sizeof(struct in6_addr);
                            break;

                        default:
                            addr = NULL;
                            addrlen = 0;
                            break;

                        }

                        if ((addr != NULL) && (addrlen > 0)) {
                            result = inet_ntop(family, addr, buffer, buflen);

                            LogResult(i, result);
                        }
                    }
                }
            }
        }
    }

    // There are two hallmarks of a synchronous versus asynchronous
    // lookup: starting and stopping the run loop is one of
    // them, setting a client callback is the other.
    //
    // If the operation is asynchronous, stop the previously-started
    // run loop.

    if (aAsync && resolved) {
        CFRunLoopStop(CFRunLoopGetCurrent());
    }
}

static void
GetAndLogNames(CFHostRef aHost, Boolean aAsync)
{
    Boolean    resolved;
    CFArrayRef names = NULL;

    names = CFHostGetNames(aHost, &resolved);

    if (names != NULL) {
		const CFIndex count = CFArrayGetCount(names);
		CFIndex       i;

        if (count > 0) {
			LogResolutionStatus(resolved, "names");

			for (i = 0; i < count; i++) {
				const CFIndex buflen = MAXHOSTNAMELEN;
				char          buffer[MAXHOSTNAMELEN];
				CFStringRef   string    = CFArrayGetValueAtIndex(names, i);
				Boolean       converted = CFStringGetCString(string, buffer, buflen, kCFStringEncodingASCII);

                LogResult(i, converted ? buffer : NULL);
			}
        }
    }

    // There are two hallmarks of a synchronous versus asynchronous
    // lookup: starting and stopping the run loop is one of
    // them, setting a client callback is the other.
    //
    // If the operation is asynchronous, stop the previously-started
    // run loop.

    if (aAsync && resolved) {
        CFRunLoopStop(CFRunLoopGetCurrent());
    }
}

static void
GetAndLogAddressesAndNames(CFHostRef aHost, Boolean aAsync)
{
    GetAndLogAddresses(aHost, aAsync);
    GetAndLogNames(aHost, aAsync);
}

static void
HostCallBack(CFHostRef aHost, CFHostInfoType aInfo, const CFStreamError *aError, void * aContext)
{
    _CFHostExampleContext *lContext = ((_CFHostExampleContext *)(aContext));

    if (aError->error == 0) {
        if (aInfo == kCFHostAddresses) {
            GetAndLogAddresses(aHost, lContext->mAsync);

        } else if (aInfo == kCFHostNames) {
            GetAndLogNames(aHost, lContext->mAsync);

        }
    } else {
        LogHostExampleError(aError);

        if (&lContext->mStreamError != aError)
        {
            lContext->mStreamError.error = aError->error;
            lContext->mStreamError.domain = aError->domain;
        }

        if (lContext->mAsync) {
            CFRunLoopStop(CFRunLoopGetCurrent());
        }
    }

    CFHostCancelInfoResolution(aHost, aInfo);
}

static Boolean
StartResolution(CFHostRef aHost, CFHostInfoType aInfo, _CFHostExampleContext *aContext)
{
    Boolean       result;

    // There are two hallmarks of a synchronous versus asynchronous
    // lookup: starting and stopping the run loop is one of
    // them, setting a client callback is the other.
    //
    // If the operation is asynchronous, schedule the host for run
    // loop operation.

    if (aContext->mAsync) {
        CFHostScheduleWithRunLoop(aHost, CFRunLoopGetCurrent(), kCFRunLoopDefaultMode);
    }

    result = CFHostStartInfoResolution(aHost, aInfo, &aContext->mStreamError);
    __Require(result, done);

    // There are two hallmarks of a synchronous versus asynchronous
    // lookup: starting and stopping the run loop is one of
    // them, setting a client callback is the other.
    //
    // If the operation is asynchronous, start the run loop.

    if (aContext->mAsync) {
        CFRunLoopRun();
    }

 done:
    // There are two hallmarks of a synchronous versus asynchronous
    // lookup: starting and stopping the run loop is one of
    // them, setting a client callback is the other.
    //
    // If the operation is asynchronous, unschedule the host from run
    // loop operation.

    if (aContext->mAsync) {
        CFHostUnscheduleFromRunLoop(aHost, CFRunLoopGetCurrent(), kCFRunLoopDefaultMode);
    }

    return (result);
}

static int
DemonstrateHostCommon(CFHostRef aHost, CFHostInfoType aInfo, _CFHostExampleContext *aContext)
{
    CFTypeID            type;
    CFHostClientContext context = { 0, aContext, NULL, NULL, NULL };
    Boolean             set;
    Boolean             started;
    CFStreamError       error  = { 0, 0 };
    int                 status = -1;

    type = CFGetTypeID(aHost);
    __Require_Action(type == CFHostGetTypeID(), done, status = -EINVAL);

    // There are two hallmarks of a synchronous versus asynchronous
    // lookup: setting a client callback is one of them, starting and
    // stopping the run loop is the other.
    //
    // If the operation is asynchronous, set the asynchronous client
    // callback.

    if (aContext->mAsync == TRUE) {
        set = CFHostSetClient(aHost, HostCallBack, &context);
        __Require_Action(set, done, status = -1);
    }

    GetAndLogAddressesAndNames(aHost, FALSE);

    started = StartResolution(aHost, aInfo, aContext);
    __Require_Action(started, done, status = -1);

    status = 0;

 done:
    // There are two hallmarks of a synchronous versus asynchronous
    // lookup: setting a client callback is one of them, starting and
    // stopping the run loop is the other.
    //
    // If the operation is asynchronous, clear the asynchronous client
    // callback.

    if (aContext->mAsync == TRUE) {
        set = CFHostSetClient(aHost, NULL, NULL);
        __Require_Action(set, done, status = -1);
    }

    if (status != 0) {
        if (error.error != 0) {
            LogHostExampleError(&error);
        }
    }

    return (status);
}

#if DEMONSTRATE_CFHOST_ADDRESSES
static int
DemonstrateHostByName(const char *name, _CFHostExampleContext *aContext)
{
    CFStringRef         string = NULL;
    CFHostRef           host = NULL;
    int                 status = -1;

    __CFHostExampleLog("By name '%s' (Forward DNS)...\n", name);

    string = CFStringCreateWithCString(kCFAllocatorDefault,
                                       name,
                                       kCFStringEncodingUTF8);
    __Require_Action(string != NULL, done, status = -ENOMEM);

    host = CFHostCreateWithName(kCFAllocatorDefault, string);
    __Require_Action(host != NULL, done, status = -ENOMEM);

    status = DemonstrateHostCommon(host, kCFHostAddresses, aContext);
    __Require(status == 0, done);

 done:
    if (host != NULL) {
        CFRelease(host);
    }

    return (status);
}
#endif // DEMONSTRATE_CFHOST_ADDRESSES

#if DEMONSTRATE_CFHOST_NAMES
static int
DemonstrateHostByAddress(const char *addressString, struct sockaddr *address, size_t length, _CFHostExampleContext *aContext)
{
    const int           family = address->sa_family;
    void *              ia;
    CFDataRef           addressData = NULL;
    CFHostRef           host = NULL;
    int                 status = -1;

    __CFHostExampleLog("By IPv%c address '%s' (Reverse DNS)...\n",
                       ((family == AF_INET) ? '4' : '6'),
                       addressString);

    // Note that while CFHostCreateWithAddress takes CFData wrapping a sockaddr,
    // inet_pton takes in_addr or in6_addr. Adjust by finding the appropriate
    // pointer within 'address' to pass to inet_pton.

    switch (family) {

    case AF_INET:
        ia = &((struct sockaddr_in *)(address))->sin_addr;
        break;

    case AF_INET6:
        ia = &((struct sockaddr_in6 *)(address))->sin6_addr;
        break;

    default:
        ia = NULL;
        break;

    }

    status = inet_pton(family, addressString, ia);
    __Require_Action(status == 1, done, status = -1);

    addressData = CFDataCreate(kCFAllocatorDefault,
                               (const UInt8 *)address,
                               length);
    __Require_Action(addressData != NULL, done, status = -ENOMEM);

    host = CFHostCreateWithAddress(kCFAllocatorDefault, addressData);
    __Require_Action(host != NULL, done, status = -ENOMEM);

    status = DemonstrateHostCommon(host, kCFHostNames, aContext);
    __Require(status == 0, done);

 done:
    if (host != NULL) {
        CFRelease(host);
    }

    if (addressData != NULL) {
        CFRelease(addressData);
    }

    return (status);
}

#if DEMONSTRATE_CFHOST_NAMES_IPV4
static int
DemonstrateHostByAddressIPv4(const char *aAddressString, _CFHostExampleContext *aContext)
{
    struct sockaddr_in  address;
    int                 status = -1;

    memset(&address, 0, sizeof (struct sockaddr_in));

    address.sin_family = AF_INET;

    status = DemonstrateHostByAddress(aAddressString,
                                      (struct sockaddr *)&address,
                                      sizeof (struct sockaddr_in),
                                      aContext);

    return (status);
}
#endif // DEMONSTRATE_CFHOST_NAMES_IPV4

#if DEMONSTRATE_CFHOST_NAMES_IPV6
static int
DemonstrateHostByAddressIPv6(const char *aAddressString, _CFHostExampleContext *aContext)
{
    struct sockaddr_in6 address;
    int                 status = -1;

    memset(&address, 0, sizeof (struct sockaddr_in6));

    address.sin6_family = AF_INET6;

    status = DemonstrateHostByAddress(aAddressString,
                                      (struct sockaddr *)&address,
                                      sizeof (struct sockaddr_in6),
                                      aContext);

    return (status);
}
#endif // DEMONSTRATE_CFHOST_NAMES_IPV6
#endif // DEMONSTRATE_CFHOST_NAMES

static int
DemonstrateHost(const _CFHostExampleLookups *lookups, _CFHostExampleContext *aContext)
{
    int result;

    __CFHostExampleLog("%synchronous lookups...\n", aContext->mAsync ? "As" : "S");

#if DEMONSTRATE_CFHOST_ADDRESSES
    if (lookups->mLookupName != NULL)
    {
        result = DemonstrateHostByName(lookups->mLookupName, aContext);
        __Require(result == 0, done);
    }
#endif

#if DEMONSTRATE_CFHOST_NAMES
#if DEMONSTRATE_CFHOST_NAMES_IPV4
    if (lookups->mLookupIPv4Address != NULL)
    {
        result = DemonstrateHostByAddressIPv4(lookups->mLookupIPv4Address, aContext);
        __Require(result == 0, done);
    }
#endif // DEMONSTRATE_CFHOST_NAMES_IPV4

#if DEMONSTRATE_CFHOST_NAMES_IPV6
    if (lookups->mLookupIPv6Address != NULL)
    {
        result = DemonstrateHostByAddressIPv6(lookups->mLookupIPv6Address, aContext);
        __Require(result == 0, done);
    }
#endif // DEMONSTRATE_CFHOST_NAMES_IPV6
#endif // DEMONSTRATE_CFHOST_NAMES

 done:
    return (result);
}

static const _CFHostExampleLookups **
GetLookups(void)
{
    return (&sLookups[0]);
}

int
main(void)
{
    const _CFHostExampleLookups **lookups = NULL;
    _CFHostExampleContext context = { FALSE, { 0, 0 } };
    int     status;

    lookups = GetLookups();

    while ((lookups != NULL) && (*lookups != NULL))
    {
#if DEMONSTRATE_CFHOST_SYNC
        // Synchronous (blocking)

        context.mAsync = FALSE;

        status = DemonstrateHost(*lookups, &context);
        __Require(status == 0, done);

        if (context.mStreamError.error != 0)
        {
            status = -1;
            break;
        }
#endif // DEMONSTRATE_CFHOST_SYNC

#if DEMONSTRATE_CFHOST_ASYNC
        // Asynchronous (non-blocking)

        context.mAsync = TRUE;

        status = DemonstrateHost(*lookups, &context);
        __Require(status == 0, done);

        if (context.mStreamError.error != 0)
        {
            status = -1;
            break;
        }
#endif // DEMONSTRATE_CFHOST_ASYNC

        lookups++;
    }

 done:
    return ((status == 0) ? EXIT_SUCCESS : EXIT_FAILURE);
}
