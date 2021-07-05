#
#    Copyright (c) 2021 OpenCFNetwork Authors. All Rights Reserved.
#    Copyright (c) 2018 Google LLC. All Rights Reserved.
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
#      This file is the GNU automake template for OpenCFNetwork.
#

include $(abs_top_nlbuild_autotools_dir)/automake/pre.am

AM_MAKEFLAGS                                     = --no-print-directory

SUBDIRS                                          = \
    src/include                                    \
    third_party                                    \
    src                                            \
    $(NULL)

EXTRA_DIST                                       = \
    Makefile-bootstrap                             \
    .default-version                               \
    bootstrap                                      \
    bootstrap-configure                            \
    $(srcdir)/third_party/nlbuild-autotools \
    README.md                                      \
    TODO.md                                        \
    $(NULL)

BUILT_SOURCES                                    = \
    .local-version                                 \
    $(NULL)

dist_doc_DATA                                    = \
    CHANGES.md                                     \
    LICENSE                                        \
    $(NULL)

DISTCLEANFILES                                   = \
    .local-version                                 \
    $(NULL)

#
# Package version files:
#
# .default-version - The default package version. This file is ALWAYS checked
#                    in and should always represent the current baseline
#                    version of the package.
#
# .dist-version    - The distributed package version. This file is NEVER
#                    checked in within the upstream repository, is auto-
#                    generated, and is only found in the package distribution.
#
# .local-version   - The current source code controlled package version. This
#                    file is NEVER checked in within the upstream repository,
#                    is auto-generated, and can always be found in both the
#                    build tree and distribution.
#
# When present, the .local-version file is preferred first, the
# .dist-version second, and the .default-version last.
#

# VERSION_FILE should be and is intentionally an immediate (:=) rather
# than a deferred (=) variable to ensure the value binds once and only once
# for a given MAKELEVEL even as .local-version and .dist-version are created
# during makefile execution.

VERSION_FILE                      := $(if $(wildcard $(builddir)/.local-version),$(builddir)/.local-version,$(if $(wildcard $(srcdir)/.dist-version),$(srcdir)/.dist-version,$(srcdir)/.default-version))

#
# Override autotool's default notion of the package version variables.
# This ensures that when we create a source distribution the
# version is always the current version, not the package bootstrap
# version.
#
# The two-level variables and the check against MAKELEVEL ensures that
# not only can the package version be overridden from the command line
# but also when the version is NOT overridden that we bind the version
# once and only once across potential sub-makes to prevent the version
# from flapping as VERSION_FILE changes.
#

export MAYBE_OPENCFNETWORK_VERSION        := $(if $(filter 0,$(MAKELEVEL)),$(shell cat $(VERSION_FILE) 2> /dev/null),$(MAYBE_OPENCFNETWORK_VERSION))

OPENCFNETWORK_VERSION                     ?= $(MAYBE_OPENCFNETWORK_VERSION)

PACKAGE_VERSION                            = $(OPENCFNETWORK_VERSION)
VERSION                                    = $(PACKAGE_VERSION)

#
# check-file-.local-version
#
# Speculatively regenerate .local-version and check to see if it needs
# to be updated.
#
# If OPENCFNETWORK_VERSION has been supplied anywhere other than in this file
# (which is implicitly the contents of .local-version), then use that;
# otherwise, attempt to generate it from the SCM system.
#
# This is called from $(call check-file,.local-version).
#
define check-file-.local-version
if [ "$(origin OPENCFNETWORK_VERSION)" != "file" ]; then \
    echo "$(OPENCFNETWORK_VERSION)" > "$(2)";            \
else                                                     \
    $(abs_top_nlbuild_autotools_dir)/scripts/mkversion   \
        -b "$(OPENCFNETWORK_VERSION)" "$(top_srcdir)"    \
        > "$(2)";                                        \
fi
endef

#
# check-file-.dist-version
#
# Speculatively regenerate .dist-version and check to see if it needs
# to be updated.
#
# This is called from $(call check-file,.dist-version).
#
define check-file-.dist-version
cat "$(1)" > "$(2)"
endef

#
# A convenience target to allow package users to easily rerun the
# package configuration according to the current configuration.
#
.PHONY: reconfigure
reconfigure: $(builddir)/config.status
	$(AM_V_at)$(<) --recheck

#
# Version file regeneration rules.
#
.PHONY: force

$(builddir)/.local-version: $(srcdir)/.default-version force

$(distdir)/.dist-version: $(builddir)/.local-version force

$(distdir)/.dist-version $(builddir)/.local-version:
	$(call check-file,$(@F))

dist distcheck distdir install-headers: $(BUILT_SOURCES)

dist-hook: $(distdir)/.dist-version

#
# Top-level convenience target for making a documentation-only
# distribution whose results appear at the top level of the build tree
# in the same fashion that the distribution would be for 'make dist'.
#

.PHONY: docdist
docdist: $(BUILT_SOURCES)
	$(MAKE) -C doc docdistdir=$(abs_builddir) $(@)

#
# Data file for pkg-config
#

pkgconfigdir			= ${libdir}/pkgconfig
pkgconfig_DATA			= CFNetwork.pc

# A hook for the 'uninstall' target to ensure that directories this
# package creates or that this package populates during the 'install'
# target are removed, if empty (that is, this package is the "last one
# out").

uninstall-hook:
	-rmdir ${DESTDIR}${docdir}
	-rmdir ${DESTDIR}${pkgconfigdir}

include $(abs_top_nlbuild_autotools_dir)/automake/post.am
