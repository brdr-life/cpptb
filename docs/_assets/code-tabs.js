(() => {
  "use strict";

  const storagePrefix = "cpptb-code-tabs:";
  const tabGroups = new Map();
  let tabSetIndex = 0;

  function storedLabel(group) {
    try {
      return window.localStorage.getItem(storagePrefix + group);
    } catch (_error) {
      return null;
    }
  }

  function storeLabel(group, label) {
    try {
      window.localStorage.setItem(storagePrefix + group, label);
    } catch (_error) {
      // Storage can be unavailable for local or privacy-restricted previews.
    }
  }

  function selectTab(tabSet, label, focus = false) {
    const selected = tabSet.entries.find((entry) => entry.label === label) ||
      tabSet.entries[0];

    for (const entry of tabSet.entries) {
      const active = entry === selected;
      entry.button.setAttribute("aria-selected", String(active));
      entry.button.tabIndex = active ? 0 : -1;
      entry.panel.hidden = !active;
    }

    if (focus) {
      selected.button.focus();
    }
  }

  function selectGroup(group, label, focusTabSet = null) {
    storeLabel(group, label);
    const connectedTabSets = (tabGroups.get(group) || []).filter(
      (tabSet) => tabSet.marker.isConnected,
    );
    tabGroups.set(group, connectedTabSets);
    for (const tabSet of connectedTabSets) {
      selectTab(tabSet, label, tabSet === focusTabSet);
    }
  }

  function handleKeydown(event, tabSet, entryIndex) {
    const keys = ["ArrowLeft", "ArrowRight", "Home", "End"];
    if (!keys.includes(event.key)) {
      return;
    }

    event.preventDefault();
    let nextIndex = entryIndex;
    if (event.key === "ArrowLeft") {
      nextIndex = (entryIndex - 1 + tabSet.entries.length) % tabSet.entries.length;
    } else if (event.key === "ArrowRight") {
      nextIndex = (entryIndex + 1) % tabSet.entries.length;
    } else if (event.key === "Home") {
      nextIndex = 0;
    } else if (event.key === "End") {
      nextIndex = tabSet.entries.length - 1;
    }

    selectGroup(tabSet.group, tabSet.entries[nextIndex].label, tabSet);
  }

  function collectSourceEntries(marker) {
    const count = Number.parseInt(marker.dataset.tabs || "0", 10);
    const entries = [];
    let labelElement = marker.nextElementSibling;

    for (let index = 0; index < count; ++index) {
      if (!labelElement?.classList.contains("cpptb-code-tab-label")) {
        return [];
      }

      const contentElement = labelElement.nextElementSibling;
      if (!contentElement || !contentElement.querySelector("pre")) {
        return [];
      }

      entries.push({
        label: labelElement.textContent.trim(),
        labelElement,
        contentElement,
      });
      labelElement = contentElement.nextElementSibling;
    }

    return entries;
  }

  function initializeTabSet(marker) {
    if (marker.dataset.cpptbTabsReady !== undefined) {
      return;
    }

    const sourceEntries = collectSourceEntries(marker);
    if (sourceEntries.length === 0) {
      return;
    }

    const id = `cpptb-code-tabs-${++tabSetIndex}`;
    const group = marker.dataset.tabGroup || id;
    const tabList = document.createElement("div");
    const tabSet = { entries: [], group, marker };

    marker.dataset.cpptbTabsReady = "";
    tabList.className = "cpptb-code-tabs__tablist";
    tabList.setAttribute("role", "tablist");
    tabList.setAttribute("aria-orientation", "horizontal");
    tabList.setAttribute("aria-label", marker.dataset.tabLabel || "Testbench implementation");
    marker.append(tabList);

    sourceEntries.forEach((source, entryIndex) => {
      const button = document.createElement("button");
      const panel = document.createElement("div");
      const tabId = `${id}-tab-${entryIndex}`;
      const panelId = `${id}-panel-${entryIndex}`;

      button.className = "cpptb-code-tabs__tab";
      button.type = "button";
      button.id = tabId;
      button.textContent = source.label;
      button.setAttribute("role", "tab");
      button.setAttribute("aria-controls", panelId);

      panel.className = "cpptb-code-tabs__panel";
      panel.id = panelId;
      panel.setAttribute("role", "tabpanel");
      panel.setAttribute("aria-labelledby", tabId);
      panel.append(source.contentElement);

      const entry = { button, panel, label: source.label };
      tabSet.entries.push(entry);
      tabList.append(button);
      marker.append(panel);
      source.labelElement.remove();

      button.addEventListener("click", () => selectGroup(group, source.label));
      button.addEventListener("keydown", (event) => handleKeydown(event, tabSet, entryIndex));
    });

    if (!tabGroups.has(group)) {
      tabGroups.set(group, []);
    }
    tabGroups.get(group).push(tabSet);

    const preferred = storedLabel(group);
    selectTab(tabSet, preferred || tabSet.entries[0].label);
  }

  function initializeCodeTabs() {
    for (const marker of document.querySelectorAll(".cpptb-code-tabs")) {
      initializeTabSet(marker);
    }
  }

  if (typeof document$ !== "undefined") {
    document$.subscribe(initializeCodeTabs);
  } else if (document.readyState === "loading") {
    document.addEventListener("DOMContentLoaded", initializeCodeTabs);
  } else {
    initializeCodeTabs();
  }
})();
