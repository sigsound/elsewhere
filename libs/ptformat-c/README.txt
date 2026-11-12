libptformat in Elsewhere
=====================

Elsewhere reads Pro Tools sessions with libptformat (https://github.com/zamaudio/ptformat),
by Damien Zammit and Robin Gareus, licensed under the GNU Lesser General Public License,
version 2.1 or later (see ptformat/COPYING.LESSER).

It's built as a separate shared library that ships with Elsewhere and that Elsewhere loads when
you import a Pro Tools session:

    macOS:    Elsewhere.app/Contents/Frameworks/libptformat.dylib
    Windows:  ptformat.dll, next to Elsewhere.exe

This archive is that library's complete source code:

    libs/ptformat/     libptformat, at upstream commit af15ee9, with one change by Elsewhere:
                       sample positions are converted between rates in double precision
                       (was float). The change is patches/ptformat-double-ratefactor.patch,
                       and it's already applied to the files here.
    libs/ptformat-c/   A small C entry point (ptformat_c_load / ptformat_c_free) compiled into
                   the same library, so only C strings cross between it and Elsewhere. Also
                   LGPL-2.1-or-later.

You're free to change it and use your own build instead of the one that came with Elsewhere.

Building it
-----------

From the libs folder.

macOS (then re-sign the app, as replacing a file breaks its signature):

    c++ -std=c++17 -O2 -shared -fPIC -fvisibility=hidden -Iptformat -Iptformat-c \
        ptformat/ptformat.cc ptformat-c/ptformat_c.cc -install_name @rpath/libptformat.dylib \
        -o libptformat.dylib
    cp libptformat.dylib /Applications/Elsewhere.app/Contents/Frameworks/
    codesign --force --deep --sign - /Applications/Elsewhere.app

Windows (Visual Studio Developer Command Prompt):

    cl /O2 /LD /EHsc /MT /Iptformat /Iptformat-c ptformat\ptformat.cc ptformat-c\ptformat_c.cc /Fe:ptformat.dll
    copy ptformat.dll "C:\Program Files\Elsewhere\"

Elsewhere looks for the functions ptformat_c_abi_version (returning 1), ptformat_c_load
and ptformat_c_free, described in ptformat-c/ptformat_c.h.
