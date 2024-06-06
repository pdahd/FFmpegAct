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
    cd $GITHUB_WORKSPACE/ffmpeg-6.1.1

    local _EXTRA_CONFIGURE_FLAGS=""
    if [ $ARCH = "arm" ]; then
        _ARCH="armeabi-v7a"
        _EXTRA_CONFIGURE_FLAGS="--enable-neon"
    elif [ $ARCH = "i686" ]; then
        _ARCH="x86"
        # Specify --disable-asm to prevent text relocations on i686,
        # see https://trac.ffmpeg.org/ticket/4928
        _EXTRA_CONFIGURE_FLAGS="--disable-asm"
    elif [ $ARCH = "x86_64" ]; then
        _ARCH="x86_64"
    elif [ $ARCH = "aarch64" ]; then
        _ARCH=$ARCH
        _EXTRA_CONFIGURE_FLAGS="--enable-neon"
    else
        termux_error_exit "Unsupported arch: $ARCH"
    fi
    
    $GITHUB_WORKSPACE/ffmpeg-6.1.1/configure \
        --arch="${_ARCH}" \
        --as="$AS" \
        --cc="$CC" \
        --cxx="$CXX" \
        --nm="$NM" \
        --pkg-config="$PKG_CONFIG" \
        --strip="$STRIP" \
        --cross-prefix="${CROSS_PREFIX}" \
        --disable-indevs \
        --disable-outdevs \
        --enable-indev=lavfi \
        --disable-static \
        --disable-symver \
        --enable-cross-compile \
        --enable-gnutls \
        --enable-gpl \
        --enable-version3 \
        --enable-jni \
        --enable-lcms2 \
        --enable-libaom \
        --enable-libass \
        --enable-libbluray \
        --enable-libdav1d \
        --enable-libfontconfig \
        --enable-libfreetype \
        --enable-libfribidi \
        --enable-libgme \
        --enable-libharfbuzz \
        --enable-libmp3lame \
        --enable-libopencore-amrnb \
        --enable-libopencore-amrwb \
        --enable-libopenmpt \
        --enable-libopus \
        --enable-librav1e \
        --enable-libsoxr \
        --enable-libsrt \
        --enable-libssh \
        --enable-libsvtav1 \
        --enable-libtheora \
        --enable-libv4l2 \
        --enable-libvidstab \
        --enable-libvo-amrwbenc \
        --enable-libvorbis \
        --enable-libvpx \
        --enable-libwebp \
        --enable-libx264 \
        --enable-libx265 \
        --enable-libxml2 \
        --enable-libxvid \
        --enable-libzimg \
        --enable-mediacodec \
        --enable-opencl \
        --enable-shared \
        --prefix="${PREFIX}" \
        --target-os=android \
        --extra-libs="-landroid-glob" \
        --disable-vulkan \
        $_EXTRA_CONFIGURE_FLAGS \
        --disable-libfdk-aac
    make -j$(nproc)
    make install
    make clean
    # GPLed FFmpeg binaries linked against fdk-aac are not redistributable.
}


