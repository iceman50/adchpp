# Copyright (C) 2026 iceman50
# vim: set filetype: py

EnsureSConsVersion(4, 0, 0)

import os, sys
from build_util import Dev

gcc_flags = {
    "common": [
        "-g",
        "-Wall",
        "-Wextra",
        "-Wno-unused-parameter",
        "-Wno-unused-value",
        "-Wno-missing-field-initializers",
        "-Wno-address",
        "-Wno-unknown-pragmas",
        "-Wno-deprecated-declarations",  # TODO re-eval on boost updates
        "-Wno-deprecated",  # C++11 deprecates dynamic exception specifications
        "-fexceptions",
    ],
    "debug": [],
    "release": ["-O3", "-fno-ipa-cp-clone"],
}

# Use the strict ISO mode so GNU extensions cannot hide C++11 portability issues.
gcc_xxflags = {"common": ["-std=c++11"], "debug": [], "release": []}

msvc_flags = {
    # 4100: unreferenced formal parameter
    # 4121: alignment of member sensitive to packing
    # 4127: conditional expression is constant
    # 4189: var init'd, unused
    # 4244: possible loss of data on conversion
    # 4290: exception spec ignored
    # 4355: "this" used in a constructor
    # 4510: no default constructor
    # 4512: assn not generated
    # 4610: no default constructor
    # 4706: assignment within conditional expression
    # 4800: converting from BOOL to bool
    # 4996: fn unsafe, use fn_s
    "common": [
        "/W4",
        "/EHsc",
        "/Zi",
        "/Zm200",
        "/GR",
        "/FC",
        "/wd4100",
        "/wd4121",
        "/wd4127",
        "/wd4189",
        "/wd4244",
        "/wd4290",
        "/wd4355",
        "/wd4510",
        "/wd4512",
        "/wd4610",
        "/wd4706",
        "/wd4800",
        "/wd4996",
    ],
    "debug": ["/MDd", "/LDd"],
    "release": ["/O2", "/MD", "/LD"],
}
# we set /LD(d) by default for all sub-projects, since most of them are DLLs. don't forget to
# remove it when building executables!

# MSVC has no C++11-specific /std switch; supported MSVC releases expose
# their C++11 language support in the default mode. compiler.h enforces the
# minimum compiler version for that path.
msvc_xxflags = {"common": [], "debug": [], "release": []}

gcc_link_flags = {"common": ["-g", "$UNDEF", "-time"], "debug": [], "release": ["-O3"]}

msvc_link_flags = {
    "common": ["/DEBUG", "/FIXED:NO", "/INCREMENTAL:NO"],
    "debug": [],
    "release": [],
}

msvc_defs = {
    "common": ["_REENTRANT"],
    "debug": ["_DEBUG", "_HAS_ITERATOR_DEBUGGING=0", "_SECURE_SCL=0"],
    "release": ["NDEBUG"],
}

gcc_defs = {
    # _BSD_SOURCE is for some int types in LuaSocket on Linux.
    # _DEFAULT_SOURCE:
    # https://sourceware.org/glibc/wiki/Release/2.20#Deprecation_of__BSD_SOURCE_and__SVID_SOURCE_feature_macros
    "common": ["_REENTRANT", "_BSD_SOURCE", "_DEFAULT_SOURCE"],
    "debug": ["_DEBUG"],
    "release": ["NDEBUG"],
}

# --- cut ---

import os, sys, sysconfig

# Plugins we can build have an "src/SConscript" file.
plugins = [
    plugin
    for plugin in os.listdir("plugins")
    if os.path.isfile(os.path.join("plugins", plugin, "src", "SConscript"))
]

langs = ["lua", "python", "ruby"]

defEnv = Environment(ENV=os.environ)
opts = Variables("custom.py", ARGUMENTS)

if sys.platform == "win32":
    tooldef = "mingw"
else:
    tooldef = "default"

opts.AddVariables(
    EnumVariable(
        "tools",
        "Toolset to compile with, default = platform default (msvc under windows)",
        tooldef,
        ["mingw", "default", "clang", "clang-analyzer"],
    ),
    EnumVariable("mode", "Compile mode", "debug", ["debug", "release"]),
    ListVariable("plugins", "The plugins to compile", "all", plugins),
    ListVariable("langs", "The language bindings to compile", "all", langs),
    BoolVariable("secure", "Add support for secure TLS connections via OpenSSL", "yes"),
    (
        "openssl",
        "OpenSSL root containing include/ and lib/ (Windows)",
        "openssl",
    ),
    BoolVariable(
        "gch",
        "Use GCH when compiling GUI (disable if you have linking problems with mingw)",
        "yes",
    ),
    BoolVariable("verbose", "Show verbose command lines", "no"),
    BoolVariable(
        "savetemps", "Save intermediate compilation files (assembly output)", "no"
    ),
    ("prefix", "Prefix to use when cross compiling", ""),
    EnumVariable("arch", "Target architecture", "x64", ["arm64", "x64"]),
    (
        "python",
        "Python path to use when compiling python extensions",
        sysconfig.get_config_var("prefix"),
    ),
    ("ruby", "Path to the ruby binary", "ruby"),
    ("lua", "Path to the lua binary", "lua"),
    BoolVariable("systemlua", "Try to use the system lua libraries", "no"),
    BoolVariable("systemboost", "Use the system boost libraries", "no"),
    BoolVariable("docs", "Build docs (requires asciidoc)", "no"),
)

opts.Update(defEnv)
Help(opts.GenerateHelpText(defEnv))

# workaround for SCons 1.2 which hard-codes possible archs (only allows 'x86' and 'amd64'...)
# TODO remove when SCons knows about all available archs
TARGET_ARCH = defEnv["arch"]
if TARGET_ARCH == "x64":
    TARGET_ARCH = "amd64"

env = Environment(
    ENV=os.environ,
    tools=[defEnv["tools"], "swig"],
    toolpath=["tools"],
    options=opts,
    TARGET_ARCH=TARGET_ARCH,
    MSVS_ARCH=TARGET_ARCH,
)

# Keep the OpenSSL location configurable so a trusted local static build can
# be consumed without copying or modifying third-party sources in this tree.
env["OPENSSL_ROOT"] = Dir(env["openssl"]).abspath


# filter out boost from dependencies to get a speedier rebuild scan
# this means that if boost changes, scons -c needs to be run
# delete .sconsign.dblite to see the effects of this if you're upgrading
def filterBoost(x):
    return [y for y in x if str(y).find("boost") == -1]


SourceFileScanner.function[".c"].recurse_nodes = filterBoost
SourceFileScanner.function[".cpp"].recurse_nodes = filterBoost
SourceFileScanner.function[".h"].recurse_nodes = filterBoost
SourceFileScanner.function[".hpp"].recurse_nodes = filterBoost

dev = Dev(env)
dev.prepare()

env.SConsignFile()

conf = Configure(
    env,
    conf_dir=dev.get_build_path(".sconf_temp"),
    log_file=dev.get_build_path("config.log"),
    clean=False,
    help=False,
    custom_tests={"CheckBoost": dev.CheckBoost},
)

if dev.env["systemboost"]:
    sysBoostLibs = []

    def checkBoostLib_(lib, header, call):
        if conf.CheckLibWithHeader(lib, header, "C++", call, 0):
            sysBoostLibs.append(lib)
            return True
        return False

    def checkBoostLib(lib, header, call):
        return checkBoostLib_(lib, header, call) or checkBoostLib_(
            lib + "-mt", header, call
        )  # Cygwin boost libs are named that way

    if (
        not conf.CheckBoost("1.49.0")
        or not checkBoostLib(
            "libboost_system",
            "boost/system/error_code.hpp",
            "boost::system::error_code ec;",
        )
        or not checkBoostLib(
            "libboost_date_time",
            "boost/date_time/posix_time/posix_time.hpp",
            "boost::posix_time::microsec_clock::local_time();",
        )
        or not checkBoostLib(
            "libboost_locale",
            "boost/locale.hpp",
            'boost::locale::generator().generate("");',
        )
    ):
        raise Exception("Cannot use system boost libraries - try with systemboost=0")

if not dev.env["systemboost"]:
    env.Append(CPPPATH=["#/boost/"])
    env.Append(CPPDEFINES=["BOOST_ALL_DYN_LINK=1"])
    if env["CC"] == "cl":  # MSVC
        env.Append(CPPDEFINES=["BOOST_ALL_NO_LIB=1"])

if not dev.is_win32():
    env.Append(CPPDEFINES=["_XOPEN_SOURCE=500"])
    env.Append(CCFLAGS=["-fvisibility=hidden"])
    env.Append(LIBS=["stdc++", "m"])

if "gcc" in env["TOOLS"]:
    if dev.is_win32():
        env.Append(LINKFLAGS=["-Wl,--enable-auto-import"])

    if env["savetemps"]:
        env.Append(CCFLAGS=["-save-temps", "-fverbose-asm"])
    else:
        env.Append(CCFLAGS=["-pipe"])

    if env["arch"] == "x64":
        env.Append(CCFLAGS=["-march=nehalem"])

if env["CC"] == "cl":  # MSVC
    flags = msvc_flags
    xxflags = msvc_xxflags
    link_flags = msvc_link_flags
    defs = msvc_defs

    if env["arch"] == "x86":
        env.Append(CPPDEFINES=["_USE_32BIT_TIME_T=1"])  # for compatibility with PHP

else:
    flags = gcc_flags
    xxflags = gcc_xxflags
    link_flags = gcc_link_flags
    defs = gcc_defs

    env.Tool("gch", toolpath=".")

env.Append(CPPDEFINES=defs[env["mode"]])
env.Append(CPPDEFINES=defs["common"])

env.Append(CCFLAGS=flags[env["mode"]])
env.Append(CCFLAGS=flags["common"])

env.Append(CXXFLAGS=xxflags[env["mode"]])
env.Append(CXXFLAGS=xxflags["common"])

env.Append(LINKFLAGS=link_flags[env["mode"]])
env.Append(LINKFLAGS=link_flags["common"])
env.Append(UNDEF="-Wl,--no-undefined")

if dev.is_win32():
    env.Append(LIBS=["ws2_32", "mswsock"])
    if env["secure"]:
        env.Append(LIBS=["crypt32"]) 

import SCons.Scanner

SWIGScanner = SCons.Scanner.ClassicCPP(
    "SWIGScan",
    ".i",
    "CPPPATH",
    '^[ \t]*[%,#][ \t]*(?:include|import)[ \t]*(<|")([^>"]+)(>|")',
)
env.Append(SCANNERS=[SWIGScanner])

if not env.GetOption("clean") and not env.GetOption("help"):

    if not dev.is_win32():
        if conf.CheckCHeader("poll.h"):
            conf.env.Append(CPPDEFINES="HAVE_POLL_H")
        if conf.CheckCHeader("sys/epoll.h"):
            conf.env.Append(CPPDEFINES=["HAVE_SYS_EPOLL_H"])
        if conf.CheckLib("pthread", "pthread_create"):
            conf.env.Append(CPPDEFINES=["HAVE_PTHREAD"])
        if env["secure"]:
            if not conf.CheckLib("ssl", "SSL_connect"):
                raise Exception(
                    "secure=yes was requested, but OpenSSL could not be linked"
                )
            conf.env.Append(CPPDEFINES=["HAVE_OPENSSL"])
        if conf.CheckLib("dl", "dlopen"):
            conf.env.Append(CPPDEFINES=["HAVE_DL"])
    else:
        if env["secure"]:
            openssl_header = os.path.join(
                env["OPENSSL_ROOT"], "include", "openssl", "ssl.h"
            )
            if not os.path.isfile(openssl_header):
                raise Exception(
                    "secure=yes was requested, but OpenSSL headers were not found at "
                    + env["OPENSSL_ROOT"]
                )
            conf.env.Append(CPPDEFINES=["HAVE_OPENSSL"])

        # LuaSocket 2.0 supplies ip_mreq itself on newer MinGW-w64 releases,
        # while current winsock.h supplies the same type. Keep the vendored
        # source untouched and suppress the system typedef only for C sources
        # when the compatibility header demonstrates that it is necessary.
        lua_socket_header = env.File(
            "#/lua/LuaSocket/socket/wsocket.h"
        ).abspath.replace("\\", "/")
        if (
            "gcc" in env["TOOLS"]
            and ("Script" in env["plugins"] or "lua" in env["langs"])
            and not conf.CheckCHeader(lua_socket_header)
        ):
            conf.env.Append(CFLAGS=["-D_MINGW_IP_MREQ1_H"])
            if not conf.CheckCHeader(lua_socket_header):
                raise Exception("Cannot compile the bundled LuaSocket headers")

    env = conf.Finish()

env.Append(LIBPATH=env.Dir(dev.get_build_root() + "bin/").abspath)
if not dev.is_win32():
    dev.env.Append(RPATH=env.Literal("\\$$ORIGIN"))

if dev.env["systemboost"]:
    env.Append(LIBS=sysBoostLibs)

else:
    dev.boost_system = dev.build("boost/libs/system/src/")
    dev.boost_date_time = dev.build("boost/libs/date_time/src/")
    dev.boost_locale = dev.build("boost/libs/locale/src/")

    env.Append(LIBS=["aboost_system", "aboost_date_time", "aboost_locale"])

dev.adchpp = dev.build("adchpp/")

dev.build("adchppd/")

# Lua for plugins & swig
if "Script" in env["plugins"] or "lua" in env["langs"]:
    dev.build("lua/")

# Library wrappers
dev.build("swig/")

# Plugins
for plugin in env["plugins"]:
    dev.build("plugins/" + plugin + "/src/")

if env["docs"]:
    asciidoc_cmd = dev.get_asciidoc()
    if asciidoc_cmd is None:
        print("asciidoc not found, docs won't be built")

    else:
        env["asciidoc_cmd"] = asciidoc_cmd

        def asciidoc(target, source, env):
            env.Execute(
                env["asciidoc_cmd"]
                + ' -o"'
                + str(target[0])
                + '" "'
                + str(source[0])
                + '"'
            )

        doc_path = "#/build/docs/"

        env.Command(doc_path + "readme.html", "#/readme.txt", asciidoc)

        guide_path = "#/docs/user_guide/"
        env.Command(
            doc_path + "user_guide/basic_guide.html",
            guide_path + "basic_guide.txt",
            asciidoc,
        )
        env.Command(
            doc_path + "user_guide/expert_guide.html",
            guide_path + "expert_guide.txt",
            asciidoc,
        )
        env.Command(
            doc_path + "user_guide/images",
            guide_path + "images",
            Copy("$TARGET", "$SOURCE"),
        )
