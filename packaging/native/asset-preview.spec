Name: asset-preview
Version: @VERSION@
Release: 1
Summary: Live asset previews for AI agent workflows
License: MIT AND LicenseRef-Bundled-Dependencies
URL: https://github.com/RaresKeY/asset-preview
BuildArch: x86_64
Requires: glibc >= 2.39
Requires: libGL.so.1()(64bit)
Requires: libEGL.so.1()(64bit)
Requires: libOpenGL.so.0()(64bit)
Requires: libvulkan.so.1()(64bit)
Requires: fontconfig
Recommends: dejavu-sans-fonts
AutoReqProv: no
%global debug_package %{nil}
%global __os_install_post %{nil}
%description
A local Qt asset viewer and agent command client. Private dependency libraries
are installed under /opt/asset-preview. Notices and exact corresponding sources
are delivered with the release. Requires Linux graphics drivers and X11/XWayland.
%install
mkdir -p %{buildroot}
cp -a @PAYLOAD@/. %{buildroot}/
%files
/opt/asset-preview
/usr/bin/asset-preview
/usr/share/applications/io.github.RaresKeY.AssetPreview.desktop
/usr/share/icons/hicolor/scalable/apps/io.github.RaresKeY.AssetPreview.svg
/usr/share/metainfo/io.github.RaresKeY.AssetPreview.metainfo.xml
%license /usr/share/licenses/asset-preview/LICENSE
/usr/share/doc/asset-preview/THIRD_PARTY_NOTICES.md
