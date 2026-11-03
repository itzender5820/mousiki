# AUR package (Arch Linux)

`PKGBUILD` builds Mousiki Plus from the GitHub release tag `v<pkgver>` and installs it like the `.deb` does: the program,
`scripts/` (with `fpcalc`), `stations.txt` and `radio_config.txt` in `/usr/lib/mousiki`, and the two commands `mousiki` and
`lala` as links in `/usr/bin`. No Arch computer is needed to test it: Docker is enough.

## Test it (Windows PowerShell, macOS, Linux; Docker running)

```
cd packaging/aur
docker run --rm -it -v "${PWD}:/pkg" archlinux bash /pkg/run-in-container.sh
```

(`${PWD}` is PowerShell; in a Linux / macOS shell use `"$PWD"`. In Git Bash on Windows put `MSYS_NO_PATHCONV=1` in front of
`docker`, otherwise Git Bash rewrites `/pkg` into `C:/Program Files/Git/pkg`.) It takes some minutes: it updates a fresh Arch container,
builds the package, checks it with `namcap`, installs it and starts `mousiki --version`. Afterwards the folder `out/` holds
the `PKGBUILD` with the real checksum filled in, the `.SRCINFO` and the finished `.pkg.tar.zst`.

## Publish it (once an AUR account can be made)

1. Account on <https://aur.archlinux.org>, the public SSH key in the profile.
2. Replace the maintainer line at the top of the `PKGBUILD`.
3. ```
   git clone ssh://aur@aur.archlinux.org/mousiki-plus.git aur-mousiki-plus
   cp out/PKGBUILD out/.SRCINFO aur-mousiki-plus/
   cd aur-mousiki-plus
   git add PKGBUILD .SRCINFO && git commit -m "mousiki-plus 4.9.0" && git push
   ```

## For every new release

Change `pkgver` (and set `pkgrel=1`), run the test above again, copy `out/PKGBUILD` and `out/.SRCINFO` into the AUR clone,
commit and push. (A bug in the packaging alone: raise `pkgrel` instead.)

## Good to know

- The package name `mousiki-plus` could get a `-git` sibling (building the newest commit) or a `-bin` one (repacking the
  Linux portable zip); this one is the plain "build the release" package.
- Someone who uses Arch can take over the maintenance: the AUR lets a maintainer hand a package over.
