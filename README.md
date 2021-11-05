[![Build Status][opencfnetwork-github-action-svg]][opencfnetwork-github-action]

[opencfnetwork-github]: https://github.com/gerickson/opencfnetwork
[opencfnetwork-github-action]: https://github.com/gerickson/opencfnetwork/actions?query=workflow%3ABuild+branch%3Amain+event%3Apush
[opencfnetwork-github-action-svg]: https://github.com/gerickson/opencfnetwork/actions/workflows/build.yml/badge.svg?branch=main&event=push

Open CFNetwork
==============

# Introduction

This is the public, open source distribution of [Apple, Inc.'s
CFNetwork framework](https://opensource.apple.com/source/CFNetwork/).
This distribution is refered to as OpenCFNetwork to distinguish it
from the official Apple release, and to reflect the open source,
community-based and -driven nature of this project.


This release of Open CFNetwork corresponds to the CFNetwork
framework found in Mac OS X 10.8 and later,
[CFNetwork-129.20](https://opensource.apple.com/source/CFNetwork/CFNetwork-129.20/).

The goal of this port is to provide, over time, a feature-compatible, cross-
platform version of the official CFNetwork framework. In general,
we do not propose extending functionality beyond the official Apple
release so that this project can serve as a drop-in replacement.

## What Works and What Does Not Work

While all of the functionality present in Open CFNetwork should work in Darwin, it has
not been tested relative to the Apple-provided equivalent framework.

For Linux, CFHost has been ported and tested. All other functionality has only been
altered for successful compilation and linking but not functionality.

For Windows, no porting or testing has been done.

| Functionality   | Darwin | Linux | Windows |
| --------------- | :----: | :---: | :-----: |
| FTP Stream      |   Y    |   N   |    ?    |
| Host            |   Y    |   Y   |    ?    |
| HTTP Stream     |   Y    |   N   |    ?    |
| Net Diagnostics |   Y    |   N   |    ?    |
| Net Services    |   Y    |   N   |    ?    |
| Socket Stream   |   Y    |  (1)  |    ?    |

1. The core CFSocketStream functionality in Linux has been ported and tested; however,
   the supplemental CFSocketStream functionality in CFNetwork, particularly for secure
   streams has not.

# Getting Started with Open CFNetwork

## Building Open CFNetwork

If you are not using a prebuilt distribution of Open CFNetwork,
building Open CFNetwork should be a straightforward. Start with:

    % ./configure
    % make

The second `configure` step generates `Makefile` files from
`Makefile.in` files and only needs to be done once unless those input
files have changed.

Although not strictly necessary, the additional step of sanity
checking the build results is recommended:

    % make check

### Dependencies

In addition to depending on the C Standard Libraries, Open CFNetwork
depends on:

* [avahi](https://www.avahi.org)
* [c-ares](https://c-ares.haxx.se)

The dependencies can either be satisfied by building them directly
from source, or on system such as Linux, installing them using a
package management system. For example, on Debian systems:

    % sudo apt-get install libavahi-compat-libdnssd-dev libc-ares-dev

## Installing Open CFNetwork

To install Open CFNetwork for your use simply invoke:

    % make install

to install Open CFNetwork in the location indicated by the --prefix
`configure` option (default "/usr/local"). If you intended an
arbitrarily relocatable Open CFNetwork installation and passed
`--prefix=/` to `configure`, then you might use DESTDIR to, for
example install Open CFNetwork in your user directory:

    % make DESTIDIR="${HOME}" install

## Maintaining Open CFNetwork

If you want to maintain, enhance, extend, or otherwise modify Open
CFNetwork, it is likely you will need to change its build system,
based on GNU autotools, in some circumstances.

After any change to the Open CFNetwork build system, including any
*Makefile.am* files or the *configure.ac* file, you must run the
`autoreconf` to update the build system.

### Dependencies

Due to its leverage of GNU autotools, if you want to modify or
otherwise maintain the Open CFNetwork build system, the following
additional packages are required and are invoked by `autoreconf`:

  * autoconf
  * automake
  * libtool

#### Linux

When supported on Linux, on Debian-based Linux distributions such as
Ubuntu, these Open CFNetwork build system dependencies can be satisfied
with the following:

    % sudo apt-get install autoconf automake libtool

#### Mac OS X

On Mac OS X, these dependencies can be installed and satisfied using
[Brew](https://brew.sh/):

    % brew install autoconf automake libtool

# Interact

There are numerous avenues for Open CFNetwork support:

  * Bugs and feature requests - [submit to the Issue Tracker](https://github.com/gerickson/opencfnetwork/issues)

# Versioning

Open CFNetwork follows Apple's upstream CFNetwork versioning.

# License

Open CFNetwork is released under the [Apple Public Source License 2.0 license](https://opensource.org/licenses/APSL-2.0).
See the `LICENSE` file for more information.

