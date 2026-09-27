#!/usr/bin/env bash
# Packages the macOS build: a zip of the AU + VST3 bundles and an installer
# (.pkg) that puts them in /Library/Audio/Plug-Ins.
#
#   scripts/package_macos.sh <artefacts dir> <output dir>
#
# e.g. scripts/package_macos.sh build/plugin/MinervaSpaceEcho_artefacts/Release dist
#
# Signing is optional and driven by the environment:
#   MACOS_SIGN_APP        "Developer ID Application: Name (TEAMID)"  -> codesign the bundles
#   MACOS_SIGN_INSTALLER  "Developer ID Installer: Name (TEAMID)"    -> sign the .pkg
#   APPLE_ID, APPLE_TEAM_ID, APPLE_APP_PASSWORD                       -> notarize and staple
# Without them the bundles are ad-hoc signed and nothing is notarized.
set -euo pipefail

ART="${1:?artefacts dir}"
mkdir -p "${2:?output dir}"
OUT="$(cd "$2" && pwd)"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
VERSION="$(sed -n 's/^project(MinervaSpaceEcho VERSION \([0-9.]*\).*/\1/p' "$ROOT/CMakeLists.txt")"
NAME="MINERVA Space Echo"
ID="com.crumplab.minervaspaceecho"
STEM="MinervaSpaceEcho-$VERSION-macOS"

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
mkdir -p "$WORK/au" "$WORK/vst3" "$WORK/zip" "$WORK/resources"
cp -R "$ART/AU/$NAME.component" "$WORK/au/"
cp -R "$ART/VST3/$NAME.vst3" "$WORK/vst3/"

# ---- sign the bundles --------------------------------------------------------
sign_bundle() {
    if [[ -n "${MACOS_SIGN_APP:-}" ]]; then
        codesign --force --deep --options runtime --timestamp --sign "$MACOS_SIGN_APP" "$1"
    else
        codesign --force --deep --sign - "$1"
    fi
    codesign --verify --deep --strict "$1"
}
sign_bundle "$WORK/au/$NAME.component"
sign_bundle "$WORK/vst3/$NAME.vst3"

notarize() { # file to submit
    xcrun notarytool submit "$1" --apple-id "$APPLE_ID" --team-id "$APPLE_TEAM_ID" \
        --password "$APPLE_APP_PASSWORD" --wait
}
NOTARIZE=0
if [[ -n "${MACOS_SIGN_APP:-}" && -n "${APPLE_ID:-}" && -n "${APPLE_TEAM_ID:-}" && -n "${APPLE_APP_PASSWORD:-}" ]]; then
    NOTARIZE=1
    # Notarize the bundles (submitted as a zip), then staple the tickets to them.
    ditto -c -k --keepParent "$WORK/au/$NAME.component" "$WORK/au.zip"
    ditto -c -k --keepParent "$WORK/vst3/$NAME.vst3" "$WORK/vst3.zip"
    notarize "$WORK/au.zip"
    notarize "$WORK/vst3.zip"
    xcrun stapler staple "$WORK/au/$NAME.component"
    xcrun stapler staple "$WORK/vst3/$NAME.vst3"
fi

# ---- zip (manual install) ------------------------------------------------------
cp -R "$WORK/au/$NAME.component" "$WORK/vst3/$NAME.vst3" "$WORK/zip/"
cp "$ROOT/README.md" "$ROOT/LICENSE" "$ROOT/CHANGELOG.md" "$WORK/zip/"
( cd "$WORK/zip" && ditto -c -k --sequesterRsrc . "$OUT/$STEM.zip" )

# ---- installer ------------------------------------------------------------------
component_pkg() { # root, install location, identifier, output
    pkgbuild --analyze --root "$1" "$WORK/components.plist"
    # Install exactly where we say, even if a copy of the bundle exists elsewhere.
    /usr/libexec/PlistBuddy -c "Set :0:BundleIsRelocatable false" "$WORK/components.plist"
    pkgbuild --root "$1" --component-plist "$WORK/components.plist" --install-location "$2" \
        --identifier "$3" --version "$VERSION" "$4"
}
component_pkg "$WORK/au" "/Library/Audio/Plug-Ins/Components" "$ID.au" "$WORK/au.pkg"
component_pkg "$WORK/vst3" "/Library/Audio/Plug-Ins/VST3" "$ID.vst3" "$WORK/vst3.pkg"

cp "$ROOT/LICENSE" "$WORK/resources/LICENSE.txt"
cat > "$WORK/resources/welcome.txt" <<EOF
MINERVA Space Echo $VERSION

A multiple-trace memory echo (Hintzman's MINERVA II as a tape echo).
This installs the Audio Unit and VST3 plug-ins into /Library/Audio/Plug-Ins.
In Live: Settings > Plug-Ins, then Rescan. The plug-in appears under CrumpLab.
EOF
sed -e "s/@VERSION@/$VERSION/g" -e "s/@ID@/$ID/g" "$ROOT/scripts/distribution.xml" > "$WORK/distribution.xml"

SIGN_ARGS=()
if [[ -n "${MACOS_SIGN_INSTALLER:-}" ]]; then
    SIGN_ARGS=(--sign "$MACOS_SIGN_INSTALLER" --timestamp)
fi
productbuild --distribution "$WORK/distribution.xml" --resources "$WORK/resources" --package-path "$WORK" \
    ${SIGN_ARGS[@]+"${SIGN_ARGS[@]}"} "$OUT/$STEM.pkg"

if [[ "$NOTARIZE" == 1 && -n "${MACOS_SIGN_INSTALLER:-}" ]]; then
    notarize "$OUT/$STEM.pkg"
    xcrun stapler staple "$OUT/$STEM.pkg"
fi

SIGNED=ad-hoc
[[ -n "${MACOS_SIGN_APP:-}" ]] && SIGNED="Developer ID"
echo "Packaged $OUT/$STEM.zip and $OUT/$STEM.pkg (signing: $SIGNED, notarized: $NOTARIZE)"
