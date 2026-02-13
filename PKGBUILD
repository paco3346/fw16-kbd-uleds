# Maintainer: paco3346 <paco3346@gmail.com>
pkgname=fw16-kbd-uleds
pkgver=0.1.0
pkgrel=1
pkgdesc="Framework 16 keyboard uleds bridge to qmk_hid (framework::kbd_backlight)"
arch=('x86_64')
url="https://github.com/paco3346/fw16-kbd-uleds"
license=('MIT')
depends=('glibc' 'systemd' 'qmk-hid')
makedepends=('gcc' 'make')
source=("https://github.com/paco3346/fw16-kbd-uleds/archive/refs/tags/v${pkgver}.tar.gz")
sha256sums=('SKIP')

_srcdir="${pkgname}-${pkgver}"

build() {
  cd "${srcdir}/${_srcdir}"
  make CFLAGS="${CFLAGS}" LDFLAGS="${LDFLAGS}"
}

package() {
  cd "${srcdir}/${_srcdir}"
  make DESTDIR="${pkgdir}" PREFIX=/usr install

  install -Dm644 LICENSE "${pkgdir}/usr/share/licenses/${pkgname}/LICENSE"
}
