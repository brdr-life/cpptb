---
orphan: true
---

# Documentation hosting

:::{note}
This is a maintainer runbook, not user documentation, so it is deliberately
kept out of the site navigation. The rest of the repository workflow is in
`CONTRIBUTING.md`.
:::

The Sphinx documentation is published to Cloudflare Pages and gated by
Cloudflare Access, so the rendered site stays private while the repository
remains private.

GitHub Pages is not used. A Pages site is served publicly, and restricting one
to authorised users requires GitHub Enterprise Cloud, which does not apply to a
repository owned by a personal account.

Until Cloudflare is configured the deployment step reports why it skipped and
succeeds. The rendered documentation is uploaded by the `Documentation` job as
the `docs-sphinx` artifact on every run regardless, so it is always retrievable
by anyone with repository access.

## One-time setup

Nothing below can be automated from CI, because each step needs an account that
only you can authenticate.

### 1. Create the Pages project

In the Cloudflare dashboard, choose **Workers & Pages**, then **Create**,
**Pages**, **Upload assets**. Name it `cpptb-docs` and create it without
uploading anything; the workflow supplies the content.

Use a different name only if you also set a `CLOUDFLARE_PAGES_PROJECT`
repository variable to match, which the workflow reads.

### 2. Create an API token

Under **My Profile**, **API Tokens**, **Create Token**, start from **Custom
token** and grant exactly:

| Scope | Permission |
| --- | --- |
| Account, Cloudflare Pages | Edit |

Nothing else is required. Prefer a token scoped to the single account that owns
the project.

### 3. Add the repository secrets

```sh
gh secret set CLOUDFLARE_API_TOKEN    # the token from step 2
gh secret set CLOUDFLARE_ACCOUNT_ID   # Workers & Pages sidebar, "Account ID"
```

The next push to `main` deploys. Before that the job skips with a notice.

### 4. Gate the site with Access

A Pages project is world-readable until an Access policy covers it. Do this
before treating the URL as private.

In **Zero Trust**, **Access**, **Applications**, add a **Self-hosted**
application pointing at the project's domain, then attach a policy with action
**Allow** and a rule matching the identities you want, such as specific email
addresses or an email domain. Cloudflare Access is free for up to 50 users.

Verify by opening the URL in a private window: it should redirect to a
Cloudflare login rather than rendering the documentation.

## Local preview

Neither Cloudflare nor CI is needed to read the documentation while working:

```sh
make docs-sphinx-serve    # http://localhost:8001
make docs-zensical-serve  # http://localhost:8002
```

## Deployment behaviour

- Only pushes to `main` deploy. Pull requests build and upload the artifact but
  publish nothing, so review never changes the live site.
- Only the Sphinx variant is published. The Zensical build still runs in CI, so
  a break in it fails the `Documentation` job.
- `make docs-check` builds both variants and is what CI runs, so a documentation
  error fails the build before anything is deployed.
