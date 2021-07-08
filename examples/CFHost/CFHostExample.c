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

#include <AssertMacros.h>

#include <CFNetwork/CFNetwork.h>
#include <CoreFoundation/CoreFoundation.h>

#if !defined(LOG_CFHOST)
#define LOG_CFHOST 0
#endif

#define __CFHostExampleLog(format, ...)       do { fprintf(stderr, format, ##__VA_ARGS__); fflush(stderr); } while (0)

#if LOG_CFHOST
#define __CFHostExampleMaybeLog(format, ...)                           \
    __CFHostExampleLog(format, ##__VA_ARGS__)
#else
#define __CFHostExampleMaybeLog(format, ...)
#endif

#define __CFHostExampleMaybeTrace(dir, name)                           \
    __CFHostExampleMaybeLog(dir " %s\n", name)
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

static void
GetAddresses(CFHostRef aHost, Boolean aAsync)
{
    Boolean    resolved;
    CFArrayRef addresses = NULL;
    CFIndex    count;

    addresses = CFHostGetAddressing(aHost, &resolved);


    if (addresses != NULL) {
        count = CFArrayGetCount(addresses);

        if (count > 0) {
            __CFHostExampleLog("%sesolved addresses:\n", resolved ? "R" : "Unr");
            CFShow(addresses);
        }
    }

    if (aAsync && resolved) {
        CFRunLoopStop(CFRunLoopGetCurrent());
    }
}

static void
GetNames(CFHostRef aHost, Boolean aAsync)
{
    Boolean    resolved;
    CFArrayRef names = NULL;
    CFIndex    count;

    names = CFHostGetNames(aHost, &resolved);

    if (names != NULL) {
        count = CFArrayGetCount(names);

        if (count > 0) {
            __CFHostExampleLog("%sesolved names:\n", resolved ? "R" : "Unr");
            CFShow(names);
        }
    }

    if (aAsync && resolved) {
        CFRunLoopStop(CFRunLoopGetCurrent());
    }
}

static void
GetAddressesAndNames(CFHostRef aHost, Boolean aAsync)
{
    GetAddresses(aHost, aAsync);
    GetNames(aHost, aAsync);
}

static void
HostCallBack(CFHostRef aHost, CFHostInfoType aInfo, const CFStreamError *aError, void * aContext)
{
    Boolean *   async = ((Boolean *)(aContext));

    if (aInfo == kCFHostAddresses) {
        GetAddresses(aHost, *async);

    } else if (aInfo == kCFHostNames) {
        GetNames(aHost, *async);

    }

    CFHostCancelInfoResolution(aHost, aInfo);
}

static Boolean
StartResolution(CFHostRef aHost, CFHostInfoType aInfo, Boolean aAsync)
{
    CFStreamError error;
    Boolean       result;

    if (aAsync) {
        CFHostScheduleWithRunLoop(aHost, CFRunLoopGetCurrent(), kCFRunLoopDefaultMode);
    }

    result = CFHostStartInfoResolution(aHost, aInfo, &error);
    __Require(result, done);

    if (aAsync) {
        CFRunLoopRun();
    }

 done:
    if (aAsync) {
        CFHostUnscheduleFromRunLoop(aHost, CFRunLoopGetCurrent(), kCFRunLoopDefaultMode);
    }

    return (result);
}

static int
DemonstrateHostByName(Boolean *aAsync)
{
    CFStringRef         name = CFSTR("localhost");
    CFHostRef           host = NULL;
    CFTypeID            type;
    CFHostClientContext context = { 0, aAsync, NULL, NULL, NULL };
    Boolean             set;
    Boolean             started;
    int                 status = -1;

    host = CFHostCreateWithName(kCFAllocatorDefault, name);
    __Require_Action(host != NULL, exit, status = -1);

    type = CFGetTypeID(host);
    __Require_Action(type == CFHostGetTypeID(), exit, status = -1);

    set = CFHostSetClient(host, HostCallBack, &context);
    __Require_Action(set, exit, status = -1);

    GetAddressesAndNames(host, FALSE);

    started = StartResolution(host, kCFHostAddresses, *aAsync);
    __Require_Action(started, done, status = -1);

 done:
    set = CFHostSetClient(host, NULL, NULL);
    __Require_Action(set, exit, status = -1);

    status = 0;

 exit:
    if (host != NULL) {
        CFRelease(host);
    }

    return (status);
}

static int
DemonstrateHostByAddress(const char *addressString, struct sockaddr *address, size_t length, Boolean *aAsync)
{
    CFDataRef           addressData = NULL;
    CFHostRef           host = NULL;
    CFTypeID            type;
    CFHostClientContext context = { 0, aAsync, NULL, NULL, NULL };
    Boolean             set;
    Boolean             started;
    int                 status = -1;

    status = inet_pton(address->sa_family, addressString, address);
    __Require_Action(status == 1, exit, status = -1);

    addressData = CFDataCreate(kCFAllocatorDefault,
                               (const UInt8 *)address,
                               length);
    __Require_Action(addressData != NULL, exit, status = -1);

    host = CFHostCreateWithAddress(kCFAllocatorDefault, addressData);
    __Require_Action(host != NULL, exit, status = -1);

    CFRelease(addressData);

    type = CFGetTypeID(host);
    __Require_Action(type == CFHostGetTypeID(), exit, status = -1);

    set = CFHostSetClient(host, HostCallBack, &context);
    __Require_Action(set, exit, status = -1);

    GetAddressesAndNames(host, FALSE);

    started = StartResolution(host, kCFHostNames, *aAsync);
    __Require_Action(started, done, status = -1);

 done:
    set = CFHostSetClient(host, NULL, NULL);
    __Require_Action(set, exit, status = -1);

    status = 0;

 exit:
    if (host != NULL) {
        CFRelease(host);
    }

    return (status);
}

static int
DemonstrateHostByAddressIPv4(Boolean *aAsync)
{
    const char * const  addressString = "127.0.0.1";
    struct sockaddr_in  address;
    int                 status = -1;

    memset(&address, 0, sizeof (struct sockaddr_in));

    address.sin_family = AF_INET;

    status = DemonstrateHostByAddress(addressString,
                                      (struct sockaddr *)&address,
                                      sizeof (struct sockaddr_in),
                                      aAsync);

    return (status);
}

static int
DemonstrateHostByAddressIPv6(Boolean *aAsync)
{
    const char * const  addressString = "::1";
    struct sockaddr_in6 address;
    int                 status = -1;

    memset(&address, 0, sizeof (struct sockaddr_in6));

    address.sin6_family = AF_INET6;

    status = DemonstrateHostByAddress(addressString,
                                      (struct sockaddr *)&address,
                                      sizeof (struct sockaddr_in6),
                                      aAsync);

    return (status);
}

static int
DemonstrateHost(Boolean *aAsync)
{
    int result;

    __CFHostExampleLog("By name (DNS)...\n");

    result = DemonstrateHostByName(aAsync);
    __Require(result == 0, done);

    __CFHostExampleLog("By IPv4 address (Reverse DNS)...\n");

    result = DemonstrateHostByAddressIPv4(aAsync);
    __Require(result == 0, done);

    __CFHostExampleLog("By IPv6 address (Reverse DNS)...\n");

    result = DemonstrateHostByAddressIPv6(aAsync);
    __Require(result == 0, done);

 done:
    return (result);
}

int
main(void)
{
    Boolean async;
    int     status;

    // Synchronous (blocking)

    __CFHostExampleLog("Synchronous lookups...\n");

    async = FALSE;

    status = DemonstrateHost(&async);
    __Require(status == 0, done);

    // Asynchronous (non-blocking)

    __CFHostExampleLog("Asynchronous lookups...\n");

    async = TRUE;

    status = DemonstrateHost(&async);
    __Require(status == 0, done);

 done:
    return ((status == 0) ? EXIT_SUCCESS : EXIT_FAILURE);
}
