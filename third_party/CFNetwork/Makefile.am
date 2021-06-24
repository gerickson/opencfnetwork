#
#    Copyright (c) 2021 OpenCFNetwork Authors. All Rights Reserved.
#
#    This file contains Original Code and/or Modifications of Original Code
#    as defined in and that are subject to the Apple Public Source License
#    Version 2.0 (the 'License'). You may not use this file except in
#    compliance with the License. Please obtain a copy of the License at
#    http://www.opensource.apple.com/apsl/ and read it before using this
#    file.
#
#    The Original Code and all software distributed under the License are
#    distributed on an 'AS IS' basis, WITHOUT WARRANTY OF ANY KIND, EITHER
#    EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
#    INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
#    FITNESS FOR A PARTICULAR PURPOSE, QUIET ENJOYMENT OR NON-INFRINGEMENT.
#    Please see the License for the specific language governing rights and
#    limitations under the License.
#

#
#    Description:
#      This file is the GNU automake input source file for the
#      upstream CFNetwork sources.
#

include $(abs_top_nlbuild_autotools_dir)/automake/pre.am

noinst_HEADERS			=        				\
    repo/CFNetworkInternal.h                                            \
    repo/Headers/CFFTPStreamPriv.h                                      \
    repo/Headers/CFHostPriv.h                                           \
    repo/Headers/CFHTTPConnectionPriv.h                                 \
    repo/Headers/CFHTTPMessagePriv.h                                    \
    repo/Headers/CFHTTPServerPriv.h                                     \
    repo/Headers/CFHTTPStreamPriv.h                                     \
    repo/Headers/CFNetDiagnosticsPriv.h                                 \
    repo/Headers/CFNetServicesPriv.h                                    \
    repo/Headers/CFNetworkPriv.h                                        \
    repo/Headers/CFServerPriv.h                                         \
    repo/Headers/CFSocketStreamPriv.h                                   \
    repo/HTTP/CFHTTPConnectionInternal.h                                \
    repo/HTTP/CFHTTPInternal.h                                          \
    repo/HTTP/NTLM/ntlmBlobPriv.h                                       \
    repo/HTTP/NTLM/NtlmGenerator.h                                      \
    repo/HTTP/SPNEGO/spnegoBlob.h                                       \
    repo/HTTP/SPNEGO/spnegoDER.h                                        \
    repo/HTTP/SPNEGO/spnegoKrb.h                                        \
    repo/NetDiagnostics/CFNetDiagnosticsInternal.h                      \
    repo/NetDiagnostics/CFNetDiagnosticsProtocol.h                      \
    repo/NetServices/DeprecatedDNSServiceDiscovery.h                    \
    repo/Proxies/ProxySupport.h                                         \
    repo/SharedCode/CFNetConnection.h                                   \
    repo/SharedCode/CFNetworkSchedule.h                                 \
    repo/SharedCode/CFNetworkThreadSupport.h                            \
    repo/Stream/CFSocketStreamImpl.h                                    \
    $(NULL)

CFNetwork_includedir		= ${includedir}/CFNetwork

CFNetwork_include_HEADERS       =                                       \
    repo/Headers/CFFTPStream.h                                          \
    repo/Headers/CFHost.h                                               \
    repo/Headers/CFHTTPAuthentication.h                                 \
    repo/Headers/CFHTTPMessage.h                                        \
    repo/Headers/CFHTTPStream.h                                         \
    repo/Headers/CFNetDiagnostics.h                                     \
    repo/Headers/CFNetServices.h                                        \
    repo/Headers/CFNetworkDefs.h                                        \
    repo/Headers/CFNetwork.h                                            \
    repo/Headers/CFSocketStream.h                                       \
    $(NULL)

lib_LTLIBRARIES                 = libCFNetwork.la

libCFNetwork_la_CPPFLAGS        = -I${top_srcdir}/third_party/CFNetwork/repo -I${top_srcdir}/third_party/CFNetwork/repo/HTTP -I${top_srcdir}/third_party/CFNetwork/repo/Proxies -I${top_srcdir}/third_party/CFNetwork/repo/SharedCode

libCFNetwork_la_SOURCES         =                                       \
    repo/CFNetwork.c                                                    \
    repo/libresolv.c                                                    \
    repo/JavaScriptGlue.c                                               \
    repo/FTP/CFFTPStream.c                                              \
    repo/Host/CFHost.c                                                  \
    repo/HTTP/CFHTTPAuthentication.c                                    \
    repo/HTTP/CFHTTPConnection.c                                        \
    repo/HTTP/CFHTTPFilter.c                                            \
    repo/HTTP/CFHTTPMessage.c                                           \
    repo/HTTP/CFHTTPServer.c                                            \
    repo/HTTP/CFHTTPStream.c                                            \
    repo/HTTP/SPNEGO/spnegoBlob.cpp                                     \
    repo/HTTP/SPNEGO/spnegoDER.cpp                                      \
    repo/HTTP/SPNEGO/spnegoKrb.cpp                                      \
    repo/HTTP/NTLM/ntlmBlobPriv.cpp                                     \
    repo/HTTP/NTLM/NtlmGenerator.cpp                                    \
    repo/NetDiagnostics/CFNetDiagnosticPing.c                           \
    repo/NetDiagnostics/CFNetDiagnostics.c                              \
    repo/NetDiagnostics/CFNetDiagnosticsProtocolUser.c                  \
    repo/NetServices/CFNetServiceBrowser.c                              \
    repo/NetServices/CFNetServiceMonitor.c                              \
    repo/NetServices/CFNetServices.c                                    \
    repo/NetServices/DeprecatedDNSServiceDiscovery.c                    \
    repo/Proxies/ProxySupport.c                                         \
    repo/SharedCode/CFNetConnection.c                                   \
    repo/SharedCode/CFNetworkSchedule.c                                 \
    repo/SharedCode/CFNetworkThreadSupport.c                            \
    repo/SharedCode/CFServer.c                                          \
    repo/Stream/CFSocketStream.c                                        \
    repo/URL/_CFURLAccess.c                                             \
    $(NULL)

install-headers: install-CFNetwork_includeHEADERS

# A hook for the 'uninstall' target to ensure that directories this
# package creates or that this package populates during the 'install'
# target are removed, if empty (that is, this package is the "last one
# out").

uninstall-hook:
	-rmdir ${DESTDIR}${CFNetwork_includedir}

include $(abs_top_nlbuild_autotools_dir)/automake/post.am
