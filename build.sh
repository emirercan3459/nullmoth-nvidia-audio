#!/bin/bash
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
LILU="${LILU_DIR:-$HERE/../Lilu/Lilu}"
MKSDK="${MAC_KERNEL_SDK_DIR:-$HERE/../MacKernelSDK}"
OUT="$HERE/Release"

if [ ! -d "$LILU" ]; then
    echo "Cloning Lilu..."
    git clone --depth 1 https://github.com/acidanthera/Lilu.git "$HERE/Lilu_repo"
    LILU="$HERE/Lilu_repo/Lilu"
fi

if [ ! -d "$MKSDK" ]; then
    echo "Cloning MacKernelSDK..."
    git clone --depth 1 https://github.com/acidanthera/MacKernelSDK.git "$HERE/MacKernelSDK"
    MKSDK="$HERE/MacKernelSDK"
fi

rm -rf "$OUT/NMHDA.kext"
mkdir -p "$OUT/NMHDA.kext/Contents/MacOS"

FLAGS=(-arch x86_64 -mmacosx-version-min=15.0 -fapple-kext -mkernel -nostdinc
       -I"$MKSDK/Headers" -I"$LILU" -I"$LILU/Headers"
       -DKERNEL -DKERNEL_PRIVATE -DDRIVER_PRIVATE -DAPPLE -DNeXT -DPRODUCT_NAME=NMHDA -DMODULE_VERSION=1.1.0
       -std=c++17 -fno-rtti -fno-exceptions -fno-builtin -fno-common -O2 -Wall -Wno-unused-parameter -g0)

echo "Compiling nmhda.cpp..."
xcrun clang++ "${FLAGS[@]}" -c "$HERE/src/nmhda.cpp" -o "$OUT/nmhda.o"

echo "Compiling plugin_start.cpp..."
xcrun clang++ "${FLAGS[@]}" -c "$LILU/Library/plugin_start.cpp" -o "$OUT/plugin_start.o"

echo "Linking NMHDA.kext binary..."
xcrun clang++ -arch x86_64 -mmacosx-version-min=15.0 -fapple-kext -nostdlib -Xlinker -kext \
  -L"$MKSDK/Library/universal" -lkmodc++ -lkmod \
  "$OUT/nmhda.o" "$OUT/plugin_start.o" -o "$OUT/NMHDA.kext/Contents/MacOS/NMHDA"

cp "$HERE/src/Info.plist" "$OUT/NMHDA.kext/Contents/Info.plist"
rm -f "$OUT/nmhda.o" "$OUT/plugin_start.o"

(cd "$OUT" && rm -f NMHDA-v1.1.0-macOS-Sequoia-Tahoe.zip && zip -rq NMHDA-v1.1.0-macOS-Sequoia-Tahoe.zip NMHDA.kext)

echo "NMHDA.kext successfully built at: $OUT/NMHDA.kext"
shasum -a 256 "$OUT/NMHDA.kext/Contents/MacOS/NMHDA"
