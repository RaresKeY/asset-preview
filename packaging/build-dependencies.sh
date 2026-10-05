#!/usr/bin/env bash
set -euo pipefail
export PKG_CONFIG_PATH=/opt/dependencies/lib/pkgconfig
export LD_LIBRARY_PATH=/opt/dependencies/lib
mkdir -p /dependency-sources /opt/dependencies
cd /dependency-sources
stage="${1:-all}"
if [[ "$stage" = prepare ]]; then
fetch() { curl --fail --location --retry 3 "$2" -o "$1"; echo "$3  $1" | sha256sum -c -; }
# Hashes identify the exact unmodified source archives retained with each release.
fetch ffmpeg.tar.xz https://ffmpeg.org/releases/ffmpeg-7.1.5.tar.xz de668509caf9e35e3cd162473441fdb29538c6d96ed080292b3cf9e6fc5d558f
fetch mpv.tar.gz https://github.com/mpv-player/mpv/archive/refs/tags/v0.41.0.tar.gz ee21092a5ee427353392360929dc64645c54479aefdb5babc5cfbb5fad626209
fetch f3d.tar.gz https://github.com/f3d-app/f3d/archive/refs/tags/v3.5.0.tar.gz 033845b5d49af3ae60fcc3fe85d82c841d990d3534638a4472123f84b3e82795
fetch vtk.tar.gz https://www.vtk.org/files/release/9.4/VTK-9.4.2.tar.gz 36c98e0da96bb12a30fe53708097aa9492e7b66d5c3b366e1c8dc251e2856a02
mkdir ffmpeg mpv f3d vtk
for item in ffmpeg mpv f3d vtk; do tar -xf "$item.tar."* -C "$item" --strip-components=1; done
exit 0
fi
if [[ "$stage" = ffmpeg ]]; then
cd ffmpeg
./configure --prefix=/opt/dependencies --enable-shared --disable-static \
 --disable-gpl --disable-nonfree --disable-autodetect --disable-programs \
 --disable-doc --disable-network --disable-avdevice --disable-autodetect \
 --disable-everything --enable-libdav1d \
 --enable-decoder=h264,hevc,mpeg4,mpeg2video,vp8,vp9,libdav1d,gif,png,mjpeg,aac,mp3,flac,vorbis,opus,pcm_s16le,pcm_s24le,pcm_s32le,pcm_f32le,wmv1,wmv2,wmv3,vc1,msmpeg4v3,theora \
 --enable-demuxer=mov,matroska,avi,gif,mpegvideo,mpegts,ogg,flac,wav,mp3,aac,image2,asf \
 --enable-parser=h264,hevc,mpeg4video,mpegvideo,vp8,vp9,aac,mpegaudio,flac,vorbis,opus,av1,png \
 --enable-protocol=file \
 --enable-filter=buffer,buffersink,abuffer,abuffersink,format,aformat,scale,aresample,volume,anull,null,setpts,asetpts
make -j4 && make install
exit 0
fi
if [[ "$stage" = mpv ]]; then
cd mpv
meson setup build --prefix=/opt/dependencies --libdir=lib --buildtype=release \
 -Dgpl=false -Dcplayer=false -Dlibmpv=true -Dauto_features=disabled \
 -Dgl=enabled -Dplain-gl=enabled -Degl=enabled -Dpulse=enabled -Dbuild-date=false
meson compile -C build -j4 && meson install -C build
exit 0
fi
if [[ "$stage" = vtk ]]; then
cd vtk
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
 -DCMAKE_INSTALL_PREFIX=/opt/dependencies -DCMAKE_INSTALL_LIBDIR=lib \
 -DVTK_BUILD_TESTING=OFF -DVTK_BUILD_EXAMPLES=OFF -DVTK_WRAP_PYTHON=OFF \
 -DVTK_GROUP_ENABLE_StandAlone=DONT_WANT -DVTK_GROUP_ENABLE_Rendering=DONT_WANT \
 -DVTK_GROUP_ENABLE_Views=DONT_WANT -DVTK_GROUP_ENABLE_Web=DONT_WANT \
 -DVTK_MODULE_ENABLE_VTK_CommonCore=YES -DVTK_MODULE_ENABLE_VTK_CommonDataModel=YES \
 -DVTK_MODULE_ENABLE_VTK_CommonExecutionModel=YES -DVTK_MODULE_ENABLE_VTK_FiltersGeneral=YES \
 -DVTK_MODULE_ENABLE_VTK_FiltersGeometry=YES -DVTK_MODULE_ENABLE_VTK_ImagingCore=YES \
 -DVTK_MODULE_ENABLE_VTK_ImagingHybrid=YES -DVTK_MODULE_ENABLE_VTK_InteractionStyle=YES \
 -DVTK_MODULE_ENABLE_VTK_InteractionWidgets=YES -DVTK_MODULE_ENABLE_VTK_IOCityGML=YES \
 -DVTK_MODULE_ENABLE_VTK_IOLegacy=YES -DVTK_MODULE_ENABLE_VTK_IOGeometry=YES \
 -DVTK_MODULE_ENABLE_VTK_IOImage=YES -DVTK_MODULE_ENABLE_VTK_IOImport=YES \
 -DVTK_MODULE_ENABLE_VTK_IOPLY=YES -DVTK_MODULE_ENABLE_VTK_IOXML=YES \
 -DVTK_MODULE_ENABLE_VTK_RenderingAnnotation=YES -DVTK_MODULE_ENABLE_VTK_RenderingCore=YES \
 -DVTK_MODULE_ENABLE_VTK_RenderingOpenGL2=YES -DVTK_MODULE_ENABLE_VTK_RenderingVolumeOpenGL2=YES \
 -DVTK_MODULE_ENABLE_VTK_TestingCore=YES -DVTK_MODULE_ENABLE_VTK_RenderingGridAxes=YES \
 -DVTK_USE_X=ON -DVTK_OPENGL_HAS_EGL=ON
cmake --build build --parallel 4 && cmake --install build
exit 0
fi
if [[ "$stage" = f3d ]]; then
cd f3d
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
 -DCMAKE_PREFIX_PATH=/opt/dependencies -DCMAKE_INSTALL_PREFIX=/opt/dependencies \
 -DCMAKE_INSTALL_LIBDIR=lib -DF3D_BUILD_APPLICATION=OFF -DF3D_BINDINGS_PYTHON=OFF \
 -DF3D_PLUGIN_BUILD_ASSIMP=ON -DF3D_PLUGINS_STATIC_BUILD=ON -DBUILD_TESTING=OFF
cmake --build build --parallel 4 && cmake --install build
fi
# Retain configuration commands, not generated object files, in the source release.
