#!/usr/bin/env bash
#
# Builds the macOS installer package.
#
# The point of shipping a .pkg rather than a .zip is not polish, it is that the
# installed bundles arrive WITHOUT com.apple.quarantine. Apple's rule is that a
# host may load a quarantined plug-in only if that plug-in is notarised, and
# Resolume does not surface the "allow anyway" prompt Apple documents — the
# plugin simply never appears. A .zip propagates quarantine to everything it
# extracts, which is why the old instructions had to teach `xattr -dr`. Installer
# payloads are laid down by installd, which is not quarantine-aware, so the same
# binaries delivered this way just work.
#
# Signing is therefore an improvement, not a prerequisite: unsigned, the user
# clears one Gatekeeper dialog and the plugins still load afterwards. Signed and
# notarised, there is no dialog. Both paths are supported so that forks and pull
# requests — which cannot see the secrets — still produce a working installer.
#
# Usage: build-pkg.sh <payload-dir> <version> <output.pkg>
#   payload-dir must contain Datamosh.bundle and DatamoshTransplant.bundle
#
set -euo pipefail

PAYLOAD="${1:?payload directory required}"
VERSION="${2:?version required}"
OUTPUT="${3:?output path required}"

HERE="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
WORK="$( mktemp -d )"
trap 'rm -rf "$WORK"; security delete-keychain "$KEYCHAIN" 2>/dev/null || true' EXIT

KEYCHAIN="datamosh-build.keychain"
IDENTIFIER_BASE="ie.letissier.datamosh"

say() { printf '\n\033[1m==> %s\033[0m\n' "$*"; }

# --- signing identities, if we have them ----------------------------------
# Absent secrets are a supported configuration, not an error: the package is
# still the thing that solves the quarantine problem.
SIGN_APP=""
SIGN_PKG=""

if [[ -n "${MACOS_CERT_P12:-}" && -n "${MACOS_CERT_PASSWORD:-}" ]]; then
	say "Importing signing identities"
	KEYCHAIN_PASSWORD="$( uuidgen )"
	security create-keychain -p "$KEYCHAIN_PASSWORD" "$KEYCHAIN"
	security set-keychain-settings -lut 21600 "$KEYCHAIN"
	security unlock-keychain -p "$KEYCHAIN_PASSWORD" "$KEYCHAIN"

	echo "$MACOS_CERT_P12" | base64 --decode > "$WORK/certs.p12"
	security import "$WORK/certs.p12" -k "$KEYCHAIN" -P "$MACOS_CERT_PASSWORD" \
		-T /usr/bin/codesign -T /usr/bin/productbuild
	# Without this, codesign blocks on a GUI prompt that will never be answered
	# and the job hangs until it times out rather than failing.
	security set-key-partition-list -S apple-tool:,apple: -s -k "$KEYCHAIN_PASSWORD" "$KEYCHAIN" >/dev/null
	security list-keychains -d user -s "$KEYCHAIN" $( security list-keychains -d user | tr -d '"' )

	SIGN_APP="$( security find-identity -v -p codesigning "$KEYCHAIN" | grep "Developer ID Application" | head -1 | sed -E 's/.*"(.+)"/\1/' || true )"
	SIGN_PKG="$( security find-identity -v "$KEYCHAIN" | grep "Developer ID Installer" | head -1 | sed -E 's/.*"(.+)"/\1/' || true )"

	[[ -n "$SIGN_APP" ]] && echo "    application: $SIGN_APP" || echo "    no Developer ID Application identity found"
	[[ -n "$SIGN_PKG" ]] && echo "    installer:   $SIGN_PKG" || echo "    no Developer ID Installer identity found"
else
	say "No signing secrets present — building an unsigned package"
fi

# --- sign the bundles -----------------------------------------------------
if [[ -n "$SIGN_APP" ]]; then
	say "Signing the plugin bundles"
	for bundle in "$PAYLOAD"/*.bundle; do
		# --options runtime is required for notarisation. Resolume must have
		# library validation disabled to load any third-party plugin at all —
		# unsigned ones currently work, which is only possible without it — so
		# signing under our own team ID does not lock us out of the host.
		codesign --force --timestamp --options runtime \
			--sign "$SIGN_APP" "$bundle"
		codesign --verify --strict --verbose=2 "$bundle"
	done
fi

# --- quarantine hygiene ---------------------------------------------------
# Extended attributes are stored verbatim in a package payload and restored on
# install. If anything in the staging tree carries com.apple.quarantine, every
# user who installs inherits it and the plugin silently never loads — the exact
# failure this package exists to prevent, reintroduced invisibly.
say "Clearing extended attributes from the payload"
xattr -cr "$PAYLOAD"

REMAINING="$( xattr -lr "$PAYLOAD" | grep -c 'com.apple.quarantine' || true )"
if [[ "$REMAINING" != "0" ]]; then
	echo "error: payload still carries com.apple.quarantine after xattr -cr" >&2
	xattr -lr "$PAYLOAD" >&2
	exit 1
fi

# --- component packages ---------------------------------------------------
# Arena and Avenue read from separate, product-named folders under the user's
# Documents. There is no system-wide plugin location, so both are user-domain
# installs and neither needs elevation. Building one component per product lets
# the installer show them as tickboxes.
say "Building component packages"
for product in Arena Avenue; do
	# Lowercased with tr rather than ${product,,} — macOS ships bash 3.2 as
	# /bin/bash and the ,, expansion is a bash 4 feature that fails to parse
	# there, taking the whole script with it.
	slug="$( printf '%s' "$product" | tr '[:upper:]' '[:lower:]' )"
	pkgbuild \
		--root "$PAYLOAD" \
		--identifier "${IDENTIFIER_BASE}.${slug}" \
		--version "$VERSION" \
		--install-location "Documents/Resolume ${product}/Extra Effects" \
		"$WORK/datamosh-${slug}.pkg"
done

# --- distribution ---------------------------------------------------------
say "Building the distribution package"
sed -e "s/@VERSION@/$VERSION/g" "$HERE/distribution.xml" > "$WORK/distribution.xml"

# Assembled here rather than checked in, so the licence shown by the installer
# is the repo's actual LICENSE and cannot drift away from it.
mkdir -p "$WORK/resources"
cp "$HERE/resources"/*.html "$WORK/resources/"
cp "$HERE/../../LICENSE" "$WORK/resources/LICENSE.txt"

PRODUCTBUILD_ARGS=(
	--distribution "$WORK/distribution.xml"
	--package-path "$WORK"
	--resources "$WORK/resources"
)
[[ -n "$SIGN_PKG" ]] && PRODUCTBUILD_ARGS+=( --sign "$SIGN_PKG" )

productbuild "${PRODUCTBUILD_ARGS[@]}" "$OUTPUT"

# --- notarise -------------------------------------------------------------
# Only worth doing if the package is signed; notarytool rejects unsigned input.
if [[ -n "$SIGN_PKG" && -n "${APPLE_API_KEY_P8:-}" && -n "${APPLE_API_KEY_ID:-}" && -n "${APPLE_API_ISSUER_ID:-}" ]]; then
	say "Notarising"
	echo "$APPLE_API_KEY_P8" | base64 --decode > "$WORK/key.p8"

	xcrun notarytool submit "$OUTPUT" \
		--key "$WORK/key.p8" \
		--key-id "$APPLE_API_KEY_ID" \
		--issuer "$APPLE_API_ISSUER_ID" \
		--wait --timeout 30m

	say "Stapling"
	# Without the staple the ticket only exists on Apple's servers, so a user
	# installing offline — or behind a firewall that blocks the OCSP lookup —
	# gets the Gatekeeper dialog anyway.
	xcrun stapler staple "$OUTPUT"
	xcrun stapler validate "$OUTPUT"
else
	say "Skipping notarisation (needs a signed package and API key secrets)"
fi

# --- report ---------------------------------------------------------------
say "Result"
pkgutil --check-signature "$OUTPUT" || true
echo
echo "Package contents:"
pkgutil --payload-files "$WORK/datamosh-arena.pkg" | head -20
