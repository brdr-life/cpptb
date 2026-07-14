# Sphinx configuration

Sphinx reads the shared Markdown from `docs/` while its configuration lives
outside that content tree so other renderers do not publish it. From the
repository root, build or preview through the standard Make targets:

```sh
make docs-sphinx-build
make docs-sphinx-serve
```

Set `SPHINX_HTML_BASE_URL` to the published documentation URL when deploying.
The `sphinx.ext.githubpages` extension writes the `.nojekyll` file required by
GitHub Pages.
