#!/bin/bash

TERMUX_PKG_HOMEPAGE=https://ffmpeg.org
TERMUX_PKG_DESCRIPTION="Tools and libraries to manipulate a wide range of multimedia formats and protocols"
TERMUX_PKG_LICENSE="GPL-3.0"
TERMUX_PKG_MAINTAINER="@termux"
# Please align version with `ffplay` package.
TERMUX_PKG_VERSION="6.1.1"
TERMUX_PKG_REVISION=4
TERMUX_PKG_SRCURL=https://www.ffmpeg.org/releases/ffmpeg-${TERMUX_PKG_VERSION}.tar.xz
TERMUX_PKG_SHA256=8684f4b00f94b85461884c3719382f1261f0d9eb3d59640a1f4ac0873616f968
TERMUX_PKG_DEPENDS="fontconfig, freetype, fribidi, game-music-emu, harfbuzz, libaom, libandroid-glob, libass, libbluray, libbz2, libdav1d, libgnutls, libiconv, liblzma, libmp3lame, libopencore-amr, libopenmpt, libopus, librav1e, libsoxr, libsrt, libssh, libtheora, libv4l, libvo-amrwbenc, libvorbis, libvpx, libvidstab, libwebp, libx264, libx265, libxml2, libzimg, littlecms, ocl-icd, svt-av1, xvidcore, zlib"
TERMUX_PKG_BUILD_DEPENDS="opencl-headers"
TERMUX_PKG_CONFLICTS="libav"
TERMUX_PKG_BREAKS="ffmpeg-dev"
TERMUX_PKG_REPLACES="ffmpeg-dev"

termux_step_pre_configure() {
    # Do not forget to bump revision of reverse dependencies and rebuild them
    # after SOVERSION is changed. (These variables are also used afterwards.)
    _FFMPEG_SOVER_avutil=58
    _FFMPEG_SOVER_avcodec=60
    _FFMPEG_SOVER_avformat=60

    local f
    for f in util codec format; do
        local v=$(sh ffbuild/libversion.sh av${f} \
                libav${f}/version.h libav${f}/version_major.h \
                | sed -En 's/^libav'"${f}"'_VERSION_MAJOR=([0-9]+)$/\1/p')
        if [ ! "${v}" ] || [ "$(eval echo \$_FFMPEG_SOVER_av${f})" != "${v}" ]; then
            termux_error_exit "SOVERSION guard check failed for libav${f}.so."
        fi
    done
}

termux_step_configure() {
    echo "Running termux_step_configure"
    cd $GITHUB_WORKSPACE/ffmpeg-6.1.1

    # 创建调试信息文件
    debug_file="configure_debug.txt"
    echo "" > "$debug_file"

    local _EXTRA_CONFIGURE_FLAGS=""
    if [ $ARCH = "arm" ]; then
        _ARCH="armeabi-v7a"
        _EXTRA_CONFIGURE_FLAGS="--enable-neon"
    elif [ $ARCH = "i686" ]; then
        _ARCH="x86"
        _EXTRA_CONFIGURE_FLAGS="--disable-asm"
    elif [ $ARCH = "x86_64" ]; then
        _ARCH="x86_64"
    elif [ $ARCH = "aarch64" ]; then
        _ARCH=$ARCH
        _EXTRA_CONFIGURE_FLAGS="--enable-neon"
    else
        termux_error_exit "Unsupported arch: $ARCH"
    fi

    # 将 configure 命令写入文件
    echo "Executing configure command:" >> "$debug_file"
    echo "$GITHUB_WORKSPACE/ffmpeg-6.1.1/configure \\" >> "$debug_file"
    echo "    --arch=\"${_ARCH}\" \\" >> "$debug_file"
    echo "    --as=\"$AS\" \\" >> "$debug_file"
    echo "    --cc=\"$CC\" \\" >> "$debug_file"
    echo "    --cxx=\"$CXX\" \\" >> "$debug_file"
    echo "    --nm=\"$NM\" \\" >> "$debug_file"
    echo "    --pkg-config=\"$PKG_CONFIG\" \\" >> "$debug_file"
    echo "    --strip=\"$STRIP\" \\" >> "$debug_file"
    echo "    --cross-prefix=\"${CROSS_PREFIX}\" \\" >> "$debug_file"
    echo "    --disable-static \\" >> "$debug_file"
    echo "    --disable-symver \\" >> "$debug_file"
    echo "    --enable-shared \\" >> "$debug_file"
    echo "    --enable-cross-compile \\" >> "$debug_file"
    echo "    --enable-libx264 \\" >> "$debug_file"
    echo "    --enable-libx265 \\" >> "$debug_file"
    echo "    --prefix=\"${PREFIX}\" \\" >> "$debug_file"
    echo "    --target-os=android \\" >> "$debug_file"
    echo "    $_EXTRA_CONFIGURE_FLAGS" >> "$debug_file"

    # 执行 configure 命令，并将输出重定向到文件
    $GITHUB_WORKSPACE/ffmpeg-6.1.1/configure \
        --arch="${_ARCH}" \
        --as="$AS" \
        --cc="$CC" \
        --cxx="$CXX" \
        --nm="$NM" \
        --pkg-config="$PKG_CONFIG" \
        --strip="$STRIP" \
        --cross-prefix="${CROSS_PREFIX}" \
        --disable-static \
        --disable-symver \
        --enable-shared \
        --enable-cross-compile \
        --enable-libx264 \
        --enable-libx265 \
        --prefix="${PREFIX}" \
        --target-os=android \
        $_EXTRA_CONFIGURE_FLAGS >> "$debug_file" 2>&1

    # 检查 configure 命令返回值
    if [ $? -ne 0 ]; then
        echo "Error: configure failed!" >> "$debug_file"
        exit 1
    fi

    # 删除 configure.log 文件
    rm -f configure.log
}


