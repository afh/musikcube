#!/bin/bash

PLATFORM=$(uname)

if [[ "$PLATFORM" == 'Linux' ]]; then
    echo "[patch-rpath] patch Linux .so files..."

    # update the RPATH so libraries in libs/ can discover each other,
    # and plugins can discover themselves, and libs/ (but not the
    # other way around)

    FILES="./bin/lib/*"
    for f in $FILES
    do
        patchelf --set-rpath "\$ORIGIN" "$f"
    done

    FILES="./bin/plugins/*.so"
    for f in $FILES
    do
        patchelf --set-rpath "\$ORIGIN:\$ORIGIN/../lib" "$f"
    done

    chmod -x ./bin/lib/*
fi

if [[ "$PLATFORM" == 'Darwin' ]]; then
    echo "[patch-rpath] patch macOS binaries..."

    install_name_tool -add_rpath "@executable_path/" bin/musikcube
    install_name_tool -add_rpath "@executable_path/lib" bin/musikcube
    install_name_tool -add_rpath "@executable_path/" bin/musikcubed
    install_name_tool -add_rpath "@executable_path/lib" bin/musikcubed
fi

exit 0