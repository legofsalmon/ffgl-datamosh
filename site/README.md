# The documentation site

Four static pages, no build step, no framework, no dependencies. Deployed to
Vercel from this folder.

```
index.html          landing page — includes the live canvas demo
parameters.html     every parameter, what it does, what it fights with
recipes.html        settings for each look, plus the live workflows
architecture.html   pipeline internals and the FFGL traps
404.html            served automatically by Vercel for unknown paths

style.css           shared stylesheet, light and dark
demo.js             the hero canvas demo
favicon.svg
og.png              link preview image — generated, see below
vercel.json         headers and caching
robots.txt
sitemap.xml
tools/make-og.py    regenerates og.png
```

## The palette

One palette, dark, declared on bare `:root` in `style.css`. The site does not
follow the operating system's light/dark setting, and there is no toggle.

Everything downstream reads tokens — there is not a single colour literal in the
stylesheet outside that block, and `demo.js` samples `--surface-sunk`, `--ink`
and `--accent` off the root element rather than carrying its own. Retinting the
site is editing eleven lines in one place.

Two things carry fixed colours on purpose: `favicon.svg`, because it is
composited against browser chrome nobody controls, and `og.png`, because it is
an image. Both are already built on the same dark ground.

## Vercel settings

| Setting | Value |
| --- | --- |
| Root Directory | `site` |
| Framework Preset | Other |
| Build Command | *(none)* |
| Output Directory | *(none — serve the root directory as-is)* |

`vercel.json` is read from the Root Directory, so it applies once the project
points here. It sets security headers, a real Content-Security-Policy, and a
ten-minute cache with a week of `stale-while-revalidate` on the assets.

The assets are deliberately **not** cached as `immutable`: their filenames are
not content-hashed, so an edit to `style.css` would otherwise be invisible to
anyone who had already loaded the site.

## Working on it locally

The pages are plain files and link to each other with `.html`, so they work when
opened straight from disk. To exercise it the way Vercel serves it:

```sh
cd site && python3 -m http.server 8000
```

## The domain

`https://datamosh.letissier.ie`.

It appears in exactly one block per page — the `canonical` and Open Graph tags
in `<head>`, marked with a comment — plus `sitemap.xml` and `robots.txt`. To
move the site:

```sh
cd site
sed -i 's|https://datamosh\.letissier\.ie|https://new-domain|g' *.html *.xml *.txt
```

`og:image` must be an absolute URL that actually resolves or shared links will
not unfurl with a preview image, so this is worth getting right rather than
leaving stale.

Three files outside this folder also carry the URL:

- `README.md`
- `packaging/RELEASE-NOTES.md` — becomes the GitHub Release body
- `packaging/INSTALL-macos.txt` and `packaging/INSTALL-windows.txt` — **copied
  into every release archive**, so whatever URL they carry is frozen for that
  version the moment it is cut. Moving the domain means cutting a release.

## Regenerating the preview image

```sh
python3 site/tools/make-og.py
```

Pure standard library — `zlib` and `struct` are enough to write a PNG, so there
is nothing to install. It renders the same idea as the plugin's own thumbnails:
a clean frame above the line, and below it the same frame carried sideways in
macroblocks by motion that no longer matches its content.

The script is in the repo rather than just the image because a binary nobody can
reproduce is a binary nobody can change.
