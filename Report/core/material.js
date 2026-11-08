// Material Analytics report type: the columns and details of a MaterialAnalytics.json report, and the optional
// cook stats CSV that replaces the shader estimates with real cooked counts. Shown by report.js.
(() => {
    "use strict";

    const chooseCookStatsButton = document.getElementById("choose-cook-stats");
    const cookStatsInput = document.getElementById("cook-stats-file");
    const cookPlatformSelect = document.getElementById("cook-platform");
    const cookStatsSummary = document.getElementById("cook-stats-summary");
    const defaultCookStatsSummary = cookStatsSummary.textContent;

    // Usage status of one vertex factory, shown in the vertex factory table.
    const usageStatuses = {
        usedNotEnabled: { label: "Used but not enabled", className: "used-not-enabled" },
        enabledUnused: { label: "Enabled but unused", className: "enabled-unused" },
        ok: { label: "Flags match users", className: "ok" },
        noneFound: { label: "No users found", className: "enabled-unused" },
        notChecked: { label: "Not checked", className: "unknown" }
    };

    // Users-column text for each UsersUnknownReason the editor writes when a vertex factory's users are unknown.
    const usersUnknownTexts = {
        UsersNotGathered: "Not checked: turn on Vertex Factory Counts (Registry Only) to list users",
        LoadingOff: "Not checked: turn on Vertex Factory Users (Loads User Assets) to check",
        NoneFoundByLoading: "None found in Blueprints, particle systems or skeletal meshes; levels not checked",
        PlacedActors: "Not checked: used by actors placed in levels",
        Niagara: "Not checked: Niagara systems aren't loaded",
        NotChecked: "Not checked"
    };

    // Every value offered in the graph menus. "axis" adds it to the X/Y menus, "filter" to the filter menu.
    // Types: "number" or "string" (pick from values).
    // A column is hidden when no material in the file has a value for it.
    const columns = [
        { key: "PackageDiskSize", label: "Package Disk Size (bytes)", type: "number", axis: true, filter: true,
            value: material => material.PackageDiskSize },
        { key: "InstanceCount", label: "Instances", type: "number", axis: true, filter: true,
            value: material => material.InstanceCount },
        // Shader Count uses the loaded cook stats CSV when there is one, otherwise the report's estimate.
        { key: "ShaderCount", label: "Shader Count", type: "number", axis: true, filter: true,
            value: material => cookedByMaterial.get(material)?.shaders ?? material.ShaderCount },
        { key: "ShaderCountSource", label: "Shader Count Source", type: "string", filter: true,
            value: material => cookStats ? shaderCountSource(material) : undefined },        { key: "TotalUsers", label: "Total Users", type: "number", axis: true, filter: true,
            value: material => hasUsers(material) ? countUsers(material) : undefined },
        { key: "EnabledUsageFlags", label: "Enabled Usage Flags", type: "number", axis: true,
            value: material => material.UsageFlagsRead ? summarizeUsage(material).enabledCount : undefined },
        { key: "EnabledUnusedFlags", label: "Enabled but Unused Flags", type: "number", axis: true,
            value: material => material.UsageFlagsRead ? summarizeUsage(material).enabledUnusedCount : undefined },
        { key: "PackagePath", label: "Package Path", type: "string", filter: true,
            value: material => material.PackagePath }
    ];

    // The report engine's callbacks, given to init.
    let report = null;
    // How many materials the page shows, for the cook stats summary. 0 before a report is shown.
    let shownMaterialCount = 0;
    // The ShaderPlatform object of the loaded report ({ Name, Index }), or null for older reports.
    let reportShaderPlatform = null;
    // The loaded cook stats CSV: { fileName, modified, platforms: Map(platform index text -> Map(package path -> cooked shaders)) }.
    let cookStats = null;
    // True once the user picks a platform in the menu, so reloading the report keeps that choice.
    let cookPlatformPickedByUser = false;
    // Cooked { shaders, cooked } per report material for the chosen platform; cooked is false when the CSV has no row
    // for the master or its static permutation instances. Empty without cook stats.
    let cookedByMaterial = new Map();
    // How the chosen platform's CSV rows matched the report: { cookedMaterials, outsideRows, outsideShaders, hasInstanceLists }.
    let cookMatch = null;

    // Binds the cook stats controls. engine.refresh() rebuilds the page when it shows materials.
    function init(engine) {
        report = engine;
        chooseCookStatsButton.addEventListener("click", () => cookStatsInput.click());

        cookStatsInput.addEventListener("change", () => {
            if (cookStatsInput.files.length > 0) {
                loadCookStatsFile(cookStatsInput.files[0]);
            }
        });

        cookPlatformSelect.addEventListener("change", () => {
            cookPlatformPickedByUser = true;
            report.refresh();
        });
    }

    // Reads a cook's Saved/MaterialStats/Stats-*.csv and applies it to the shown report, if any.
    async function loadCookStatsFile(file) {
        report.clearError();
        try {
            if (!file.name.toLowerCase().endsWith(".csv")) {
                throw new Error("Please choose a cook stats CSV from Saved/MaterialStats.");
            }
            cookStats = {
                fileName: file.name,
                modified: new Date(file.lastModified),
                platforms: parseCookStats(await file.text())
            };
            cookPlatformPickedByUser = false;
            if (!report.refresh()) {
                updateCookPlatformSelect();
                renderCookStatsSummary();
            }
        } catch (error) {
            report.showError(error instanceof Error ? error.message : "The cook stats CSV could not be loaded.");
        } finally {
            cookStatsInput.value = "";
        }
    }

    // Parses the MaterialStats CSV the engine writes after a cook (Path, Platform, Cooked, ...).
    // Returns Map(platform index text -> Map(package path -> cooked shader count)).
    function parseCookStats(text) {
        const lines = text.split(/\r?\n/).filter(line => line.trim() !== "");
        const header = parseCsvLine(lines[0] ?? "");
        const pathIndex = header.indexOf("Path");
        const platformIndex = header.indexOf("Platform");
        const cookedIndex = header.indexOf("Cooked");
        if (pathIndex < 0 || platformIndex < 0 || cookedIndex < 0) {
            throw new Error("The CSV has no Path, Platform and Cooked columns. Is it a cook's MaterialStats file?");
        }

        const platforms = new Map();
        lines.slice(1).forEach(line => {
            const fields = parseCsvLine(line);
            const path = fields[pathIndex];
            const cooked = Number(fields[cookedIndex]);
            if (!path || !Number.isFinite(cooked)) {
                return;
            }
            const platform = fields[platformIndex];
            if (!platforms.has(platform)) {
                platforms.set(platform, new Map());
            }
            const cookedByPath = platforms.get(platform);
            cookedByPath.set(path, (cookedByPath.get(path) ?? 0) + cooked);
        });
        if (platforms.size === 0) {
            throw new Error("The cook stats CSV has no material rows.");
        }
        return platforms;
    }

    // Splits one CSV line into fields, handling quoted fields and doubled quotes inside them.
    function parseCsvLine(line) {
        const fields = [];
        let field = "";
        let inQuotes = false;
        for (let index = 0; index < line.length; index += 1) {
            const character = line[index];
            if (inQuotes) {
                if (character === "\"" && line[index + 1] === "\"") {
                    field += "\"";
                    index += 1;
                } else if (character === "\"") {
                    inQuotes = false;
                } else {
                    field += character;
                }
            } else if (character === "\"") {
                inQuotes = true;
            } else if (character === ",") {
                fields.push(field);
                field = "";
            } else {
                field += character;
            }
        }
        fields.push(field);
        return fields;
    }

    // Fills the platform menu from the cook stats. Keeps the platform already picked; otherwise chooses the report's
    // own shader platform when the CSV has it. The menu is only shown when the CSV has more than one platform.
    function updateCookPlatformSelect() {
        const previous = cookPlatformSelect.value;
        cookPlatformSelect.replaceChildren();
        if (!cookStats) {
            cookPlatformSelect.hidden = true;
            return;
        }

        const reportPlatform = reportShaderPlatform ? String(reportShaderPlatform.Index) : null;
        Array.from(cookStats.platforms.keys())
            .sort((a, b) => Number(a) - Number(b))
            .forEach(platform => cookPlatformSelect.add(new Option(cookPlatformLabel(platform), platform)));
        const platforms = Array.from(cookStats.platforms.keys());
        if (cookPlatformPickedByUser && platforms.includes(previous)) {
            cookPlatformSelect.value = previous;
        } else if (reportPlatform !== null && platforms.includes(reportPlatform)) {
            cookPlatformSelect.value = reportPlatform;
        } else if (platforms.includes(previous)) {
            cookPlatformSelect.value = previous;
        }
        cookPlatformSelect.hidden = platforms.length < 2;
    }

    function cookPlatformLabel(platform) {
        return reportShaderPlatform && String(reportShaderPlatform.Index) === platform
            ? `${reportShaderPlatform.Name} (${platform})`
            : `Shader platform ${platform}`;
    }

    // Sums the chosen platform's cooked shaders for each material over its master and static permutation instances.
    // The cook only lists materials that got shaders, so a material without rows was not cooked.
    function matchCookStats(reportMaterials) {
        cookedByMaterial = new Map();
        if (!cookStats) {
            return null;
        }

        const cookedByPath = cookStats.platforms.get(cookPlatformSelect.value) ?? new Map();
        const matchedPaths = new Set();
        let cookedMaterials = 0;
        reportMaterials.forEach(material => {
            const instancePaths = (material.StaticPermutationInstances ?? []).map(path => path.split(".")[0]);
            const paths = new Set([`${material.PackagePath}/${material.AssetName}`, ...instancePaths]);
            let shaders = 0;
            let cooked = false;
            paths.forEach(path => {
                if (cookedByPath.has(path)) {
                    shaders += cookedByPath.get(path);
                    cooked = true;
                    matchedPaths.add(path);
                }
            });
            cookedByMaterial.set(material, { shaders, cooked });
            if (cooked) {
                cookedMaterials += 1;
            }
        });

        let outsideRows = 0;
        let outsideShaders = 0;
        cookedByPath.forEach((shaders, path) => {
            if (!matchedPaths.has(path)) {
                outsideRows += 1;
                outsideShaders += shaders;
            }
        });
        return {
            cookedMaterials,
            outsideRows,
            outsideShaders,
            hasInstanceLists: reportMaterials.some(material => Array.isArray(material.StaticPermutationInstances))
        };
    }

    function renderCookStatsSummary() {
        cookStatsSummary.classList.remove("error");
        if (!cookStats) {
            cookStatsSummary.textContent = defaultCookStatsSummary;
            return;
        }

        const parts = [
            `Cook stats: ${cookStats.fileName} (${cookStats.modified.toLocaleString()})`,
            cookPlatformLabel(cookPlatformSelect.value)
        ];
        if (!cookMatch) {
            parts.push("load a report JSON to match it");
        } else {
            parts.push(`${cookMatch.cookedMaterials.toLocaleString()} of ${shownMaterialCount.toLocaleString()} materials cooked`);
            parts.push(`${cookMatch.outsideRows.toLocaleString()} cooked materials ` +
                `(${cookMatch.outsideShaders.toLocaleString()} shaders) are not in the report`);
            if (!cookMatch.hasInstanceLists) {
                parts.push("instances could not be matched; generate the report with Shader Permutations enabled");
                cookStatsSummary.classList.add("error");
            }
        }
        cookStatsSummary.textContent = parts.join(" · ");
    }

    // Only called when cook stats are loaded, so every material has a cookedByMaterial entry.
    function shaderCountSource(material) {
        return cookedByMaterial.get(material).cooked ? "Cooked" : "Not cooked";
    }

    function hasUsers(material) {
        return Object.values(material.VertexFactories ?? {}).some(entry => Array.isArray(entry.Users));
    }

    function countUsers(material) {
        return Object.values(material.VertexFactories ?? {})
            .reduce((total, entry) => total + (entry.Users?.length ?? 0), 0);
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
        if (entry.Enabled && entry.UsersUnknownReason === "NoneFoundByLoading") {
            return usageStatuses.noneFound;
        }
        return usageStatuses.notChecked;
    }

    // Counts the enabled usage flags of a material, and those enabled without any user.
    function summarizeUsage(material) {
        let enabledCount = 0;
        let enabledUnusedCount = 0;
        Object.values(material.VertexFactories ?? {}).forEach(entry => {
            if (entry.Enabled === true) {
                enabledCount += 1;
            }
            if (entryStatus(entry) === usageStatuses.enabledUnused) {
                enabledUnusedCount += 1;
            }
        });
        return { enabledCount, enabledUnusedCount };
    }

    function splitHeader(header) {
        return header
            .replace(/([a-z0-9])([A-Z])/g, "$1 $2")
            .replace(/[_-]+/g, " ")
            .replace(/\b\w/g, character => character.toUpperCase());
    }

    // Adds the material's summary, vertex factories, instances and instance grid to the details panel.
    function renderDetails(material, panel, ui) {
        const summary = document.createElement("div");
        summary.className = "details-summary";
        const usage = summarizeUsage(material);
        if (typeof material.PackageDiskSize === "number") {
            ui.addSummaryItem(summary, "Disk size", ui.formatBytes(material.PackageDiskSize));
        }
        ui.addNumberItem(summary, "Instances", material.InstanceCount);
        if (material.UsageFlagsRead === true) {
            ui.addSummaryItem(summary, "Enabled usage flags", usage.enabledCount.toLocaleString());
        }
        ui.addNumberItem(summary, "Shader count (layout estimate)", material.ShaderCount);        const cooked = cookedByMaterial.get(material);
        if (cooked) {
            ui.addSummaryItem(summary, "Cooked shaders", cooked.cooked ? cooked.shaders.toLocaleString() : "Not cooked");
        }
        panel.append(summary);

        panel.append(createVertexFactoryTable(material, ui));

        if (Array.isArray(material.Instances)) {
            const heading = document.createElement("h3");
            heading.textContent = "Instances";
            panel.append(heading, ui.createPathList(material.Instances, "instance"));
        }

        if (Array.isArray(material.InstancePermutations)) {
            panel.append(createInstancePermutationGrid(material, ui));
        }
    }

    // Creates an empty details-panel table with the given column headers, returning the table and its body.
    function createDetailsTable(headers) {
        const table = document.createElement("table");
        table.className = "details-table";
        const head = table.createTHead().insertRow();
        headers.forEach(text => {
            const cell = document.createElement("th");
            cell.textContent = text;
            head.append(cell);
        });
        return { table, body: table.createTBody() };
    }

    function createVertexFactoryTable(material, ui) {
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
        const statusOrder = [usageStatuses.usedNotEnabled, usageStatuses.enabledUnused, usageStatuses.noneFound, usageStatuses.ok];
        const rank = entry => {
            const index = statusOrder.indexOf(entryStatus(entry));
            return index === -1 ? statusOrder.length : index;
        };
        entries.sort(([, left], [, right]) => rank(left) - rank(right));

        const { table, body } = createDetailsTable(["Vertex factory", "Enabled", "Status", "Users"]);
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
                pill.textContent = statusOfEntry.label;
                statusCell.append(pill);
            } else {
                statusCell.textContent = "—";
            }

            const usersCell = row.insertCell();
            if (Array.isArray(entry.Users)) {
                usersCell.append(entry.Users.length === 0
                    ? "None"
                    : ui.createPathList(entry.Users, "user"));
            } else {
                // Reports made before UsersUnknownReason existed fall back to the plain text.
                usersCell.textContent = usersUnknownTexts[entry.UsersUnknownReason] ?? "Not checked";
                usersCell.className = "path-text";
            }
        });
        fragment.append(table);
        return fragment;
    }

    // Display names of the PermutationParameters kinds.
    const permutationParameterKinds = {
        StaticSwitch: "Static switch",
        ComponentMask: "Component mask",
        BaseProperty: "Base property"
    };

    // How the instance grid shows each cell State the editor writes. Each colour describes what the instance itself
    // does compared with its parent. Cells without a state are not overridden and have the master's value.
    const instanceCellStates = {
        SameAsParent: { className: "same-as-parent", marker: "○", title: "Overridden with the parent's value" },
        Changed: { className: "changed", marker: "●", title: "Overridden with a value different from the parent's" },
        Inherited: { className: "inherited", marker: "↑", title: "Not overridden; inherited from a parent instance, different from the master's value" }
    };

    // Explains the instance grid's colours and markers, shown above the grid.
    function createInstanceGridLegend() {
        const legend = document.createElement("div");
        legend.className = "grid-legend";
        [
            ["master", "", "Not overridden: the instance uses its parent's value and adds no cost. " +
                `${instanceCellStates.Inherited.marker} marks an inherited value that differs from the master's.`],
            ["same-as-parent", instanceCellStates.SameAsParent.marker,
                "Overridden with the parent's value. No new shader map, but the instance keeps its own copy of one, which costs some CPU memory and load time. Untick the override."],
            ["changed", instanceCellStates.Changed.marker,
                "Overridden with a value different from the parent's, including setting it back to the master's value. Needs its own shader map unless other instances share the same values."]
        ].forEach(([className, marker, text]) => {
            const item = document.createElement("div");
            item.className = "grid-legend-item";
            const swatch = document.createElement("span");
            swatch.className = `grid-swatch ${className}`;
            swatch.textContent = marker;
            const description = document.createElement("span");
            description.textContent = text;
            item.append(swatch, description);
            legend.append(item);
        });
        const note = document.createElement("p");
        note.className = "path-text";
        note.textContent = "Only parameters that some instance overrides are shown.";
        legend.append(note);
        return legend;
    }

    // Shows every counted instance, sorted by path, against the static parameters and base properties that any
    // instance overrides.
    function createInstancePermutationGrid(material, ui) {
        const fragment = document.createDocumentFragment();
        const heading = document.createElement("h3");
        heading.textContent = "Instance static parameters";
        fragment.append(heading);

        const instances = material.InstancePermutations;
        const parameters = Array.isArray(material.PermutationParameters) ? material.PermutationParameters : [];
        // Base properties come first, led by the blend mode, then the static parameters. Inside each group the most
        // changed parameters come first.
        const columnRank = parameter => parameter.Kind !== "BaseProperty" ? 2 : parameter.Name === "BlendMode" ? 0 : 1;
        const columns = parameters
            .map((parameter, index) => ({ parameter, index }))
            .filter(({ parameter }) => (parameter.ChangedCount ?? 0) > 0 || (parameter.NoOpCount ?? 0) > 0)
            .sort((left, right) =>
                columnRank(left.parameter) - columnRank(right.parameter) ||
                (right.parameter.ChangedCount ?? 0) - (left.parameter.ChangedCount ?? 0) ||
                (right.parameter.NoOpCount ?? 0) - (left.parameter.NoOpCount ?? 0) ||
                String(left.parameter.Name).localeCompare(String(right.parameter.Name)));
        if (instances.length === 0 || columns.length === 0) {
            const none = document.createElement("p");
            none.className = "path-text";
            none.textContent = instances.length === 0
                ? "No counted instances were loaded."
                : "No instance overrides a static parameter or base property, so every instance uses the master's shader map.";
            fragment.append(none);
            return fragment;
        }

        const table = document.createElement("table");
        table.className = "details-table permutation-grid";
        const headRow = table.createTHead().insertRow();
        const corner = document.createElement("th");
        corner.textContent = "Instance";
        headRow.append(corner);
        columns.forEach(({ parameter }) => {
            const cell = document.createElement("th");
            cell.textContent = parameter.Name ?? "";
            const kind = permutationParameterKinds[parameter.Kind] ?? parameter.Kind ?? "";
            const changedCount = (parameter.ChangedCount ?? 0).toLocaleString();
            const noOpCount = (parameter.NoOpCount ?? 0).toLocaleString();
            cell.title = [
                parameter.Default !== undefined ? `${kind}, master's value ${parameter.Default}` : kind,
                `Overrides that differ from the parent: ${changedCount}`,
                `Overrides with the parent's value: ${noOpCount}`
            ].join("\n");
            headRow.append(cell);
        });

        const body = table.createTBody();
        instances.forEach(instance => {
            const row = body.insertRow();
            const nameCell = document.createElement("th");
            nameCell.scope = "row";
            const path = String(instance.Path ?? "");
            const assetName = path.split("/").pop().split(".")[0];
            nameCell.append(ui.createPathLink(path, assetName || path));
            row.append(nameCell);

            const cellsByParameter = new Map((instance.Cells ?? []).map(cell => [cell.Parameter, cell]));
            columns.forEach(({ parameter, index }) => {
                const cell = row.insertCell();
                const instanceCell = cellsByParameter.get(index);
                const state = instanceCell ? instanceCellStates[instanceCell.State] : null;
                if (!state) {
                    cell.className = "grid-cell master";
                    cell.textContent = parameter.Default ?? "Master's";
                    return;
                }
                cell.className = `grid-cell ${state.className}`;
                cell.title = state.title;
                cell.textContent = `${state.marker} ${instanceCell.Value ?? ""}`;
            });
        });

        const scroller = document.createElement("div");
        scroller.className = "permutation-grid-scroll";
        scroller.append(table);

        // Collapsed by default, like the Instances list.
        const section = document.createElement("details");
        section.className = "path-list";
        const summary = document.createElement("summary");
        summary.textContent = `${instances.length.toLocaleString()} ${instances.length === 1 ? "instance" : "instances"} × ` +
            `${columns.length.toLocaleString()} ${columns.length === 1 ? "parameter" : "parameters"}`;
        section.append(summary, createInstanceGridLegend(), scroller);
        fragment.append(section);
        return fragment;
    }

    (window.AssetAnalyticsReportTypes ??= []).push({
        id: "material",
        title: "Material Analytics",
        listKey: "Materials",
        noun: "material",
        nounPlural: "materials",
        dataRoute: "material-data",
        defaultFileName: "MaterialAnalytics.json",
        columns,
        defaultAxes: ["TotalUsers", "ShaderCount"],
        init,
        onReportLoaded: loadedReport => {
            reportShaderPlatform = loadedReport.ShaderPlatform ?? null;
        },
        // Matches the cook stats against the materials before their rows are built, as the shader columns read them.
        beforeBuild: reportMaterials => {
            updateCookPlatformSelect();
            cookMatch = matchCookStats(reportMaterials);
        },
        afterShow: reportMaterials => {
            shownMaterialCount = reportMaterials.length;
            renderCookStatsSummary();
        },
        loadCsv: loadCookStatsFile,
        renderDetails
    });
})();
