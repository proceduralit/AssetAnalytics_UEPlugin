// Shared report engine for every Asset Analytics report type: file loading, filters, axes, graph and details panel.
// Each report type (mesh.js, material.js) adds itself to window.AssetAnalyticsReportTypes before this script runs.
// A report type describes its JSON and page:
//   id, title, listKey (the JSON list of assets), noun, nounPlural, dataRoute (editor route of its generated JSON),
//   defaultFileName, columns, defaultAxes ([x key, y key]), defaultFilterColumn (optional),
//   renderDetails(asset, panel, ui), and the optional hooks
//   init(context), onReportLoaded(report), beforeBuild(assets), afterShow(assets), pointStyle(row), loadCsv(file).
// A numeric column with unknownPlotValue plots assets without a value at that position, as hollow grey "Unknown"
// points, instead of listing them as not plotted. Filters still treat the value as missing.
(() => {
    "use strict";

    const reportTypes = window.AssetAnalyticsReportTypes ?? [];
    const chooseFileButton = document.getElementById("choose-file");
    const fileInput = document.getElementById("json-file");
    const reportTitle = document.getElementById("report-title");
    const addFilterButton = document.getElementById("add-filter");
    const filterList = document.getElementById("filter-list");
    const dropZone = document.getElementById("drop-zone");
    const chart = document.getElementById("chart");
    const selectedAssetLabel = document.getElementById("selected-asset-label");
    const selectedAssetText = document.getElementById("selected-asset");
    const selectedAssetPath = document.getElementById("selected-asset-path");
    const bridgeStatus = document.getElementById("bridge-status");
    const showInContentBrowserButton = document.getElementById("show-in-content-browser");
    const xAxisSelect = document.getElementById("x-axis");
    const yAxisSelect = document.getElementById("y-axis");
    const fileSummary = document.getElementById("file-summary");
    const status = document.getElementById("status");
    const detailsPanel = document.getElementById("asset-details");
    const skippedPanel = document.getElementById("skipped-assets");
    const reportBridgeUrl = "http://127.0.0.1:19842/asset-analytics/select";
    const defaultPointStyle = { color: "#36a3ff", borderColor: "#b9ddff" };
    // Hollow grey point for an asset plotted at a column's unknownPlotValue.
    const unknownPointStyle = { color: "rgba(0, 0, 0, 0)", borderColor: "#9ba4ae", borderWidth: 2 };
    const unknownText = "Unknown";

    // Details-panel helpers handed to each report type's renderDetails.
    const detailsHelpers = { addSummaryItem, addNumberItem, appendPathSection, createPathLink, createPathList, formatBytes };

    // The report type of the page URL's ?type= value, which also picks the JSON the editor serves. Null for none.
    const urlType = reportTypes.find(type => type.id === new URLSearchParams(window.location.search).get("type")) ?? null;

    // The report type whose assets are shown, or the URL's type before a report is loaded.
    let activeType = null;
    let assets = [];
    let reportFileName = "";
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
    // Rows that pass the filters but miss an X or Y value, as { row, missingLabels }, listed when nothing is selected.
    let skippedRows = [];

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

    reportTypes.forEach(type => type.init?.({
        // Rebuilds the page from the loaded assets when they belong to this type. Returns false when they do not.
        refresh: () => {
            if (activeType !== type || assets.length === 0) {
                return false;
            }
            showAssets(type, assets, reportFileName);
            return true;
        },
        clearError,
        showError
    }));
    setActiveType(urlType);
    loadDefaultReport();

    // Shows the title, labels and type-specific controls of type; null shows the page for any report.
    function setActiveType(type) {
        activeType = type;
        const title = type ? type.title : "Asset Analytics";
        document.title = `${title} Report`;
        reportTitle.textContent = title;
        selectedAssetLabel.textContent = `Selected ${type ? type.noun : "asset"}`;
        dropZone.setAttribute("aria-label", `${title} scatter plot`);
        detailsPanel.setAttribute("aria-label", `Selected ${type ? type.noun : "asset"} details`);
        skippedPanel.setAttribute("aria-label", `${type ? type.nounPlural : "Assets"} not plotted`);
        document.querySelectorAll("[data-report-type]").forEach(element => {
            element.hidden = element.dataset.reportType !== type?.id;
        });
        if (assets.length === 0) {
            fileSummary.textContent = type
                ? `Choose or drop a ${type.title} JSON file to begin.`
                : `Choose or drop a ${reportTypes.map(item => item.title).join(" or ")} JSON file to begin.`;
        }
    }

    function hasFiles(event) {
        return Array.from(event.dataTransfer?.types ?? []).includes("Files");
    }

    // Loads a report JSON, or hands a CSV to the report type that reads one (Material Analytics cook stats).
    async function loadFile(file) {
        clearError();

        const fileName = file.name.toLowerCase();
        if (fileName.endsWith(".csv")) {
            const csvType = reportTypes.find(type => type.loadCsv);
            if (!csvType) {
                showError("Please choose an Asset Analytics JSON file.");
            } else if (activeType !== csvType && assets.length > 0) {
                showError(`CSV files can only be added to a ${csvType.title} report.`);
            } else {
                if (activeType !== csvType) {
                    setActiveType(csvType);
                }
                csvType.loadCsv(file);
            }
            return;
        }
        if (!fileName.endsWith(".json")) {
            showError("Please choose an Asset Analytics JSON file.");
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

    // Loads the JSON the editor generated for the URL's report type. Does nothing when the page is opened as a file.
    async function loadDefaultReport() {
        if (!urlType || (window.location.protocol !== "http:" && window.location.protocol !== "https:")) {
            return;
        }

        try {
            const response = await fetch(urlType.dataRoute, { cache: "no-store" });
            if (!response.ok) {
                return;
            }
            loadReportText(await response.text(), urlType.defaultFileName);
        } catch (error) {
            showError(error instanceof Error ? error.message : "The generated JSON could not be loaded.");
        }
    }

    // Finds the report type from the JSON's asset list and shows its assets.
    // Throws when the text is not an Asset Analytics report or has no assets.
    function loadReportText(text, fileName) {
        let report;
        try {
            report = JSON.parse(text);
        } catch {
            throw new Error("The file is not valid JSON.");
        }
        const type = reportTypes.find(item => Array.isArray(report?.[item.listKey]));
        if (!type) {
            const listNames = reportTypes.map(item => item.listKey).join(" or ");
            throw new Error(`The JSON has no ${listNames} list. Is it an Asset Analytics report?`);
        }
        const reportAssets = report[type.listKey];
        if (reportAssets.length === 0) {
            throw new Error(`The report contains no ${type.nounPlural}.`);
        }
        if (typeof Plotly === "undefined") {
            throw new Error("The local Plotly library could not be loaded.");
        }

        type.onReportLoaded?.(report);
        showAssets(type, reportAssets, fileName);
    }

    // Builds the rows, graph menus and filters for the assets of a report type and draws the graph.
    // Throws when the assets do not have two values to plot.
    function showAssets(type, reportAssets, fileName) {
        type.beforeBuild?.(reportAssets);
        const builtRows = buildRows(type, reportAssets);
        const availableColumns = type.columns.filter(column => builtRows.some(row => row[column.key] !== ""));
        const availableMetrics = availableColumns.filter(column => column.axis);
        if (availableMetrics.length < 2) {
            throw new Error(`The report needs at least two numeric or True/False values per ${type.noun} to draw a graph.`);
        }

        assets = reportAssets;
        reportFileName = fileName;
        if (activeType !== type) {
            // A new report type starts from its own default axes rather than the previous type's.
            xAxisSelect.replaceChildren();
            yAxisSelect.replaceChildren();
            setActiveType(type);
        }
        rows = builtRows;
        metrics = availableMetrics;
        filterColumns = availableColumns.filter(column => column.filter);
        populateAxisSelects();
        const hasFilterColumns = resetFilterConditions();
        fileSummary.textContent = `${fileName} · ${rows.length.toLocaleString()} ${type.nounPlural}`;
        type.afterShow?.(reportAssets);
        dropZone.classList.add("has-data");
        xAxisSelect.disabled = false;
        yAxisSelect.disabled = false;
        addFilterButton.disabled = !hasFilterColumns;
        renderChart();
    }

    // Builds one row per asset with the values of every column as text, so the graph and filters can read them.
    // Booleans become "True" or "False"; missing values become "".
    function buildRows(type, reportAssets) {
        return reportAssets.map((asset, assetIndex) => {
            const row = {
                assetIndex,
                AssetName: String(asset.AssetName ?? "")
            };
            type.columns.forEach(column => {
                const value = column.value(asset);
                if (value === undefined || value === null) {
                    row[column.key] = "";
                } else if (typeof value === "boolean") {
                    row[column.key] = value ? "True" : "False";
                } else {
                    row[column.key] = String(value);
                }
            });
            return row;
        });
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

        const [defaultX, defaultY] = activeType.defaultAxes;
        xAxisSelect.value = chooseMetric(previousX, defaultX, 0);
        yAxisSelect.value = chooseMetric(previousY, defaultY, Math.min(1, metrics.length - 1));

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
        columnSelect.value = filterColumns.some(column => column.key === activeType.defaultFilterColumn)
            ? activeType.defaultFilterColumn
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
        if (column.type !== "string") {
            valueEditor.addEventListener("change", renderChart);
        }
        condition.valueInput.replaceWith(valueEditor);
        condition.valueInput = valueEditor;
    }

    function createStringValueMenu(condition, column) {
        const valueCounts = new Map();
        rows.forEach(row => {
            const value = row[column.key].trim();
            if (value !== "") {
                valueCounts.set(value, (valueCounts.get(value) ?? 0) + 1);
            }
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
                    `${value}, ${count.toLocaleString()} ${count === 1 ? activeType.noun : activeType.nounPlural}`);
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

        // A report type may color points by a value, such as static and skeletal meshes, and name it in the tooltip.
        const pointStyle = activeType.pointStyle ?? (() => defaultPointStyle);
        const points = [];
        skippedRows = [];
        let unknownCount = 0;
        filteredRows.forEach(row => {
            const knownX = metricValue(row[xMetric.key], xMetric.type);
            const knownY = metricValue(row[yMetric.key], yMetric.type);
            const x = knownX ?? xMetric.unknownPlotValue ?? null;
            const y = knownY ?? yMetric.unknownPlotValue ?? null;
            if (x !== null && y !== null) {
                const isUnknown = knownX === null || knownY === null;
                if (isUnknown) {
                    unknownCount += 1;
                }
                points.push({
                    row,
                    x,
                    y,
                    xText: knownX === null ? unknownText : row[xMetric.key],
                    yText: knownY === null ? unknownText : row[yMetric.key],
                    style: isUnknown ? { ...pointStyle(row), ...unknownPointStyle } : pointStyle(row)
                });
                return;
            }
            const missingLabels = [];
            if (x === null) {
                missingLabels.push(xMetric.label);
            }
            if (y === null && yMetric !== xMetric) {
                missingLabels.push(yMetric.label);
            }
            skippedRows.push({ row, missingLabels });
        });
        skippedRows.sort((left, right) => left.row.AssetName.localeCompare(right.row.AssetName));

        const skipped = skippedRows.length;
        boxZoomPending = false;
        pointClickHandled = false;
        clearSelectedAsset();
        const trace = {
            type: "scatter",
            mode: "markers",
            x: points.map(point => point.x),
            y: points.map(point => point.y),
            customdata: points.map(point => [
                point.row.AssetName || `Unnamed ${activeType.noun}`,
                point.row.PackagePath || "",
                point.xText,
                point.yText,
                point.row.assetIndex,
                point.style.label ?? ""
            ]),
            marker: {
                color: points.map(point => point.style.color),
                line: {
                    color: points.map(point => point.style.borderColor),
                    width: points.map(point => point.style.borderWidth ?? 1)
                },
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
                (activeType.pointStyle ? "Type: %{customdata[5]}<br>" : "") +
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
            uirevision: `${activeType.id}:${xMetric.key}:${yMetric.key}`,
            xaxis: axisLayout(xMetric),
            yaxis: axisLayout(yMetric),
            shapes: [],
            annotations: []
        };
        // A dotted line labelled Unknown marks where unknown values are plotted, since auto ticks may skip it.
        if (points.some(point => point.xText === unknownText)) {
            addUnknownGuide(layout, "x", xMetric.unknownPlotValue);
        }
        if (points.some(point => point.yText === unknownText)) {
            addUnknownGuide(layout, "y", yMetric.unknownPlotValue);
        }

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
        status.textContent =
            `${points.length.toLocaleString()} ${points.length === 1 ? activeType.noun : activeType.nounPlural} plotted` +
            (unknownCount > 0 ? ` (${unknownCount.toLocaleString()} at Unknown)` : "") +
            (filteredOut > 0 ? ` · ${filteredOut.toLocaleString()} filtered out` : "") +
            (skipped > 0 ? ` · ${skipped.toLocaleString()} not plotted because an axis value is missing (listed below)` : "");
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
            const asset = assets[point?.customdata?.[4]];
            if (!assetName || !packagePath || !asset) {
                clearSelectedAsset();
                return;
            }

            selectAsset(asset, false);
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

    // Makes asset the selected asset and shows its details in place of the not-plotted list.
    // fromSkippedList is true when it was picked in that list, which adds a button to return to it.
    function selectAsset(asset, fromSkippedList) {
        const assetName = asset.AssetName || `Unnamed ${activeType.noun}`;
        const packagePath = asset.PackagePath ?? "";
        selectedAsset = { assetName, packagePath };
        selectedAssetText.textContent = assetName;
        selectedAssetPath.textContent = packagePath;
        selectedAssetPath.title = packagePath;
        // The status line stays empty until Show in Content Browser is clicked.
        bridgeStatus.classList.remove("error");
        bridgeStatus.textContent = "";
        showInContentBrowserButton.disabled = packagePath === "";
        skippedPanel.hidden = true;
        renderDetails(asset, fromSkippedList);
    }

    // Clears the selection and shows the not-plotted list again in place of the details.
    function clearSelectedAsset() {
        selectedAsset = null;
        selectedAssetText.textContent = skippedRows.length > 0
            ? `Click a point in the graph or a ${activeType.noun} in the not-plotted list.`
            : "Click a point in the graph.";
        selectedAssetPath.textContent = "";
        selectedAssetPath.title = "";
        bridgeStatus.classList.remove("error");
        bridgeStatus.textContent = "";
        showInContentBrowserButton.disabled = true;
        detailsPanel.hidden = true;
        detailsPanel.replaceChildren();
        renderSkippedList();
    }

    // Fills the not-plotted panel with the filtered assets that miss an axis value, each with the values it misses.
    // Clicking an asset shows its details. The panel is hidden when every filtered asset is plotted.
    function renderSkippedList() {
        skippedPanel.replaceChildren();
        if (skippedRows.length === 0) {
            skippedPanel.hidden = true;
            return;
        }

        const count = skippedRows.length;
        const nounText = count === 1 ? activeType.noun : activeType.nounPlural;
        const title = document.createElement("h2");
        title.textContent = `${count.toLocaleString()} ${nounText} not plotted`;
        const description = document.createElement("p");
        description.className = "details-path";
        description.textContent =
            `These ${nounText} have no value for an axis, so they have no place on the graph. ` +
            `Click one to see its details.`;

        const scrollBox = document.createElement("div");
        scrollBox.className = "skipped-list";
        const table = document.createElement("table");
        table.className = "details-table";
        const headerRow = table.createTHead().insertRow();
        [activeType.noun[0].toUpperCase() + activeType.noun.slice(1), "Path", "Missing value"].forEach(text => {
            const header = document.createElement("th");
            header.textContent = text;
            headerRow.append(header);
        });
        const body = table.createTBody();
        skippedRows.forEach(({ row, missingLabels }) => {
            const asset = assets[row.assetIndex];
            const tableRow = body.insertRow();

            const nameButton = document.createElement("button");
            nameButton.type = "button";
            nameButton.className = "path-link";
            nameButton.textContent = asset.AssetName || `Unnamed ${activeType.noun}`;
            nameButton.title = "Show details";
            nameButton.addEventListener("click", () => {
                Plotly.restyle(chart, { selectedpoints: [null] });
                selectAsset(asset, true);
                detailsPanel.scrollIntoView({ block: "nearest" });
            });
            tableRow.insertCell().append(nameButton);

            const pathCell = tableRow.insertCell();
            pathCell.className = "path-text";
            pathCell.textContent = asset.PackagePath ?? "";
            tableRow.insertCell().textContent = missingLabels.join(", ");
        });
        scrollBox.append(table);
        skippedPanel.append(title, description, scrollBox);
        skippedPanel.hidden = false;
    }

    // Fills the details panel below the graph with the asset's name and path, then the report type's details.
    // withBackButton adds a button that returns to the not-plotted list.
    function renderDetails(asset, withBackButton = false) {
        detailsPanel.replaceChildren();

        if (withBackButton) {
            const backButton = document.createElement("button");
            backButton.type = "button";
            backButton.className = "path-link details-back";
            backButton.textContent = `← Back to ${activeType.nounPlural} not plotted`;
            backButton.addEventListener("click", () => {
                clearSelectedAsset();
                skippedPanel.scrollIntoView({ block: "nearest" });
            });
            detailsPanel.append(backButton);
        }

        const title = document.createElement("h2");
        title.textContent = asset.AssetName ?? `Unnamed ${activeType.noun}`;
        const path = document.createElement("p");
        path.className = "details-path";
        path.textContent = asset.PackagePath ?? "";
        detailsPanel.append(title, path);

        activeType.renderDetails(asset, detailsPanel, detailsHelpers);
        detailsPanel.hidden = false;
    }

    function addSummaryItem(container, label, value) {
        const item = document.createElement("span");
        const labelText = document.createElement("b");
        labelText.textContent = `${label}: `;
        item.append(labelText, value);
        container.append(item);
    }

    // Adds a summary item for a number, or nothing when the report has no value for it.
    function addNumberItem(container, label, value) {
        if (typeof value === "number") {
            addSummaryItem(container, label, value.toLocaleString());
        }
    }

    // Adds a heading and a path list to container, or emptyText when there are no paths.
    function appendPathSection(container, headingText, paths, noun, emptyText, open = true) {
        const heading = document.createElement("h3");
        heading.textContent = headingText;
        if (paths.length === 0) {
            const none = document.createElement("p");
            none.className = "path-text";
            none.textContent = emptyText;
            container.append(heading, none);
        } else {
            container.append(heading, createPathList(paths, noun, open));
        }
    }

    // Returns a collapsible list of asset paths, open when open is true; each path that is a plain asset can be
    // selected in Unreal.
    function createPathList(paths, noun, open = false) {
        const list = document.createElement("details");
        list.className = "path-list";
        list.open = open;
        const summary = document.createElement("summary");
        summary.textContent = `${paths.length.toLocaleString()} ${paths.length === 1 ? noun : `${noun}s`}`;
        const items = document.createElement("ul");
        paths.forEach(objectPath => {
            const item = document.createElement("li");
            item.append(createPathLink(objectPath));
            items.append(item);
        });
        list.append(summary, items);
        return list;
    }

    // Returns a button showing label that selects the asset in Unreal, or plain text for paths the Content Browser
    // cannot select. A label other than the path shows the full path in the tooltip.
    function createPathLink(objectPath, label = objectPath) {
        const asset = splitObjectPath(objectPath);
        if (!asset) {
            const text = document.createElement("span");
            text.className = "path-text";
            text.textContent = label;
            return text;
        }
        const link = document.createElement("button");
        link.type = "button";
        link.className = "path-link";
        link.textContent = label;
        link.title = label === objectPath ? "Select in the Content Browser" : `${objectPath}\nSelect in the Content Browser`;
        link.addEventListener("click", () => selectInUnreal(asset.packagePath, asset.assetName));
        return link;
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

            if (column.type === "string") {
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
        } else if (metric.integerTicks) {
            layout.dtick = 1;
        }
        if (metric.unknownPlotValue !== undefined) {
            // Plotly writes negative tick labels with a Unicode minus sign, so both spellings are renamed.
            const valueText = String(metric.unknownPlotValue);
            layout.labelalias = {
                [valueText]: unknownText,
                [valueText.replace("-", "−")]: unknownText
            };
        }
        return layout;
    }

    // Adds a dotted line across the plot at value on the "x" or "y" axis, labelled Unknown, to layout.
    function addUnknownGuide(layout, axis, value) {
        const line = { color: "#9ba4ae", width: 1, dash: "dot" };
        if (axis === "x") {
            layout.shapes.push({ type: "line", xref: "x", yref: "paper", x0: value, x1: value, y0: 0, y1: 1, line, layer: "below" });
            layout.annotations.push({ xref: "x", yref: "paper", x: value, y: 1, yanchor: "bottom", text: unknownText,
                showarrow: false, font: { color: "#9ba4ae", size: 12 } });
        } else {
            layout.shapes.push({ type: "line", xref: "paper", yref: "y", x0: 0, x1: 1, y0: value, y1: value, line, layer: "below" });
            layout.annotations.push({ xref: "paper", yref: "y", x: 1, y: value, xanchor: "right", yanchor: "bottom",
                text: unknownText, showarrow: false, font: { color: "#9ba4ae", size: 12 } });
        }
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
