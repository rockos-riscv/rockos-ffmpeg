#!/bin/bash

GIT_DIR=$(git rev-parse --show-toplevel)
ROOT_DIR=$(realpath "$GIT_DIR")

if ! [ -d "$1" ]; then
    echo "set default FF_INSTALL_DIR: "$ROOT_DIR"/ffmpeg_build"
    if [ ! -d $ROOT_DIR/ffmpeg_build ]; then
        mkdir -p $ROOT_DIR/ffmpeg_build
    fi
    FF_INSTALL_DIR=$ROOT_DIR/ffmpeg_build
else
    FF_INSTALL_DIR=$1
fi

if ! [ -d "$2" ]; then
    INC_DIR=/usr/include
    echo "include: " \""$INC_DIR"\"
else
    INC_DIR=$2
    echo "include: " \""$INC_DIR"\"
fi

if ! [ -d "$3" ]; then
    LIB_DIR=/usr/lib64
	echo "lib: " \""$LIB_DIR"\"
else
    LIB_DIR=$3
    echo "lib: " \""$LIB_DIR"\"
fi

export LD_LIBRARY_PATH=$LIB_DIR:$INC_DIR:$LD_LIBRARY_PATH

if  [ "$4" = "docker" ]; then
    echo "build ffmpeg in docker."
    ./configure --prefix="$FF_INSTALL_DIR" --target-os=linux --enable-stripping --enable-static --enable-shared --extra-cflags="-DTRANSCODE_HOST=1 -DTRANSCODE_INFO_REPORT" --extra-cflags="-I"$INC_DIR"" --extra-ldflags="-L"$LIB_DIR" -les_mpp -lescltde -lesclrpc -lesclmemory -lesclplatform -lesclvdec -lesclvenc -lescllogger -les_host_query_cap -les-ipcm" --enable-avfilter --disable-version3 --enable-logging --enable-optimizations --disable-extra-warnings --enable-avdevice --enable-avcodec --enable-avformat --enable-network --disable-gray --enable-swscale-alpha --disable-small --disable-dxva2 --enable-runtime-cpudetect --disable-hardcoded-tables --disable-mipsdsp --disable-mipsdspr2 --disable-msa --enable-hwaccels --disable-cuda --disable-cuvid --disable-nvenc --disable-avisynth --disable-frei0r --disable-libopencore-amrnb --disable-libopencore-amrwb --disable-libdc1394 --disable-libgsm --disable-libilbc --disable-libvo-amrwbenc --disable-symver --disable-doc --disable-gpl --disable-nonfree --enable-ffmpeg --disable-ffplay --disable-libv4l2 --enable-ffprobe --disable-libxcb --disable-postproc --enable-swscale --enable-indevs --disable-alsa --enable-outdevs --enable-esmpp --enable-pthreads --enable-zlib --enable-bzlib --disable-libfdk-aac --disable-libcdio --disable-gnutls --disable-libdrm --disable-libopenh264 --disable-vaapi --disable-vdpau --disable-mmal --disable-omx --disable-omx-rpi --disable-libopencv --disable-libopus --disable-libvpx --disable-libass --disable-libbluray --disable-libvpl --disable-libmfx --disable-librtmp --disable-libmp3lame --disable-libmodplug --disable-libspeex --disable-libtheora --disable-iconv --disable-libfreetype --disable-fontconfig --disable-libopenjpeg --disable-libx264 --disable-libx265 --disable-libdav1d --disable-x86asm --disable-mmx --disable-sse --disable-sse2 --disable-sse3 --disable-ssse3 --disable-sse4 --disable-sse42 --disable-avx --disable-avx2 --disable-armv6 --disable-armv6t2 --disable-vfp --disable-neon --disable-altivec --enable-pic
else
    echo "build ffmpeg in ctyunos"
    ./configure --prefix="$FF_INSTALL_DIR" --target-os=linux --enable-stripping --enable-static --enable-shared --extra-cflags="-DTRANSCODE_HOST=1 -DTRANSCODE_INFO_REPORT" --extra-cflags="-I"$INC_DIR"" --extra-ldflags="-L"$LIB_DIR" -les_mpp -lescltde -lesclrpc -lesclmemory -lesclplatform -lesclvdec -lesclvenc -lescllogger -les_host_query_cap -les-ipcm" --enable-avfilter --disable-version3 --enable-logging --enable-optimizations --disable-extra-warnings --enable-avdevice --enable-avcodec --enable-avformat --enable-network --disable-gray --enable-swscale-alpha --disable-small --disable-dxva2 --enable-runtime-cpudetect --disable-hardcoded-tables --disable-mipsdsp --disable-mipsdspr2 --disable-msa --enable-hwaccels --disable-cuda --disable-cuvid --disable-nvenc --disable-avisynth --disable-frei0r --disable-libopencore-amrnb --disable-libopencore-amrwb --disable-libdc1394 --disable-libgsm --disable-libilbc --disable-libvo-amrwbenc --disable-symver --disable-doc --enable-gpl --enable-nonfree --enable-ffmpeg --disable-ffplay --disable-libv4l2 --enable-ffprobe --disable-libxcb --enable-postproc --enable-swscale --enable-indevs --disable-alsa --enable-outdevs --enable-esmpp --enable-pthreads --enable-zlib --enable-bzlib --disable-libfdk-aac --disable-libcdio --disable-gnutls --disable-libdrm --disable-libopenh264 --disable-vaapi --disable-vdpau --disable-mmal --disable-omx --disable-omx-rpi --disable-libopencv --disable-libopus --disable-libvpx --disable-libass --disable-libbluray --disable-libvpl --disable-libmfx --disable-librtmp --disable-libmp3lame --disable-libmodplug --disable-libspeex --disable-libtheora --disable-iconv --disable-libfreetype --disable-fontconfig --disable-libopenjpeg --enable-libx264 --enable-libx265 --disable-libdav1d --disable-x86asm --disable-mmx --disable-sse --disable-sse2 --disable-sse3 --disable-ssse3 --disable-sse4 --disable-sse42 --disable-avx --disable-avx2 --disable-armv6 --disable-armv6t2 --disable-vfp --disable-neon --disable-altivec --enable-pic
fi
make
make install
