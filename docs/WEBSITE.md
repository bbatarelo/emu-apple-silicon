# Project website

The landing page lives in `site/`. It uses plain HTML and CSS, with no JavaScript,
tracking, external fonts, or runtime dependencies. The Python standard-library
builder reads `VERSION` and generates `build/site/`, including the canonical URL,
social metadata, SoftwareSourceCode structured data, and sitemap. Do not edit or commit generated files.

## Preview locally

From the repository root, with Python 3 installed:

```sh
python3 scripts/build-site.py
python3 -m http.server 8000 --directory build/site
```

Open http://localhost:8000. Stop the server with Ctrl-C.

## Enable publishing once

In GitHub, open **Settings → Pages → Build and deployment → Source** and select
**GitHub Actions**. After this workflow reaches `main`, use **Actions → Project
website → Run workflow** if an initial deployment is needed. Subsequent changes
to the website, builder, workflow, or VERSION on main publish automatically.
Pull requests build the page but do not deploy it.

The default address is https://bbatarelo.github.io/emu-apple-silicon/.
The workflow reads the configured Pages URL, so a future custom domain also
updates the canonical URL and sitemap. For a local custom-domain preview:

```sh
python3 scripts/build-site.py --url https://emu.batarelo.net
```

Once published, add the website URL to the repository About field. In Google
Search Console, create a URL-prefix property for the exact website address and
verify it with Google's HTML meta tag in `site/index.html`. Deploy the tag before
clicking Verify. Submit `sitemap.xml` and request indexing of the landing page.
There is no guarantee of indexing or ranking; no Google account or verification
is configured by this repository itself. A project-path robots.txt would not
control the domain, so the builder deliberately does not generate one.

## Maintenance

The version comes from VERSION. Keep compatibility, high-rate caveats, and build
instructions consistent with README and the implementation. Do not describe a
source release as a compiled installer. Site-only changes do not alter the driver;
follow the repository version policy before merging. Test the page at narrow and
wide viewport sizes, and check keyboard navigation when changing layout.

Photograph attribution is in `site/assets/ATTRIBUTION.md` and visible on the page.
Keep those credits when replacing or reusing the photograph.
