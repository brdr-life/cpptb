"""Sphinx configuration for the cpptb documentation."""

from __future__ import annotations

import os


project = "cpptb"
author = "cpptb contributors"
copyright = "2026, cpptb contributors"
version = "0.1"
release = "0.1.0"

extensions = [
    "myst_parser",
    "sphinx.ext.githubpages",
]

source_suffix = {
    ".md": "markdown",
}
root_doc = "index"
exclude_patterns = [
    "_*",
    "_*/**",
    "**/.DS_Store",
    "**/.ipynb_checkpoints",
]

nitpicky = True
show_warning_types = True

myst_enable_extensions = [
    "colon_fence",
    "deflist",
    "fieldlist",
    "substitution",
    "tasklist",
]
myst_colon_fence_exact_match = True
myst_heading_anchors = 3
myst_links_external_new_tab = True

highlight_language = "text"
pygments_style = "sphinx"
pygments_dark_style = "monokai"

html_theme = "furo"
html_title = "cpptb documentation"
html_baseurl = os.environ.get("SPHINX_HTML_BASE_URL", "")
html_static_path = [
    os.path.abspath(os.path.join(os.path.dirname(__file__), "../../../docs/_assets"))
]
html_css_files = ["code-tabs.css", "roadmap-status.css"]
html_js_files = ["code-tabs.js"]
html_theme_options = {
    "navigation_with_keys": True,
}

linkcheck_anchors = True
linkcheck_allowed_redirects = {}
linkcheck_retries = 2
linkcheck_timeout = 15
linkcheck_workers = 5
linkcheck_rate_limit_timeout = 60
linkcheck_report_timeouts_as_broken = True

_ROOT_TOCTREE = """
```{toctree}
:hidden:
:maxdepth: 1
:caption: Start here

getting-started
```

```{toctree}
:hidden:
:maxdepth: 2
:caption: Guides

testbench-authoring
verification-components
random-stimulus
test-lifecycle
running-tests
hierarchy
examples
clocking
scheduling
code-generation
```

```{toctree}
:hidden:
:maxdepth: 1
:caption: Internals

architecture
performance
```

```{toctree}
:hidden:
:maxdepth: 1
:caption: Project

roadmap
```
"""

_EXAMPLES_TOCTREE = """
```{toctree}
:hidden:
:maxdepth: 1

examples/counter
examples/timer-only
examples/fifo-scoreboard
examples/component-fifo
examples/multiclock
examples/apb-regfile
examples/watchdog-timeout
examples/fault-injection
examples/rich-data
examples/heavy-benchmarks
examples/open-source-cores
```
"""

_RANDOMIZATION_TOCTREE = """
```{toctree}
:hidden:
:maxdepth: 1

randomization/examples
randomization/value-generation
randomization/constrained-transactions
randomization/policies-and-composition
randomization/solvers-and-diagnostics
randomization/functional-coverage
randomization/reproducibility
```
"""

_NAVIGATION_TOCTREES = {
    "index": _ROOT_TOCTREE,
    "examples": _EXAMPLES_TOCTREE,
    "random-stimulus": _RANDOMIZATION_TOCTREE,
}


def _inject_navigation(app, docname, source):
    """Keep the Sphinx toctree out of the generator-neutral Markdown source."""
    toctree = app.config.cpptb_navigation_toctrees.get(docname)
    if toctree:
        source[0] += toctree


def setup(app):
    app.add_config_value("cpptb_navigation_toctrees", _NAVIGATION_TOCTREES, "env")
    app.connect("source-read", _inject_navigation)
