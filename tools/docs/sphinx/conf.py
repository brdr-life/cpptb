"""Sphinx configuration for the cpptb documentation."""

from __future__ import annotations

import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "_ext"))

project = "cpptb"
author = "cpptb contributors"
copyright = "2026, cpptb contributors"
version = "0.1"
release = "0.1.0"

extensions = [
    "myst_parser",
    "sphinx.ext.githubpages",
    "api_links",
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
html_css_files = [
    "code-tabs.css",
    "roadmap-status.css",
    "sidebar-current.css",
    "refcard.css",
]
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
core-ideas
coming-from-cocotb
```

```{toctree}
:hidden:
:maxdepth: 2
:caption: Core concepts

testbench-authoring
test-lifecycle
clocking
scheduling
logging
waveforms
```

```{toctree}
:hidden:
:maxdepth: 1
:caption: Examples

Overview <examples>
Simple <examples/simple>
Advanced <examples/advanced>
```

```{toctree}
:hidden:
:maxdepth: 1
:caption: Connect to your DUT

hierarchy
interfaces
four-state
code-generation
```

```{toctree}
:hidden:
:maxdepth: 2
:caption: Advanced Verification

verification-components
Registers & memory <memory-register-models>
random-stimulus
```

```{toctree}
:hidden:
:maxdepth: 1
:caption: Library reference

API reference <refcard>
Triggers & phase waits <library/awaitables>
Tasks & coordination <library/coordination>
Signals & the Dut <library/signals>
TestContext & checks <library/test-context>
Randomization <library/randomization>
Functional coverage <library/coverage>
Verification components <library/components>
Register models <library/registers>
Embedding & results <library/embedding>
```

```{toctree}
:hidden:
:maxdepth: 1
:caption: Reference

running-tests
cli
cpptb-toml
troubleshooting
glossary
```

```{toctree}
:hidden:
:maxdepth: 1
:caption: Internals

how-it-works
architecture
performance
```

```{toctree}
:hidden:
:maxdepth: 1
:caption: Project

roadmap
future-directions
Ibex ports & co-simulation <open-core-ports>
```
"""

_SIMPLE_EXAMPLES_TOCTREE = """
```{toctree}
:hidden:
:maxdepth: 1

counter
timer-only
fifo-scoreboard
multiclock
watchdog-timeout
```
"""

_ADVANCED_EXAMPLES_TOCTREE = """
```{toctree}
:hidden:
:maxdepth: 1

component-fifo
apb-regfile
apb-trace
ipxact-regfile
fault-injection
rich-data
interfaces
mixed-logging
heavy-benchmarks
open-source-cores
secworks-aes-regmodel
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

_VERIFICATION_COMPONENTS_TOCTREE = """
```{toctree}
:hidden:
:maxdepth: 2

verification-components/memory-model
verification-components/transaction-recording
```
"""

_REGISTER_MODELS_TOCTREE = """
```{toctree}
:hidden:
:maxdepth: 1

Generate a model <verification-components/register-generation>
Standard sequences <verification-components/register-sequences>
```
"""

_NAVIGATION_TOCTREES = {
    "index": _ROOT_TOCTREE,
    "examples/simple": _SIMPLE_EXAMPLES_TOCTREE,
    "examples/advanced": _ADVANCED_EXAMPLES_TOCTREE,
    "random-stimulus": _RANDOMIZATION_TOCTREE,
    "verification-components": _VERIFICATION_COMPONENTS_TOCTREE,
    "memory-register-models": _REGISTER_MODELS_TOCTREE,
}


def _inject_navigation(app, docname, source):
    """Keep the Sphinx toctree out of the generator-neutral Markdown source."""
    toctree = app.config.cpptb_navigation_toctrees.get(docname)
    if toctree:
        source[0] += toctree


def setup(app):
    app.add_config_value("cpptb_navigation_toctrees", _NAVIGATION_TOCTREES, "env")
    app.connect("source-read", _inject_navigation)
