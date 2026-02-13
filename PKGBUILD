# Maintainer: paco3346 <paco3346@gmail.com>
pkgname=fw16-kbd-uleds-git
pkgver=0
pkgrel=1
pkgdesc="Framework 16 keyboard uleds bridge to qmk_hid (framework::kbd_backlight) - Git version"
arch=('x86_64')
url="https://github.com/paco3346/fw16-kbd-uleds"
license=('MIT')
depends=('glibc' 'systemd' 'qmk-hid')
makedepends=('git')
provides=("${pkgname%-git}")
conflicts=("${pkgname%-git}")
source=("${pkgname%-git}::git+${url}.git")
sha256sums=('SKIP')

pkgver() {
  cd "${pkgname%-git}"
  printf "r%s.%s" "$(git rev-list --count HEAD)" "$(git rev-parse --short HEAD)"
}

build() {
  cd "${pkgname%-git}"
  make CFLAGS="${CFLAGS}" LDFLAGS="${LDFLAGS}"
}

package() {
  cd "${pkgname%-git}"
  make DESTDIR="${pkgdir}" PREFIX=/usr install
}
