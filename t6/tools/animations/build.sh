#!/bin/bash
# Builds t6xanimpack against OpenAssetTools on Linux (x64), and writes the campaign's animations from BO2's
# zone/all/common.ff to T6iris-campaign-animations.bin, which goes in the game's folder next to t6mp.exe.
#
#   ./build.sh /path/to/bo2/zone/all/common.ff [more zones...]
set -e
here="$(cd "$(dirname "$0")" && pwd)"
work="${WORK:-$here/build}"
mkdir -p "$work"
cd "$work"
if [ ! -d OpenAssetTools ]; then
    git clone --depth 1 https://github.com/Laupetin/OpenAssetTools.git
    git -C OpenAssetTools submodule update --init --depth 1 thirdparty/libtomcrypt thirdparty/libtommath \
        thirdparty/zlib thirdparty/json thirdparty/lz4 thirdparty/stb thirdparty/catch2
    mkdir -p OpenAssetTools/thirdparty/eigen  # (only its model writing needs it)
fi
if [ ! -x premake/bin/release/premake5 ]; then
    git clone --depth 1 -b v5.0.0-beta8 https://github.com/premake/premake-core.git premake
    make -C premake -f Bootstrap.mak linux
fi
cd OpenAssetTools
../premake/bin/release/premake5 gmake
make -C build config=release_x64 ZoneLoading ZoneCommon Common Utils Cryptography XMemCompress lzx minilzo salsa20 \
    libtomcrypt libtommath zlib -j"$(nproc)"
O="$PWD"
L="$O/build/lib/Release_x64"
g++ -std=c++23 -O2 -DARCH_x64 -I"$O/src/Common" -I"$O/src/ZoneCommon" -I"$O/src/ZoneLoading" -I"$O/src/Utils" \
    -I"$O/src/Cryptography" -I"$O/build/src/ZoneCode" -I"$O/thirdparty/zlib" "$here/t6xanimpack.cpp" -o "$work/t6xanimpack" \
    -L"$L" -lZoneLoading -lZoneCommon -lCryptography -lCommon -lUtils -lXMemCompress -llzx -lminilzo -lsalsa20 \
    -llibtomcrypt -llibtommath -lzlib -lpthread
cd "$here"
"$work/t6xanimpack" "$here/T6iris-campaign-animations.bin" "$@"
