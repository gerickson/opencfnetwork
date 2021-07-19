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

#define USE_LOCAL_SCOPE_LOOKUPS      1
#define USE_GLOBAL_SCOPE_LOOKUPS     !USE_LOCAL_SCOPE_LOOKUPS

#if !defined(LOG_CFHOSTEXAMPLE)
#define LOG_CFHOSTEXAMPLE            0
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

typedef struct {
    const char * mLookupName;
    const char * mLookupIPv4Address;
    const char * mLookupIPv6Address;
} _CFHostExampleLookups;

#if USE_LOCAL_SCOPE_LOOKUPS
static const _CFHostExampleLookups sLocalScopeLookups = {
    "localhost",
    "127.0.0.1",
    "::1"
};
#endif // USE_LOCAL_SCOPE_LOOKUPS

#if USE_GLOBAL_SCOPE_LOOKUPS
static const _CFHostExampleLookups sGlobalScopeLookups = {
    "dns.google",
    "8.8.8.8",
    "2001:4860:4860::8888"
};
#endif // USE_GLOBAL_SCOPE_LOOKUPS

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
    Boolean *   async = ((Boolean *)(aContext));

    if (aError->error == 0) {
        if (aInfo == kCFHostAddresses) {
            GetAndLogAddresses(aHost, *async);

        } else if (aInfo == kCFHostNames) {
            GetAndLogNames(aHost, *async);

        }
    } else {
        __CFHostExampleLog("Resolution failed with error %d.%ld\n",
                           aError->error, aError->domain);

        if (*async) {
            CFRunLoopStop(CFRunLoopGetCurrent());
        }
    }

    CFHostCancelInfoResolution(aHost, aInfo);
}

static Boolean
StartResolution(CFHostRef aHost, CFHostInfoType aInfo, CFStreamError *aError, Boolean aAsync)
{
    Boolean       result;

    // There are two hallmarks of a synchronous versus asynchronous
    // lookup: starting and stopping the run loop is one of
    // them, setting a client callback is the other.
    //
    // If the operation is asynchronous, schedule the host for run
    // loop operation.

    if (aAsync) {
        CFHostScheduleWithRunLoop(aHost, CFRunLoopGetCurrent(), kCFRunLoopDefaultMode);
    }

    result = CFHostStartInfoResolution(aHost, aInfo, aError);
    __Require(result, done);

    // There are two hallmarks of a synchronous versus asynchronous
    // lookup: starting and stopping the run loop is one of
    // them, setting a client callback is the other.
    //
    // If the operation is asynchronous, start the run loop.

    if (aAsync) {
        CFRunLoopRun();
    }

 done:
    // There are two hallmarks of a synchronous versus asynchronous
    // lookup: starting and stopping the run loop is one of
    // them, setting a client callback is the other.
    //
    // If the operation is asynchronous, unschedule the host from run
    // loop operation.

    if (aAsync) {
        CFHostUnscheduleFromRunLoop(aHost, CFRunLoopGetCurrent(), kCFRunLoopDefaultMode);
    }

    return (result);
}

static int
DemonstrateHostCommon(CFHostRef aHost, CFHostInfoType aInfo, Boolean *aAsync)
{
    CFTypeID            type;
    CFHostClientContext context = { 0, aAsync, NULL, NULL, NULL };
    Boolean             set;
    Boolean             started;
    CFStreamError       error  = { 0, 0 };
    int                 status = -1;

    type = CFGetTypeID(aHost);
    __Require_Action(type == CFHostGetTypeID(), done, status = -1);

    // There are two hallmarks of a synchronous versus asynchronous
    // lookup: setting a client callback is one of them, starting and
    // stopping the run loop is the other.
    //
    // If the operation is asynchronous, set the asynchronous client
    // callback.

    if (*aAsync == TRUE) {
        set = CFHostSetClient(aHost, HostCallBack, &context);
        __Require_Action(set, done, status = -1);
    }

    GetAndLogAddressesAndNames(aHost, FALSE);

    started = StartResolution(aHost, aInfo, &error, *aAsync);
    __Require_Action(started, done, status = -1);

    status = 0;

 done:
    // There are two hallmarks of a synchronous versus asynchronous
    // lookup: setting a client callback is one of them, starting and
    // stopping the run loop is the other.
    //
    // If the operation is asynchronous, clear the asynchronous client
    // callback.

    if (*aAsync == TRUE) {
        set = CFHostSetClient(aHost, NULL, NULL);
        __Require_Action(set, done, status = -1);
    }

    if (status != 0) {
        if (error.error != 0) {
            __CFHostExampleLog("Resolution failed with error %d.%ld\n",
                               error.error, error.domain);
        }
    }

    return (status);
}

static int
DemonstrateHostByName(const char *name, Boolean *aAsync)
{
    CFStringRef         string = NULL;
    CFHostRef           host = NULL;
    int                 status = -1;

    __CFHostExampleLog("By name '%s' (DNS)...\n", name);

    string = CFStringCreateWithCString(kCFAllocatorDefault,
                                       name,
                                       kCFStringEncodingUTF8);
    __Require_Action(string != NULL, done, status = -ENOMEM);

    host = CFHostCreateWithName(kCFAllocatorDefault, string);
    __Require_Action(host != NULL, done, status = -ENOMEM);

    status = DemonstrateHostCommon(host, kCFHostAddresses, aAsync);
    __Require(status == 0, done);

 done:
    if (host != NULL) {
        CFRelease(host);
    }

    return (status);
}

static int
DemonstrateHostByAddress(const char *addressString, struct sockaddr *address, size_t length, Boolean *aAsync)
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
    __Require_Action(addressData != NULL, done, status = -1);

    host = CFHostCreateWithAddress(kCFAllocatorDefault, addressData);
    __Require_Action(host != NULL, done, status = -1);

    status = DemonstrateHostCommon(host, kCFHostNames, aAsync);
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

static int
DemonstrateHostByAddressIPv4(const char *aAddressString, Boolean *aAsync)
{
    struct sockaddr_in  address;
    int                 status = -1;

    memset(&address, 0, sizeof (struct sockaddr_in));

    address.sin_family = AF_INET;

    status = DemonstrateHostByAddress(aAddressString,
                                      (struct sockaddr *)&address,
                                      sizeof (struct sockaddr_in),
                                      aAsync);

    return (status);
}

static int
DemonstrateHostByAddressIPv6(const char *aAddressString, Boolean *aAsync)
{
    struct sockaddr_in6 address;
    int                 status = -1;

    memset(&address, 0, sizeof (struct sockaddr_in6));

    address.sin6_family = AF_INET6;

    status = DemonstrateHostByAddress(aAddressString,
                                      (struct sockaddr *)&address,
                                      sizeof (struct sockaddr_in6),
                                      aAsync);

    return (status);
}

static int
DemonstrateHost(const _CFHostExampleLookups *lookups, Boolean *aAsync)
{
    int result;

    __CFHostExampleLog("%synchronous lookups...\n", *aAsync ? "As" : "S");

    result = DemonstrateHostByName(lookups->mLookupName, aAsync);
    __Require(result == 0, done);

    result = DemonstrateHostByAddressIPv4(lookups->mLookupIPv4Address, aAsync);
    __Require(result == 0, done);

    result = DemonstrateHostByAddressIPv6(lookups->mLookupIPv6Address, aAsync);
    __Require(result == 0, done);

 done:
    return (result);
}

static const _CFHostExampleLookups *
GetLookups(void)
{
    const _CFHostExampleLookups * lookups;

#if USE_LOCAL_SCOPE_LOOKUPS
    lookups = &sLocalScopeLookups;
#elif USE_GLOBAL_SCOPE_LOOKUPS
    lookups = &sGlobalScopeLookups;
#else
#error "Choose one of USE_LOCAL_SCOPE_LOOKUPS or USE_GLOBAL_SCOPE_LOOKUPS."
#endif

    return (lookups);
}

int
main(void)
{
    Boolean async;
    int     status;

    // Synchronous (blocking)

    async = FALSE;

    status = DemonstrateHost(GetLookups(), &async);
    __Require(status == 0, done);

    // Asynchronous (non-blocking)

    async = TRUE;

    status = DemonstrateHost(GetLookups(), &async);
    __Require(status == 0, done);

 done:
    return ((status == 0) ? EXIT_SUCCESS : EXIT_FAILURE);
}
