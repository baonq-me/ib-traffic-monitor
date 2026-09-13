#!/bin/bash
# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
#
# Build a .deb package containing the ib-traffic-monitor binary and the
# systemd service for its headless Prometheus exporter mode.
#
# usage: packaging/build-deb.sh [output directory]
#
# environment:
#   MAINTAINER    maintainer field of the package
#   ARCH          target architecture, defaults to the host architecture

set -euo pipefail

PACKAGE="ib-traffic-monitor"
SOURCE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUTPUT_DIR="$(cd "${1:-${SOURCE_DIR}/dist}" 2>/dev/null || { mkdir -p "${1:-${SOURCE_DIR}/dist}"; cd "${1:-${SOURCE_DIR}/dist}"; }; pwd)"
MAINTAINER="${MAINTAINER:-baonq-me <quocbao747@gmail.com>}"

for command_name in dpkg-deb install make sed; do
    if ! command -v "${command_name}" > /dev/null 2>&1; then
        echo "ERROR: ${command_name} is required to build the package" >&2
        exit 1
    fi
done

ARCH="${ARCH:-$(dpkg --print-architecture)}"

# the version of the package is the version of the program
VERSION=""

for version_file in "${SOURCE_DIR}/version.h" "${SOURCE_DIR}/ib-traffic-monitor.c"; do
    if [ -f "${version_file}" ]; then
        VERSION="$(sed -n 's/^#define VERSION "\(.*\)"$/\1/p' "${version_file}")"
    fi

    if [ -n "${VERSION}" ]; then
        break
    fi
done

if [ -z "${VERSION}" ]; then
    echo "ERROR: unable to read the VERSION define of the program" >&2
    exit 1
fi

STAGE_DIR="$(mktemp -d)"
trap 'rm -rf "${STAGE_DIR}"' EXIT

# mktemp creates the directory 0700, the root of the package must be 0755
chmod 0755 "${STAGE_DIR}"

echo "building ${PACKAGE} ${VERSION} for ${ARCH}"

# release build: no sanitizer, hardening flags on
make -C "${SOURCE_DIR}" release

# lay out the file system tree of the package
make -C "${SOURCE_DIR}" install DESTDIR="${STAGE_DIR}"

install -d "${STAGE_DIR}/usr/share/doc/${PACKAGE}"
install -m 0644 "${SOURCE_DIR}/packaging/debian/copyright" "${STAGE_DIR}/usr/share/doc/${PACKAGE}/copyright"

cat > "${STAGE_DIR}/usr/share/doc/${PACKAGE}/changelog.Debian" <<EOF
${PACKAGE} (${VERSION}) unstable; urgency=medium

  * Package built from the upstream source tree, see the ChangeLog section of
    /usr/share/doc/${PACKAGE}/README.md for the list of changes.

 -- ${MAINTAINER}  $(date -R)
EOF

gzip -9n "${STAGE_DIR}/usr/share/doc/${PACKAGE}/changelog.Debian"
install -m 0644 "${SOURCE_DIR}/README.md" "${STAGE_DIR}/usr/share/doc/${PACKAGE}/README.md"

# resolve the shared library dependencies of the binary, and fall back to the
# libraries the program is known to link against when that is not possible
DEPENDS="libc6, libncurses6 | libncurses5, libtinfo6 | libtinfo5"

if command -v dpkg-shlibdeps > /dev/null 2>&1; then
    SHLIBDEPS_DIR="${STAGE_DIR}/.shlibdeps"
    mkdir -p "${SHLIBDEPS_DIR}/debian"
    touch "${SHLIBDEPS_DIR}/debian/control"

    if RESOLVED_DEPENDS="$(cd "${SHLIBDEPS_DIR}" && dpkg-shlibdeps -O --ignore-missing-info \
        "${STAGE_DIR}/usr/bin/${PACKAGE}" 2>/dev/null | sed -n 's/^shlibs:Depends=//p')"; then
        if [ -n "${RESOLVED_DEPENDS}" ]; then
            DEPENDS="${RESOLVED_DEPENDS}"
        fi
    fi

    rm -rf "${SHLIBDEPS_DIR}"
fi

echo "dependencies: ${DEPENDS}"

# installed size in KiB, as reported to the package manager
INSTALLED_SIZE="$(du -ks "${STAGE_DIR}" | cut -f1)"

install -d "${STAGE_DIR}/DEBIAN"

sed -e "s|@VERSION@|${VERSION}|" \
    -e "s|@ARCH@|${ARCH}|" \
    -e "s|@MAINTAINER@|${MAINTAINER}|" \
    -e "s|@INSTALLED_SIZE@|${INSTALLED_SIZE}|" \
    -e "s|@DEPENDS@|${DEPENDS}|" \
    "${SOURCE_DIR}/packaging/debian/control.in" > "${STAGE_DIR}/DEBIAN/control"

# /etc/default/ib-traffic-monitor is edited by the administrator
echo "/etc/default/${PACKAGE}" > "${STAGE_DIR}/DEBIAN/conffiles"

for script_name in postinst prerm postrm; do
    install -m 0755 "${SOURCE_DIR}/packaging/debian/${script_name}" "${STAGE_DIR}/DEBIAN/${script_name}"
done

PACKAGE_PATH="${OUTPUT_DIR}/${PACKAGE}_${VERSION}_${ARCH}.deb"

dpkg-deb --build --root-owner-group "${STAGE_DIR}" "${PACKAGE_PATH}"

echo
echo "package: ${PACKAGE_PATH}"
echo "install: sudo apt install ${PACKAGE_PATH}"
