#!/bin/bash

case "${1,,}" in
	x64|x86_64|amd64) CPU_ARCH="x86_64"; LDCONFIG_ARCH="x86-64" ;;
	arm64|aarch64) CPU_ARCH="aarch64"; LDCONFIG_ARCH="AArch64" ;;
	*) echo "usage: $0 x64|arm64" >&2; exit 1 ;;
esac

if [[ -z "${GITHUB_WORKSPACE}" ]]; then
	export GITHUB_WORKSPACE="."
fi

curl -sSfLO "https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-"${CPU_ARCH}".AppImage"
chmod a+x linuxdeploy*.AppImage
curl -sSfL https://github.com"$(curl https://github.com/probonopd/go-appimage/releases/expanded_assets/continuous | grep "mkappimage-.*-"${CPU_ARCH}".AppImage" | head -n 1 | cut -d '"' -f 2)" -o mkappimage.AppImage
chmod a+x mkappimage.AppImage
curl -sSfLO "https://raw.githubusercontent.com/linuxdeploy/linuxdeploy-plugin-gtk/master/linuxdeploy-plugin-gtk.sh"
chmod a+x linuxdeploy-plugin-gtk.sh
curl -sSfLO "https://github.com/darealshinji/linuxdeploy-plugin-checkrt/releases/download/continuous/linuxdeploy-plugin-checkrt.sh"
chmod a+x linuxdeploy-plugin-checkrt.sh

if [[ ! -e /usr/lib/"${CPU_ARCH}"-linux-gnu ]]; then
	sed -i 's#lib\/"${CPU_ARCH}"-linux-gnu#lib64#g' linuxdeploy-plugin-gtk.sh
fi

mkdir -p AppDir/usr/bin
mkdir -p AppDir/usr/share/Cemu
mkdir -p AppDir/usr/share/applications
mkdir -p AppDir/usr/share/icons/hicolor/128x128/apps
mkdir -p AppDir/usr/share/metainfo
mkdir -p AppDir/usr/lib

cp dist/linux/info.cemu.Cemu.{desktop,png} AppDir/
cp dist/linux/info.cemu.Cemu.metainfo.xml AppDir/usr/share/metainfo/info.cemu.Cemu.appdata.xml

cp -r bin/* AppDir/usr/share/Cemu

mv AppDir/usr/share/Cemu/Cemu_release AppDir/usr/bin/Cemu
chmod +x AppDir/usr/bin/Cemu

# Bundle a few libraries that some target distros lack. Library names and
# locations differ between distros (Debian: /usr/lib/<arch>-linux-gnu, Arch: /usr/lib),
# so look each one up by its base name and copy whatever version this system has.
for lib in libsepol libffi libpcre libGLU libthai; do
	path="$(ldconfig -p | grep -E "^\s+${lib}\.so\.[0-9]+ .*${LDCONFIG_ARCH}" | head -n 1 | awk '{print $NF}')"
	if [[ -n "${path}" ]]; then
		cp -L "${path}" AppDir/usr/lib/
	else
		echo "note: ${lib} not found, not bundled"
	fi
done

export UPD_INFO="gh-releases-zsync|cemu-project|Cemu|ci|Cemu.AppImage.zsync"
export NO_STRIP=1
./linuxdeploy-"${CPU_ARCH}".AppImage --appimage-extract-and-run \
  --appdir="${GITHUB_WORKSPACE}"/AppDir/ \
  -d "${GITHUB_WORKSPACE}"/AppDir/info.cemu.Cemu.desktop \
  -i "${GITHUB_WORKSPACE}"/AppDir/info.cemu.Cemu.png \
  -e "${GITHUB_WORKSPACE}"/AppDir/usr/bin/Cemu \
  --plugin checkrt

if ! GITVERSION="$(git rev-parse --short HEAD 2>/dev/null)"; then
	GITVERSION=experimental
fi
echo "Cemu Version Cemu-${GITVERSION}"

rm -f AppDir/usr/lib/libwayland-client.so.0
echo -e "export LC_ALL=C\nexport FONTCONFIG_PATH=/etc/fonts" >> AppDir/apprun-hooks/linuxdeploy-plugin-gtk.sh
VERSION="${GITVERSION}" ./mkappimage.AppImage --appimage-extract-and-run "${GITHUB_WORKSPACE}"/AppDir

mkdir -p "${GITHUB_WORKSPACE}"/artifacts/
mv Cemu-"${GITVERSION}"-"${CPU_ARCH}".AppImage "${GITHUB_WORKSPACE}"/artifacts/
