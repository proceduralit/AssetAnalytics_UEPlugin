(() => {
    "use strict";

    const chooseFileButton = document.getElementById("choose-file");
    const fileInput = document.getElementById("json-file");
    const addFilterButton = document.getElementById("add-filter");
    const filterList = document.getElementById("filter-list");
    const dropZone = document.getElementById("drop-zone");
    const chart = document.getElementById("chart");
    const selectedAssetText = document.getElementById("selected-asset");
    const bridgeStatus = document.getElementById("bridge-status");
    const showInContentBrowserButton = document.getElementById("show-in-content-browser");
    const xAxisSelect = document.getElementById("x-axis");
    const yAxisSelect = document.getElementById("y-axis");
    const fileSummary = document.getElementById("file-summary");
    const status = document.getElementById("status");
    const detailsPanel = document.getElementById("material-details");
    const reportBridgeUrl = "http://127.0.0.1:19842/asset-analytics/select";

    // Usage status of a material or one of its vertex factories, shown in the details table and the Usage Status filter.
    const usageStatuses = {
        usedNotEnabled: { label: "Used but not enabled", className: "used-not-enabled" },
        enabledUnused: { label: "Enabled but unused", className: "enabled-unused" },
        ok: { label: "Flags match users", className: "ok" },
        notChecked: { label: "Not checked", className: "unknown" }
    };

    // Every value offered in the graph menus. "axis" adds it to the X/Y menus, "filter" to the filter menu.
    // Types: "number", "string" (pick from values), or "list" (a material matches if any of its values is picked).
    // A column is hidden when no material in the file has a value for it.
    const columns = [
        { key: "PackageDiskSize", label: "Package Disk Size (bytes)", type: "number", axis: true, filter: true,
            value: material => material.PackageDiskSize },
        { key: "InstanceCount", label: "Instances", type: "number", axis: true, filter: true,
            value: material => material.InstanceCount },
        { key: "ShaderMapCount", label: "Shader Maps", type: "number", axis: true, filter: true,
            value: material => material.ShaderMapCount },
        { key: "UniqueStaticPermutations", label: "Unique Static Permutations", type: "number", axis: true, filter: true,
            value: material => material.UniqueStaticPermutations },
        { key: "EstimatedVertexFactoryPermutations", label: "Estimated Vertex Factory Permutations", type: "number", axis: true, filter: true,
            value: material => material.EstimatedVertexFactoryPermutations },
        { key: "TotalUsers", label: "Total Users", type: "number", axis: true, filter: true,
            value: material => hasUsers(material) ? countUsers(material) : undefined },
        { key: "EnabledUsageFlags", label: "Enabled Usage Flags", type: "number", axis: true, filter: true,
            value: material => material.UsageFlagsRead ? summarizeUsage(material).enabledCount : undefined },
        { key: "UsedNotEnabledFlags", label: "Used but Not Enabled Flags", type: "number", axis: true, filter: true,
            value: material => material.UsageFlagsRead ? summarizeUsage(material).usedNotEnabledCount : undefined },
        { key: "EnabledUnusedFlags", label: "Enabled but Unused Flags", type: "number", axis: true, filter: true,
            value: material => material.UsageFlagsRead ? summarizeUsage(material).enabledUnusedCount : undefined },
        { key: "UsesQualitySwitch", label: "Uses Quality Switch", type: "string", filter: true,
            value: material => Array.isArray(material.QualityLevelsUsed)
                ? (material.QualityLevelsUsed.length > 0 ? "Yes" : "No")
                : undefined },
        { key: "UsageStatus", label: "Usage Status", type: "string", filter: true,
            value: material => summarizeUsage(material).status.label },
        { key: "PackagePath", label: "Package Path", type: "string", filter: true,
            value: material => material.PackagePath },
        { key: "UsedByVertexFactory", label: "Used by Vertex Factory", type: "list", filter: true,
            value: material => vertexFactoryNames(material, entry => entry.Users?.length > 0) },
        { key: "EnabledUsageFlag", label: "Usage Flag Enabled", type: "list", filter: true,
            value: material => vertexFactoryNames(material, entry => entry.Enabled === true) }
    ];

    let materials = [];
    let rows = [];
    let metrics = [];
    let filterColumns = [];
    let filterConditions = [];
    let nextFilterId = 1;
    let dragDepth = 0;
    let selectedAsset = null;
    let chartHandlersBound = false;
    let boxZoomPending = false;
    let pointClickHandled = false;

    chooseFileButton.addEventListener("click", () => fileInput.click());

    fileInput.addEventListener("change", () => {
        if (fileInput.files.length > 0) {
            loadFile(fileInput.files[0]);
        }
    });

    xAxisSelect.addEventListener("change", renderChart);
    yAxisSelect.addEventListener("change", renderChart);
    addFilterButton.addEventListener("click", () => addFilterCondition());
    showInContentBrowserButton.addEventListener("click", () => {
        if (selectedAsset) {
            selectInUnreal(selectedAsset.packagePath, selectedAsset.assetName);
        }
    });
    chart.addEventListener("contextmenu", event => event.preventDefault());

    document.addEventListener("click", event => {
        document.querySelectorAll(".filter-value-menu[open]").forEach(menu => {
            if (!menu.contains(event.target)) {
                menu.open = false;
            }
        });
    });

    document.addEventListener("dragenter", event => {
        if (hasFiles(event)) {
            event.preventDefault();
            dragDepth += 1;
            dropZone.classList.add("is-dragging");
        }
    });

    document.addEventListener("dragover", event => {
        if (hasFiles(event)) {
            event.preventDefault();
        }
    });

    document.addEventListener("dragleave", () => {
        dragDepth = Math.max(0, dragDepth - 1);
        if (dragDepth === 0) {
            dropZone.classList.remove("is-dragging");
        }
    });

    document.addEventListener("drop", event => {
        if (!hasFiles(event)) {
            return;
        }

        event.preventDefault();
        dragDepth = 0;
        dropZone.classList.remove("is-dragging");
        const file = event.dataTransfer.files[0];
        if (file) {
            loadFile(file);
        }
    });

    loadDefaultReport();

    function hasFiles(event) {
        return Array.from(event.dataTransfer?.types ?? []).includes("Files");
    }

    async function loadFile(file) {
        clearError();

        if (!file.name.toLowerCase().endsWith(".json")) {
            showError("Please choose a Material Analytics JSON file.");
            return;
        }

        try {
            loadReportText(await file.text(), file.name);
        } catch (error) {
            showError(error instanceof Error ? error.message : "The JSON could not be loaded.");
        } finally {
            fileInput.value = "";
        }
    }

    async function loadDefaultReport() {
        if (window.location.protocol !== "http:" && window.location.protocol !== "https:") {
            return;
        }

        try {
            const response = await fetch("data", { cache: "no-store" });
            if (!response.ok) {
                return;
            }
            loadReportText(await response.text(), "MaterialAnalytics.json");
        } catch (error) {
            showError(error instanceof Error ? error.message : "The generated JSON could not be loaded.");
        }
    }

    function loadReportText(text, fileName) {
        let report;
        try {
            report = JSON.parse(text);
        } catch {
            throw new Error("The file is not valid JSON.");
        }
        if (!Array.isArray(report?.Materials)) {
            throw new Error("The JSON has no Materials list. Is it a Material Analytics report?");
        }
        if (report.Materials.length === 0) {
            throw new Error("The report contains no materials.");
        }
        if (typeof Plotly === "undefined") {
            throw new Error("The local Plotly library could not be loaded.");
        }

        const builtRows = buildRows(report.Materials);
        const availableColumns = columns.filter(column => builtRows.some(row => hasValue(row[column.key])));
        const availableMetrics = availableColumns.filter(column => column.axis);
        if (availableMetrics.length < 2) {
            throw new Error("The report needs at least two numeric values per material to draw a graph.");
        }

        materials = report.Materials;
        rows = builtRows;
        metrics = availableMetrics;
        filterColumns = availableColumns.filter(column => column.filter);
        populateAxisSelects();
        const hasFilterColumns = resetFilterConditions();
        fileSummary.textContent = `${fileName} · ${rows.length.toLocaleString()} materials`;
        dropZone.classList.add("has-data");
        xAxisSelect.disabled = false;
        yAxisSelect.disabled = false;
        addFilterButton.disabled = !hasFilterColumns;
        renderChart();
    }

    // Builds one row per material with the values of every column, so the graph and filters can read them.
    // Numbers and strings are stored as text like the mesh report; list columns keep an array of names.
    function buildRows(reportMaterials) {
        return reportMaterials.map((material, materialIndex) => {
            const row = {
                materialIndex,
                AssetName: String(material.AssetName ?? "")
            };
            columns.forEach(column => {
                const value = column.value(material);
                if (column.type === "list") {
                    row[column.key] = value ?? [];
                } else {
                    row[column.key] = value === undefined || value === null ? "" : String(value);
                }
            });
            return row;
        });
    }

    function hasValue(value) {
        return Array.isArray(value) ? value.length > 0 : value !== undefined && value.trim() !== "";
    }

    function hasUsers(material) {
        return Object.values(material.VertexFactories ?? {}).some(entry => Array.isArray(entry.Users));
    }

    function countUsers(material) {
        return Object.values(material.VertexFactories ?? {})
            .reduce((total, entry) => total + (entry.Users?.length ?? 0), 0);
    }

    // Returns the readable names of the VertexFactories entries that pass isIncluded.
    function vertexFactoryNames(material, isIncluded) {
        return Object.entries(material.VertexFactories ?? {})
            .filter(([, entry]) => isIncluded(entry))
            .map(([key]) => splitHeader(key));
    }

    // Returns the status of one VertexFactories entry, or null when it has no usage flag to compare.
    function entryStatus(entry) {
        if (typeof entry.Enabled !== "boolean") {
            return null;
        }
        const userCount = entry.Users?.length;
        if (!entry.Enabled && userCount > 0) {
            return usageStatuses.usedNotEnabled;
        }
        if (entry.Enabled && userCount === 0) {
            return usageStatuses.enabledUnused;
        }
        if (entry.Enabled && userCount > 0) {
            return usageStatuses.ok;
        }
        return usageStatuses.notChecked;
    }

    // Counts enabled and mismatched usage flags of a material and picks its overall status.
    function summarizeUsage(material) {
        let enabledCount = 0;
        let usedNotEnabledCount = 0;
        let enabledUnusedCount = 0;
        Object.values(material.VertexFactories ?? {}).forEach(entry => {
            if (entry.Enabled === true) {
                enabledCount += 1;
            }
            const statusOfEntry = entryStatus(entry);
            if (statusOfEntry === usageStatuses.usedNotEnabled) {
                usedNotEnabledCount += 1;
            } else if (statusOfEntry === usageStatuses.enabledUnused) {
                enabledUnusedCount += 1;
            }
        });

        let overallStatus = usageStatuses.ok;
        if (material.UsageFlagsRead !== true) {
            overallStatus = usageStatuses.notChecked;
        } else if (usedNotEnabledCount > 0) {
            overallStatus = usageStatuses.usedNotEnabled;
        } else if (enabledUnusedCount > 0) {
            overallStatus = usageStatuses.enabledUnused;
        }
        return { status: overallStatus, enabledCount, usedNotEnabledCount, enabledUnusedCount };
    }

    function splitHeader(header) {
        return header
            .replace(/([a-z0-9])([A-Z])/g, "$1 $2")
            .replace(/[_-]+/g, " ")
            .replace(/\b\w/g, character => character.toUpperCase());
    }

    function populateAxisSelects() {
        const previousX = xAxisSelect.value;
        const previousY = yAxisSelect.value;
        xAxisSelect.replaceChildren();
        yAxisSelect.replaceChildren();

        metrics.forEach(metric => {
            xAxisSelect.add(new Option(metric.label, metric.key));
            yAxisSelect.add(new Option(metric.label, metric.key));
        });

        xAxisSelect.value = chooseMetric(previousX, "InstanceCount", 0);
        yAxisSelect.value = chooseMetric(previousY, "TotalUsers", Math.min(1, metrics.length - 1));

        if (xAxisSelect.value === yAxisSelect.value && metrics.length > 1) {
            yAxisSelect.value = metrics.find(metric => metric.key !== xAxisSelect.value).key;
        }
    }

    function resetFilterConditions() {
        filterList.replaceChildren();
        filterConditions = [];

        if (filterColumns.length === 0) {
            return false;
        }
        addFilterCondition(false);
        return true;
    }

    function addFilterCondition(shouldRender = true) {
        if (filterColumns.length === 0) {
            return;
        }

        const conditionId = nextFilterId;
        nextFilterId += 1;

        const element = document.createElement("div");
        element.className = "filter-condition";

        const columnSelect = document.createElement("select");
        columnSelect.setAttribute("aria-label", `Filter condition ${conditionId} column`);
        filterColumns.forEach(column => columnSelect.add(new Option(column.label, column.key)));
        columnSelect.value = filterColumns.some(column => column.key === "UsageStatus")
            ? "UsageStatus"
            : filterColumns[0].key;

        const operatorSelect = document.createElement("select");
        operatorSelect.setAttribute("aria-label", `Filter condition ${conditionId} comparison`);

        const valueInput = document.createElement("input");

        const removeButton = document.createElement("button");
        removeButton.type = "button";
        removeButton.textContent = "Remove";
        removeButton.setAttribute("aria-label", `Remove filter condition ${conditionId}`);

        const condition = { element, columnSelect, operatorSelect, valueInput, conditionId };
        columnSelect.addEventListener("change", () => {
            configureFilterCondition(condition);
            renderChart();
        });
        operatorSelect.addEventListener("change", renderChart);
        removeButton.addEventListener("click", () => {
            condition.element.remove();
            filterConditions = filterConditions.filter(item => item !== condition);
            renderChart();
        });

        element.append(columnSelect, operatorSelect, valueInput, removeButton);
        filterList.append(element);
        filterConditions.push(condition);
        configureFilterCondition(condition);

        if (shouldRender && rows.length > 0) {
            renderChart();
        }
    }

    function configureFilterCondition(condition) {
        const column = filterColumns.find(item => item.key === condition.columnSelect.value);
        if (!column) {
            return;
        }

        const previousOperator = condition.operatorSelect.value;
        condition.operatorSelect.replaceChildren();
        if (column.type === "number") {
            condition.operatorSelect.add(new Option("Less than (<)", "lt"));
            condition.operatorSelect.add(new Option("Equal to (=)", "eq"));
            condition.operatorSelect.add(new Option("Greater than (>)", "gt"));
            condition.operatorSelect.value = ["lt", "eq", "gt"].includes(previousOperator)
                ? previousOperator
                : "gt";
        } else if (column.type === "list") {
            condition.operatorSelect.add(new Option("Includes any of", "any"));
        } else {
            condition.operatorSelect.add(new Option("Equal to (=)", "eq"));
        }

        condition.selectedValues = new Set();
        let valueEditor;
        if (column.type === "boolean") {
            valueEditor = document.createElement("select");
            valueEditor.add(new Option("Any (inactive)", ""));
            valueEditor.add(new Option("True", "true"));
            valueEditor.add(new Option("False", "false"));
        } else if (column.type === "number") {
            valueEditor = document.createElement("input");
            valueEditor.type = "number";
            valueEditor.step = "any";
            valueEditor.placeholder = "Value (blank = inactive)";
        } else {
            valueEditor = createStringValueMenu(condition, column);
        }
        valueEditor.setAttribute("aria-label", `Filter condition ${condition.conditionId} value`);
        if (column.type !== "string" && column.type !== "list") {
            valueEditor.addEventListener("change", renderChart);
        }
        condition.valueInput.replaceWith(valueEditor);
        condition.valueInput = valueEditor;
    }

    function createStringValueMenu(condition, column) {
        const valueCounts = new Map();
        rows.forEach(row => {
            const rowValues = column.type === "list" ? row[column.key] : [row[column.key].trim()];
            rowValues.forEach(value => {
                if (value !== "") {
                    valueCounts.set(value, (valueCounts.get(value) ?? 0) + 1);
                }
            });
        });

        const menu = document.createElement("details");
        menu.className = "filter-value-menu";
        const summary = document.createElement("summary");
        summary.textContent = "Any (inactive)";

        const content = document.createElement("div");
        content.className = "filter-value-content";
        const actions = document.createElement("div");
        actions.className = "filter-value-actions";
        const clearButton = document.createElement("button");
        clearButton.type = "button";
        clearButton.textContent = "Clear selection";
        clearButton.disabled = true;
        actions.append(clearButton);

        const list = document.createElement("div");
        list.className = "filter-value-list";
        list.setAttribute("role", "group");
        list.setAttribute("aria-label", `Available ${column.label} values`);
        [...valueCounts.keys()]
            .sort((left, right) => left.localeCompare(right))
            .forEach((value, index) => {
                const count = valueCounts.get(value);
                const label = document.createElement("label");
                label.className = "filter-value-option";

                const checkbox = document.createElement("input");
                checkbox.type = "checkbox";
                checkbox.value = value;
                checkbox.id = `filter-${condition.conditionId}-value-${index + 1}`;
                checkbox.setAttribute(
                    "aria-label",
                    `${value}, ${count.toLocaleString()} ${count === 1 ? "material" : "materials"}`);
                checkbox.addEventListener("change", () => {
                    if (checkbox.checked) {
                        condition.selectedValues.add(value);
                    } else {
                        condition.selectedValues.delete(value);
                    }
                    updateStringValueMenu(summary, clearButton, condition.selectedValues);
                    renderChart();
                });

                const countText = document.createElement("span");
                countText.className = "filter-value-count";
                countText.textContent = count.toLocaleString();
                countText.setAttribute("aria-hidden", "true");
                const valueText = document.createElement("span");
                valueText.className = "filter-value-text";
                valueText.textContent = value;
                valueText.setAttribute("aria-hidden", "true");
                label.append(checkbox, countText, valueText);
                list.append(label);
            });

        clearButton.addEventListener("click", () => {
            condition.selectedValues.clear();
            list.querySelectorAll("input").forEach(checkbox => {
                checkbox.checked = false;
            });
            updateStringValueMenu(summary, clearButton, condition.selectedValues);
            renderChart();
        });
        content.append(actions, list);
        menu.append(summary, content);
        return menu;
    }

    function updateStringValueMenu(summary, clearButton, selectedValues) {
        if (selectedValues.size === 0) {
            summary.textContent = "Any (inactive)";
        } else if (selectedValues.size === 1) {
            summary.textContent = [...selectedValues][0];
        } else {
            summary.textContent = `${selectedValues.size} values selected`;
        }
        clearButton.disabled = selectedValues.size === 0;
    }

    function chooseMetric(previous, preferred, fallbackIndex) {
        if (metrics.some(metric => metric.key === previous)) {
            return previous;
        }
        if (metrics.some(metric => metric.key === preferred)) {
            return preferred;
        }
        return metrics[fallbackIndex].key;
    }

    function renderChart() {
        const xMetric = metrics.find(metric => metric.key === xAxisSelect.value);
        const yMetric = metrics.find(metric => metric.key === yAxisSelect.value);
        if (!xMetric || !yMetric) {
            return;
        }

        const rowFilters = getRowFilters();
        if (rowFilters === null) {
            return;
        }
        const filteredRows = rows.filter(row =>
            rowFilters.every(filter => matchesFilter(row, filter)));
        const filteredOut = rows.length - filteredRows.length;

        const points = filteredRows.flatMap(row => {
            const x = metricValue(row[xMetric.key], xMetric.type);
            const y = metricValue(row[yMetric.key], yMetric.type);
            if (x === null || y === null) {
                return [];
            }
            const materialStatus = summarizeUsage(materials[row.materialIndex]).status;
            return [{ row, x, y, materialStatus }];
        });

        const skipped = filteredRows.length - points.length;
        boxZoomPending = false;
        pointClickHandled = false;
        clearSelectedAsset();
        const trace = {
            type: "scatter",
            mode: "markers",
            x: points.map(point => point.x),
            y: points.map(point => point.y),
            customdata: points.map(point => [
                point.row.AssetName || "Unnamed material",
                point.row.PackagePath || "",
                point.row[xMetric.key],
                point.row[yMetric.key],
                point.materialStatus.label,
                point.row.materialIndex
            ]),
            marker: {
                color: "#36a3ff",
                line: { color: "#b9ddff", width: 1 },
                opacity: 0.76,
                size: 10
            },
            selected: {
                marker: { color: "#ffb347", opacity: 1, size: 14 }
            },
            unselected: {
                marker: { opacity: 0.28 }
            },
            hovertemplate:
                "<b>%{customdata[0]}</b><br>" +
                "%{customdata[1]}<br>" +
                "Usage: %{customdata[4]}<br>" +
                `${xMetric.label}: %{customdata[2]}<br>` +
                `${yMetric.label}: %{customdata[3]}` +
                "<extra></extra>"
        };

        const layout = {
            autosize: true,
            margin: { l: 84, r: 34, t: 36, b: 70 },
            paper_bgcolor: "#171b20",
            plot_bgcolor: "#20262d",
            font: { color: "#dce2e8", family: "Inter, Segoe UI, sans-serif" },
            hoverlabel: { bgcolor: "#0d0f12", bordercolor: "#536170", font: { color: "#f3f5f7" } },
            showlegend: false,
            dragmode: "pan",
            uirevision: `${xMetric.key}:${yMetric.key}`,
            xaxis: axisLayout(xMetric),
            yaxis: axisLayout(yMetric)
        };

        Plotly.react(chart, [trace], layout, {
            responsive: true,
            displaylogo: false,
            scrollZoom: true,
            modeBarButtonsToRemove: [
                "toImage",
                "pan2d",
                "select2d",
                "lasso2d",
                "zoomIn2d",
                "zoomOut2d",
                "autoScale2d",
                "hoverClosestCartesian",
                "hoverCompareCartesian",
                "hoverClosestGl2d",
                "toggleHover",
                "toggleSpikelines"
            ]
        }).then(bindChartHandlers);

        status.classList.remove("error");
        status.textContent = `${points.length.toLocaleString()} ${points.length === 1 ? "material" : "materials"} plotted` +
            (filteredOut > 0 ? ` · ${filteredOut.toLocaleString()} filtered out` : "") +
            (skipped > 0 ? ` · ${skipped.toLocaleString()} skipped because an axis value is missing` : "");
    }

    function bindChartHandlers() {
        if (chartHandlersBound) {
            return;
        }

        chart.on("plotly_click", event => {
            pointClickHandled = true;
            const point = event.points?.[0];
            const assetName = point?.customdata?.[0];
            const packagePath = point?.customdata?.[1];
            const material = materials[point?.customdata?.[5]];
            if (!assetName || !packagePath || !material) {
                clearSelectedAsset();
                return;
            }

            selectedAsset = { assetName, packagePath };
            selectedAssetText.textContent = `${assetName} — ${packagePath}`;
            bridgeStatus.classList.remove("error");
            bridgeStatus.textContent = "Ready to send to Unreal Editor.";
            showInContentBrowserButton.disabled = false;
            renderDetails(material);
            Plotly.restyle(chart, { selectedpoints: [[point.pointNumber]] }, [point.curveNumber]);
        });
        chart.addEventListener("click", event => {
            if (event.target.closest?.(".modebar")) {
                return;
            }

            setTimeout(() => {
                if (pointClickHandled) {
                    pointClickHandled = false;
                    return;
                }
                clearSelectedAsset();
                Plotly.restyle(chart, { selectedpoints: [null] });
            }, 0);
        });
        chart.on("plotly_relayout", event => {
            if (event.dragmode === "zoom") {
                boxZoomPending = true;
            }

            const axisRangeChanged = Object.keys(event).some(key =>
                /^(xaxis|yaxis)(\d+)?\.(range(\[\d+\])?|autorange)$/.test(key));
            if (boxZoomPending && axisRangeChanged) {
                boxZoomPending = false;
                Plotly.relayout(chart, { dragmode: "pan" });
            }
        });
        chartHandlersBound = true;
    }

    function clearSelectedAsset() {
        selectedAsset = null;
        selectedAssetText.textContent = "Click a point in the graph.";
        bridgeStatus.classList.remove("error");
        bridgeStatus.textContent = "";
        showInContentBrowserButton.disabled = true;
        detailsPanel.hidden = true;
        detailsPanel.replaceChildren();
    }

    // Fills the details panel below the graph with everything the report knows about one material.
    function renderDetails(material) {
        detailsPanel.replaceChildren();

        const title = document.createElement("h2");
        title.textContent = material.AssetName ?? "Unnamed material";
        const path = document.createElement("p");
        path.className = "details-path";
        path.textContent = material.PackagePath ?? "";

        const summary = document.createElement("div");
        summary.className = "details-summary";
        const usage = summarizeUsage(material);
        addSummaryItem(summary, "Usage", usage.status.label);
        if (typeof material.PackageDiskSize === "number") {
            addSummaryItem(summary, "Disk size", formatBytes(material.PackageDiskSize));
        }
        if (typeof material.InstanceCount === "number") {
            addSummaryItem(summary, "Instances", material.InstanceCount.toLocaleString());
        }
        if (material.UsageFlagsRead === true) {
            addSummaryItem(summary, "Enabled usage flags", usage.enabledCount.toLocaleString());
        }
        if (typeof material.ShaderMapCount === "number") {
            addSummaryItem(summary, "Shader maps", material.ShaderMapCount.toLocaleString());
        }
        if (typeof material.UniqueStaticPermutations === "number") {
            addSummaryItem(summary, "Unique static permutations", material.UniqueStaticPermutations.toLocaleString());
        }
        const reasons = material.StaticPermutationReasons;
        if (reasons) {
            const reasonText = [
                ["Static switches", reasons.StaticSwitches],
                ["Material layers", reasons.MaterialLayers],
                ["Base properties", reasons.BaseProperties],
                ["Other", reasons.Other]
            ].filter(([, count]) => count > 0)
                .map(([label, count]) => `${label} ${count.toLocaleString()}`)
                .join(" · ");
            addSummaryItem(summary, "Own shader map because of", reasonText || "None");
        }
        if (Array.isArray(material.QualityLevelsUsed)) {
            addSummaryItem(summary, "Quality levels used", material.QualityLevelsUsed.length > 0
                ? material.QualityLevelsUsed.join(", ")
                : "None (no Quality Switch)");
        }
        if (typeof material.EstimatedVertexFactoryPermutations === "number") {
            addSummaryItem(summary, "Estimated vertex factory permutations",
                material.EstimatedVertexFactoryPermutations.toLocaleString());
        }
        detailsPanel.append(title, path, summary);

        detailsPanel.append(createVertexFactoryTable(material));

        if (Array.isArray(material.StaticSwitches)) {
            detailsPanel.append(createStaticSwitchTable(material.StaticSwitches));
        }

        if (Array.isArray(material.StaticPermutationInstances)) {
            const heading = document.createElement("h3");
            heading.textContent = "Instances with their own shader map";
            if (material.StaticPermutationInstances.length === 0) {
                const none = document.createElement("p");
                none.className = "path-text";
                none.textContent = "None. Every counted instance shares the master's shader map.";
                detailsPanel.append(heading, none);
            } else {
                detailsPanel.append(heading, createPathList(material.StaticPermutationInstances, "instance"));
            }
        }

        if (Array.isArray(material.Instances)) {
            const heading = document.createElement("h3");
            heading.textContent = "Instances";
            detailsPanel.append(heading, createPathList(material.Instances, "instance"));
        }
        detailsPanel.hidden = false;
    }

    function addSummaryItem(container, label, value) {
        const item = document.createElement("span");
        const labelText = document.createElement("b");
        labelText.textContent = `${label}: `;
        item.append(labelText, value);
        container.append(item);
    }

    function createVertexFactoryTable(material) {
        const fragment = document.createDocumentFragment();
        const heading = document.createElement("h3");
        heading.textContent = "Vertex factories";
        fragment.append(heading);

        const entries = Object.entries(material.VertexFactories ?? {});
        if (entries.length === 0) {
            const empty = document.createElement("p");
            empty.className = "path-text";
            empty.textContent = "No users found and no usage flags enabled.";
            fragment.append(empty);
            return fragment;
        }

        // Problems first, keeping the engine order inside each group.
        const statusOrder = [usageStatuses.usedNotEnabled, usageStatuses.enabledUnused, usageStatuses.ok];
        const rank = entry => {
            const index = statusOrder.indexOf(entryStatus(entry));
            return index === -1 ? statusOrder.length : index;
        };
        entries.sort(([, left], [, right]) => rank(left) - rank(right));

        const table = document.createElement("table");
        table.className = "vertex-factory-table";
        const head = table.createTHead().insertRow();
        ["Vertex factory", "Enabled", "Status", "Users"].forEach(text => {
            const cell = document.createElement("th");
            cell.textContent = text;
            head.append(cell);
        });

        const body = table.createTBody();
        entries.forEach(([key, entry]) => {
            const row = body.insertRow();
            row.insertCell().textContent = splitHeader(key);
            row.insertCell().textContent = typeof entry.Enabled === "boolean"
                ? (entry.Enabled ? "Yes" : "No")
                : "—";

            const statusCell = row.insertCell();
            const statusOfEntry = entryStatus(entry);
            if (statusOfEntry) {
                const pill = document.createElement("span");
                pill.className = `status-pill ${statusOfEntry.className}`;
                pill.textContent = statusOfEntry === usageStatuses.notChecked
                    ? "Users unknown"
                    : statusOfEntry.label;
                statusCell.append(pill);
            } else {
                statusCell.textContent = "—";
            }

            const usersCell = row.insertCell();
            if (Array.isArray(entry.Users)) {
                usersCell.append(entry.Users.length === 0
                    ? "None"
                    : createPathList(entry.Users, "user"));
            } else {
                usersCell.textContent = "Unknown without loading users";
                usersCell.className = "path-text";
            }
        });
        fragment.append(table);
        return fragment;
    }

    // Lists the master's static switches with how many counted instances set them to the non-default value.
    function createStaticSwitchTable(staticSwitches) {
        const fragment = document.createDocumentFragment();
        const heading = document.createElement("h3");
        heading.textContent = "Static switches";
        fragment.append(heading);

        if (staticSwitches.length === 0) {
            const none = document.createElement("p");
            none.className = "path-text";
            none.textContent = "The material has no static switches.";
            fragment.append(none);
            return fragment;
        }

        const sortedSwitches = [...staticSwitches].sort((left, right) =>
            (right.ChangedCount ?? 0) - (left.ChangedCount ?? 0) ||
            String(left.Name).localeCompare(String(right.Name)));
        const table = document.createElement("table");
        table.className = "vertex-factory-table";
        const head = table.createTHead().insertRow();
        ["Static switch", "Default", "Instances changing it"].forEach(text => {
            const cell = document.createElement("th");
            cell.textContent = text;
            head.append(cell);
        });
        const body = table.createTBody();
        sortedSwitches.forEach(staticSwitch => {
            const row = body.insertRow();
            row.insertCell().textContent = staticSwitch.Name ?? "";
            row.insertCell().textContent = staticSwitch.Default ? "True" : "False";
            row.insertCell().textContent = (staticSwitch.ChangedCount ?? 0).toLocaleString();
        });
        fragment.append(table);
        return fragment;
    }

    // Returns a collapsible list of asset paths; each path that is a plain asset can be selected in Unreal.
    function createPathList(paths, noun) {
        const list = document.createElement("details");
        list.className = "path-list";
        const summary = document.createElement("summary");
        summary.textContent = `${paths.length.toLocaleString()} ${paths.length === 1 ? noun : `${noun}s`}`;
        const items = document.createElement("ul");
        paths.forEach(objectPath => {
            const item = document.createElement("li");
            const asset = splitObjectPath(objectPath);
            if (asset) {
                const link = document.createElement("button");
                link.type = "button";
                link.className = "path-link";
                link.textContent = objectPath;
                link.title = "Select in the Content Browser";
                link.addEventListener("click", () => selectInUnreal(asset.packagePath, asset.assetName));
                item.append(link);
            } else {
                const text = document.createElement("span");
                text.className = "path-text";
                text.textContent = objectPath;
                item.append(text);
            }
            items.append(item);
        });
        list.append(summary, items);
        return list;
    }

    // Splits "/Game/Folder/Asset.Asset" into its package path and asset name. Returns null for
    // sub-object paths such as level actors, which the Content Browser cannot select.
    function splitObjectPath(objectPath) {
        if (typeof objectPath !== "string" || objectPath.includes(":")) {
            return null;
        }
        const lastSlash = objectPath.lastIndexOf("/");
        if (lastSlash <= 0) {
            return null;
        }
        const assetName = objectPath.substring(lastSlash + 1).split(".")[0];
        return assetName ? { packagePath: objectPath.substring(0, lastSlash), assetName } : null;
    }

    function formatBytes(bytes) {
        if (bytes < 1024) {
            return `${bytes} B`;
        }
        if (bytes < 1024 * 1024) {
            return `${(bytes / 1024).toFixed(1)} KB`;
        }
        return `${(bytes / (1024 * 1024)).toFixed(2)} MB`;
    }

    async function selectInUnreal(packagePath, assetName) {
        const requestUrl = new URL(reportBridgeUrl);
        requestUrl.searchParams.set("packagePath", packagePath);
        requestUrl.searchParams.set("assetName", assetName);
        requestUrl.searchParams.set("request", Date.now().toString());

        const abortController = new AbortController();
        const timeout = setTimeout(() => abortController.abort(), 2500);
        bridgeStatus.classList.remove("error");
        bridgeStatus.textContent = "Connecting to Unreal Editor…";

        try {
            const response = await fetch(requestUrl, {
                cache: "no-store",
                signal: abortController.signal
            });
            const message = await response.text();
            if (!response.ok) {
                throw new Error(message || `Unreal Editor returned ${response.status}.`);
            }
            bridgeStatus.textContent = `${assetName} selected in the Content Browser.`;
        } catch (error) {
            bridgeStatus.classList.add("error");
            bridgeStatus.textContent = error instanceof Error &&
                error.name !== "AbortError" && error.name !== "TypeError"
                ? error.message
                : "Unreal Editor is not open or the report bridge is unavailable.";
        } finally {
            clearTimeout(timeout);
        }
    }

    function metricValue(rawValue, type) {
        const value = (rawValue ?? "").trim();
        if (value === "") {
            return null;
        }
        if (type === "boolean") {
            return value.toLowerCase() === "true" ? 1 : 0;
        }
        const number = Number(value);
        return Number.isFinite(number) ? number : null;
    }

    function getRowFilters() {
        const activeFilters = [];

        for (const condition of filterConditions) {
            const column = filterColumns.find(item => item.key === condition.columnSelect.value);
            if (!column) {
                continue;
            }

            if (column.type === "string" || column.type === "list") {
                if (condition.selectedValues.size > 0) {
                    activeFilters.push({
                        column: condition.columnSelect.value,
                        type: column.type,
                        operator: condition.operatorSelect.value,
                        value: new Set(condition.selectedValues)
                    });
                }
                continue;
            }

            condition.valueInput.setCustomValidity("");
            const rawValue = condition.valueInput.value.trim();
            if (rawValue === "") {
                continue;
            }

            const value = column.type === "number" ? Number(rawValue) : rawValue;
            if (column.type === "number" && !Number.isFinite(value)) {
                const message = "Filter values must be valid numbers.";
                condition.valueInput.setCustomValidity(message);
                showError(message);
                return null;
            }

            activeFilters.push({
                column: condition.columnSelect.value,
                type: column.type,
                operator: condition.operatorSelect.value,
                value
            });
        }
        return activeFilters;
    }

    function matchesFilter(row, filter) {
        if (filter.type === "list") {
            return row[filter.column].some(value => filter.value.has(value));
        }
        const rawRowValue = row[filter.column];
        if (rawRowValue === undefined || rawRowValue.trim() === "") {
            return false;
        }
        if (filter.type === "string") {
            return filter.value.has(rawRowValue.trim());
        }
        const rowValue = filter.type === "number"
            ? metricValue(rawRowValue, "number")
            : rawRowValue.trim().toLowerCase();
        const filterValue = filter.value;
        if (filter.operator === "lt") {
            return rowValue < filterValue;
        }
        if (filter.operator === "eq") {
            return rowValue === filterValue;
        }
        return rowValue > filterValue;
    }

    function axisLayout(metric) {
        const layout = {
            title: { text: metric.label, standoff: 16 },
            automargin: true,
            gridcolor: "#39424c",
            linecolor: "#65717d",
            zerolinecolor: "#4b5661",
            tickcolor: "#65717d"
        };

        if (metric.type === "boolean") {
            layout.tickmode = "array";
            layout.tickvals = [0, 1];
            layout.ticktext = ["False", "True"];
            layout.range = [-0.25, 1.25];
        }
        return layout;
    }

    function clearError() {
        status.classList.remove("error");
        status.textContent = "";
    }

    function showError(message) {
        status.classList.add("error");
        status.textContent = message;
    }
})();
