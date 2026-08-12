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
tools/make-og.py    regenerates og.png
```

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

## Changing the domain

The site's own domain appears in exactly one block per page — the `canonical`
and Open Graph tags in `<head>`, marked with a comment. It is currently
`https://ffgl-datamosh.vercel.app`, which is what Vercel assigns if the project
is named after the repo.

To change it:

```sh
cd site
sed -i 's|https://ffgl-datamosh\.vercel\.app|https://your-domain|g' *.html
```

**Until that is correct, shared links will not unfurl with a preview image** —
`og:image` has to be an absolute URL that resolves, and a wrong one is worse
than none.

Three files outside this folder also reference the docs and currently point at
the GitHub repo rather than the site, because they must never carry a dead link:

- `README.md`
- `packaging/RELEASE-NOTES.md` — becomes the GitHub Release body
- `packaging/INSTALL-macos.txt` and `packaging/INSTALL-windows.txt` — **copied
  into every release archive**, so whatever URL they carry is frozen for that
  version the moment it is cut

Point those at the site once the domain resolves, and cut a release so the next
download carries a working link.

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
