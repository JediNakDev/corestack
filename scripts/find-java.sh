#!/bin/sh
#
# find-java.sh - locate a JDK new enough to build and run SimpleDB.
#
# Prints the bin/ directory of the first suitable JDK found, and exits 0. Prints
# nothing and exits 1 when there is none.
#
#   usage: find-java.sh [minimum-major-version]     (default: 17)
#
# WHY THIS EXISTS
#
# db/src/java/simpledb/SocketRunner.java binds a Unix domain socket through
# java.net.UnixDomainSocketAddress, which arrived in JDK 16. A machine with an
# older `java` first on PATH does not fail at build time - ant resolves its own
# JDK, which is frequently a different and newer one - it fails at RUN time,
# inside the runner, as
#
#   NoClassDefFoundError: java/net/UnixDomainSocketAddress
#
# on stderr in var/log/tetrisdb.err. What the C side sees is only the absence of
# the "<<READY>>" handshake, so every database-backed suite reports a timeout
# and none of them names the actual cause. Resolving the JDK here, once, and
# putting it ahead of everything else on PATH for the recipes that build or run
# the jar keeps the compiler and the runtime the same JDK.
#
# Ordinary `java` on PATH is tried FIRST, so a machine that is already set up
# correctly keeps using exactly the JDK its owner chose.

min=${1:-17}

# A JAVA_HOME of /usr names the directory the macOS stub lives in, not a JDK.
# Every probe below runs a java binary, and with that value exported each one
# is the stub being told to resolve into itself - which blocks, without output
# or CPU, rather than failing. Drop it here, once, so no probe can inherit it.
if [ -n "$JAVA_HOME" ] && [ "$(cd "$JAVA_HOME" 2>/dev/null && pwd)" = /usr ]; then
    unset JAVA_HOME
fi

# The major version of a java/javac binary: 8 for "1.8.0_452", 17 for "17.0.9",
# 26 for "26.0.1". Empty if the binary does not run or prints nothing familiar.
major_of() {
    "$1" -version 2>&1 | awk -F'"' '
        /version "/ {
            v = $2
            sub(/^1\./, "", v)          # 1.8.0_452 -> 8.0_452
            sub(/[^0-9].*$/, "", v)     # 17.0.9    -> 17
            print v
            exit
        }'
}

# Echo the bin directory if <dir>/java is at least $min. javac is not required:
# ant brings its own compiler, and the runtime is what the failure above is
# about.
try_bin() {
    [ -n "$1" ] || return 1
    [ -x "$1/java" ] || return 1
    v=$(major_of "$1/java")
    [ -n "$v" ] || return 1
    [ "$v" -ge "$min" ] 2>/dev/null || return 1
    echo "$1"
    exit 0
}

# 1. Whatever `java` already resolves to.
#
# On macOS that is usually /usr/bin/java, a stub which re-executes the JDK
# named by JAVA_HOME, or the one /usr/libexec/java_home picks when it is
# unset. Its directory is not a JDK home, and the callers of this script
# derive JAVA_HOME from the directory it prints - so answering "/usr/bin"
# here sets JAVA_HOME=/usr, which is exactly the input that makes the stub
# resolve to itself and hang. Follow the same two steps the stub follows, so
# the answer is a real JDK's own bin and the version check below reads that
# JDK rather than the stub standing in front of it.
onpath=$(command -v java 2>/dev/null)
if [ -n "$onpath" ]; then
    onbin=$(dirname "$onpath")
    if [ "$onbin" = /usr/bin ]; then
        if [ -n "$JAVA_HOME" ] && [ -x "$JAVA_HOME/bin/java" ]; then
            onbin="$JAVA_HOME/bin"
        elif [ -x /usr/libexec/java_home ]; then
            stubhome=$(/usr/libexec/java_home 2>/dev/null) &&
                onbin="$stubhome/bin"
        fi
    fi
    try_bin "$onbin"
fi

# 2. An explicitly configured JAVA_HOME.
[ -n "$JAVA_HOME" ] && try_bin "$JAVA_HOME/bin"

# 3. macOS's registry. Its -v filter is advisory - it falls back to the newest
#    JDK it has when nothing matches - so try_bin re-checks the version rather
#    than trusting the query.
if [ -x /usr/libexec/java_home ]; then
    for want in "$min+" "$min"; do
        home=$(/usr/libexec/java_home -v "$want" 2>/dev/null) && try_bin "$home/bin"
    done
    home=$(/usr/libexec/java_home 2>/dev/null) && try_bin "$home/bin"
fi

# 4. Homebrew, which does not symlink its JDKs into the macOS registry, so a
#    `brew install openjdk` is invisible to step 3.
if command -v brew >/dev/null 2>&1; then
    for keg in openjdk openjdk@26 openjdk@25 openjdk@24 openjdk@23 openjdk@22 \
               openjdk@21 openjdk@20 openjdk@19 openjdk@18 openjdk@17; do
        prefix=$(brew --prefix "$keg" 2>/dev/null) || continue
        try_bin "$prefix/bin"
        try_bin "$prefix/libexec/openjdk.jdk/Contents/Home/bin"
    done
fi

# 5. The usual Linux distribution layouts, newest-looking first.
for dir in $(ls -d /usr/lib/jvm/*/bin /usr/java/*/bin 2>/dev/null | sort -r); do
    try_bin "$dir"
done

exit 1
