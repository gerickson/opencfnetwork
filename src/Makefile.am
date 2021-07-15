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
#      package first-party source code, if any.
#

include $(abs_top_nlbuild_autotools_dir)/automake/pre.am

# Presently 'include' is not enumerated here since it is covered by
# the top-level makefile.  Including it redundantly here will cause
# 'make distclean' to fail since the top-level makefile recursion into
# it for that target will remove its makefile resulting in recursion
# from this one failing.

SUBDIRS     = \
    $(NULL)

include $(abs_top_nlbuild_autotools_dir)/automake/post.am
