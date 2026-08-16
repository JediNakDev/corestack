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
onpath=$(command -v java 2>/dev/null)
[ -n "$onpath" ] && try_bin "$(dirname "$onpath")"

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
