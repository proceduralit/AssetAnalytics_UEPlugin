(() => {
    "use strict";

    const chooseFileButton = document.getElementById("choose-file");
    const fileInput = document.getElementById("csv-file");
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
    const reportBridgeUrl = "http://127.0.0.1:19842/asset-analytics/select";

    const preferredLabels = {
        PackageDiskSize: "Package Disk Size (bytes)",
        LODCount: "LODs",
        LODGroup: "LOD Group",
        CollisionComplexity: "Collision Complexity",
        SimpleCollisionPrimitives: "Simple Collision Primitives",
        PhysicsSizeMB: "Physics Size (MB)",
        ComplexCollisionVertices: "Complex Collision Verts",
        LOD0Triangles: "Number of Triangles",
        NaniteEnabled: "Nanite Enabled",
        NaniteTriangles: "Nanite Triangles",
        NaniteFallbackPercent: "Nanite Fallback (%)",
        MaxBoundsLengthM: "Max Bound Length (m)",
        MaterialSlots: "Material Slots",
        UVChannels: "UV Channels",
        GenerateLightmapUVs: "Generate Lightmap UVs",
        LightmapResolution: "Lightmap Resolution"
    };

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
    showInContentBrowserButton.addEventListener("click", showSelectedAssetInUnreal);
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

	loadDefaultCsv();

    function hasFiles(event) {
        return Array.from(event.dataTransfer?.types ?? []).includes("Files");
    }

    async function loadFile(file) {
        clearError();

        if (!file.name.toLowerCase().endsWith(".csv")) {
            showError("Please choose a CSV file.");
            return;
        }

        try {
			loadCsvText(await file.text(), file.name);
        } catch (error) {
            showError(error instanceof Error ? error.message : "The CSV could not be loaded.");
        } finally {
            fileInput.value = "";
        }
    }

	async function loadDefaultCsv() {
		if (window.location.protocol !== "http:" && window.location.protocol !== "https:") {
			return;
		}

		try {
			const response = await fetch("data", { cache: "no-store" });
			if (!response.ok) {
				return;
			}
			loadCsvText(await response.text(), "MeshAnalytics.csv");
		} catch (error) {
			showError(error instanceof Error ? error.message : "The generated CSV could not be loaded.");
		}
	}

	function loadCsvText(text, fileName) {
		const parsed = parseCsv(text);
		const detectedColumns = detectColumns(parsed.headers, parsed.rows);
		const detectedMetrics = detectedColumns.filter(column => column.type !== "string");

		if (parsed.rows.length === 0) {
			throw new Error("The CSV contains headers but no analytics rows.");
		}
		if (detectedMetrics.length < 2) {
			throw new Error("The CSV needs at least two numeric or True/False columns.");
		}
		if (typeof Plotly === "undefined") {
			throw new Error("The local Plotly library could not be loaded.");
		}

		rows = parsed.rows;
		metrics = detectedMetrics;
		filterColumns = detectedColumns.filter(column => column.key !== "AssetName");
		populateAxisSelects();
		const hasFilterColumns = resetFilterConditions();
		fileSummary.textContent = `${fileName} · ${rows.length.toLocaleString()} rows`;
		dropZone.classList.add("has-data");
		xAxisSelect.disabled = false;
		yAxisSelect.disabled = false;
		addFilterButton.disabled = !hasFilterColumns;
		renderChart();
	}

    function parseCsv(text) {
        const table = [];
        let row = [];
        let field = "";
        let quoted = false;

        for (let index = 0; index < text.length; index += 1) {
            const character = text[index];

            if (quoted) {
                if (character === '"' && text[index + 1] === '"') {
                    field += '"';
                    index += 1;
                } else if (character === '"') {
                    quoted = false;
                } else {
                    field += character;
                }
            } else if (character === '"' && field.length === 0) {
                quoted = true;
            } else if (character === ",") {
                row.push(field);
                field = "";
            } else if (character === "\n") {
                row.push(field);
                table.push(row);
                row = [];
                field = "";
            } else if (character !== "\r") {
                field += character;
            }
        }

        if (quoted) {
            throw new Error("The CSV has an unterminated quoted field.");
        }
        if (field.length > 0 || row.length > 0) {
            row.push(field);
            table.push(row);
        }

        while (table.length > 0 && table[table.length - 1].every(value => value.trim() === "")) {
            table.pop();
        }
        if (table.length === 0) {
            throw new Error("The CSV is empty.");
        }

        const headers = table[0].map((value, index) => {
            const header = value.replace(/^\uFEFF/, "").trim();
            if (!header) {
                throw new Error(`Column ${index + 1} has no header.`);
            }
            return header;
        });

        if (new Set(headers).size !== headers.length) {
            throw new Error("The CSV contains duplicate column headers.");
        }

        const parsedRows = table.slice(1).map(values => {
            const result = {};
            headers.forEach((header, index) => {
                result[header] = values[index] ?? "";
            });
            return result;
        });

        return { headers, rows: parsedRows };
    }

    function detectColumns(headers, parsedRows) {
        return headers.flatMap(header => {
            const values = parsedRows
                .map(row => row[header].trim())
                .filter(value => value !== "");

            if (values.length === 0) {
                return [];
            }

            const isBoolean = values.every(value => /^(true|false)$/i.test(value));
            const isNumeric = values.every(value => Number.isFinite(Number(value)));
            return [{
                key: header,
                label: preferredLabels[header] ?? splitHeader(header),
                type: isBoolean ? "boolean" : isNumeric ? "number" : "string",
                integerTicks: header === "LODCount"
            }];
        });
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

        xAxisSelect.value = chooseMetric(previousX, "MaxBoundsLengthM", 0);
        yAxisSelect.value = chooseMetric(previousY, "LOD0Triangles", Math.min(1, metrics.length - 1));

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
        columnSelect.value = filterColumns.some(column => column.key === "LODCount")
            ? "LODCount"
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
                    `${value}, ${count.toLocaleString()} ${count === 1 ? "row" : "rows"}`);
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
            const isSkeletalMesh = isSkeletalMeshRow(row);
            return [{
                row,
                x,
                y,
                assetType: isSkeletalMesh ? "Skeletal Mesh" : "Static Mesh",
                pointColor: isSkeletalMesh ? "#ff5ca8" : "#36a3ff",
                borderColor: isSkeletalMesh ? "#ffd0e5" : "#b9ddff"
            }];
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
                point.row.AssetName || "Unnamed asset",
                point.row.PackagePath || "",
                point.row[xMetric.key],
                point.row[yMetric.key],
                point.assetType
            ]),
            marker: {
                color: points.map(point => point.pointColor),
                line: { color: points.map(point => point.borderColor), width: 1 },
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
                "Type: %{customdata[4]}<br>" +
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
        status.textContent = `${points.length.toLocaleString()} points plotted` +
            (filteredOut > 0 ? ` · ${filteredOut.toLocaleString()} rows filtered out` : "") +
            (skipped > 0 ? ` · ${skipped.toLocaleString()} rows skipped because an axis value is missing` : "");
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
            if (!assetName || !packagePath) {
                clearSelectedAsset();
                return;
            }

            selectedAsset = { assetName, packagePath };
            selectedAssetText.textContent = `${assetName} — ${packagePath}`;
            bridgeStatus.classList.remove("error");
            bridgeStatus.textContent = "Ready to send to Unreal Editor.";
            showInContentBrowserButton.disabled = false;
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
    }

    async function showSelectedAssetInUnreal() {
        if (!selectedAsset) {
            return;
        }

        const requestedAsset = selectedAsset;
        const requestUrl = new URL(reportBridgeUrl);
        requestUrl.searchParams.set("packagePath", requestedAsset.packagePath);
        requestUrl.searchParams.set("assetName", requestedAsset.assetName);
        requestUrl.searchParams.set("request", Date.now().toString());

        const abortController = new AbortController();
        const timeout = setTimeout(() => abortController.abort(), 2500);
        showInContentBrowserButton.disabled = true;
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
            bridgeStatus.textContent = `${requestedAsset.assetName} selected in the Content Browser.`;
        } catch (error) {
            bridgeStatus.classList.add("error");
            bridgeStatus.textContent = error instanceof Error &&
                error.name !== "AbortError" && error.name !== "TypeError"
                ? error.message
                : "Unreal Editor is not open or the report bridge is unavailable.";
        } finally {
            clearTimeout(timeout);
            if (selectedAsset === requestedAsset) {
                showInContentBrowserButton.disabled = false;
            }
        }
    }

    function metricValue(rawValue, type) {
        const value = rawValue.trim();
        if (value === "") {
            return null;
        }
        if (type === "boolean") {
            return value.toLowerCase() === "true" ? 1 : 0;
        }
        const number = Number(value);
        return Number.isFinite(number) ? number : null;
    }

    function isSkeletalMeshRow(row) {
        return (row.IsSkeletalMesh ?? "").trim().toLowerCase() === "true";
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
