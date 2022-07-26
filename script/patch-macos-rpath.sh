#!/bin/sh

for bin in musikcube musikcubed; do
  install_name_tool -add_rpath "@executable_path/" "$1"/bin/$bin
  install_name_tool -add_rpath "@executable_path/lib" "$1"/bin/$bin
done
exit 0
