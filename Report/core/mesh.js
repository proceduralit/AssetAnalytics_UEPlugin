// Mesh Analytics report type: the columns and details of a MeshAnalytics.json report. Shown by report.js.
(() => {
    "use strict";

    // Every value offered in the graph menus. "axis" adds it to the X/Y menus, "filter" to the filter menu.
    // Types: "number", "boolean" (True/False) or "string" (pick from values).
    // A column is hidden when no mesh in the file has a value for it.
    const columns = [
        { key: "IsSkeletalMesh", label: "Is Skeletal Mesh", type: "boolean", axis: true, filter: true,
            value: mesh => mesh.IsSkeletalMesh },
        { key: "PackageDiskSize", label: "Package Disk Size (bytes)", type: "number", axis: true, filter: true,
            value: mesh => mesh.PackageDiskSize },
        { key: "LODCount", label: "LODs", type: "number", axis: true, filter: true, integerTicks: true,
            value: mesh => mesh.LODCount },
        { key: "LODGroup", label: "LOD Group", type: "string", filter: true,
            value: mesh => mesh.LODGroup },
        { key: "CollisionComplexity", label: "Collision Complexity", type: "string", filter: true,
            value: mesh => mesh.CollisionComplexity },
        { key: "SimpleCollisionPrimitives", label: "Simple Collision Primitives", type: "number", axis: true, filter: true,
            value: mesh => mesh.SimpleCollisionPrimitives },
        // The report stores bytes; megabytes are easier to read on an axis.
        { key: "PhysicsSizeMB", label: "Physics Size (MB)", type: "number", axis: true, filter: true,
            value: mesh => typeof mesh.PhysicsSize === "number" ? roundTo(mesh.PhysicsSize / (1024 * 1024), 3) : undefined },
        { key: "ComplexCollisionVertices", label: "Complex Collision Verts", type: "number", axis: true, filter: true,
            value: mesh => mesh.ComplexCollisionVertices },
        { key: "LOD0Triangles", label: "Number of Triangles", type: "number", axis: true, filter: true,
            value: mesh => mesh.LOD0Triangles },
        { key: "NaniteEnabled", label: "Nanite Enabled", type: "boolean", axis: true, filter: true,
            value: mesh => mesh.NaniteEnabled },
        { key: "NaniteTriangles", label: "Nanite Triangles", type: "number", axis: true, filter: true,
            value: mesh => mesh.NaniteTriangles },
        { key: "NaniteFallbackPercent", label: "Nanite Fallback (%)", type: "number", axis: true, filter: true,
            value: mesh => typeof mesh.NaniteFallbackPercent === "number" ? roundTo(mesh.NaniteFallbackPercent, 3) : undefined },
        { key: "MaterialSlots", label: "Material Slots", type: "number", axis: true, filter: true,
            value: mesh => mesh.MaterialSlots },
        { key: "UniqueTextures", label: "Unique Textures", type: "number", axis: true, filter: true,
            value: mesh => mesh.UniqueTextures },
        { key: "MaxTextureResolution", label: "Max Texture Resolution", type: "number", axis: true, filter: true,
            value: mesh => mesh.MaxTextureResolution },
        { key: "UVChannels", label: "UV Channels", type: "number", axis: true, filter: true,
            value: mesh => mesh.UVChannels },
        { key: "LightmapResolution", label: "Lightmap Resolution", type: "number", axis: true, filter: true,
            value: mesh => mesh.LightmapResolution },
        { key: "MaxBoundsLengthM", label: "Max Bound Length (m)", type: "number", axis: true, filter: true,
            value: mesh => typeof mesh.MaxBoundsLengthM === "number" ? roundTo(mesh.MaxBoundsLengthM, 3) : undefined },
        { key: "PackagePath", label: "Package Path", type: "string", filter: true,
            value: mesh => mesh.PackagePath }
    ];

    const skeletalMeshStyle = { label: "Skeletal Mesh", color: "#ff5ca8", borderColor: "#ffd0e5" };
    const staticMeshStyle = { label: "Static Mesh", color: "#36a3ff", borderColor: "#b9ddff" };

    function roundTo(value, decimals) {
        return Number(value.toFixed(decimals));
    }

    // Adds the mesh's summary, materials and textures to the details panel.
    function renderDetails(mesh, panel, ui) {
        const summary = document.createElement("div");
        summary.className = "details-summary";
        ui.addSummaryItem(summary, "Type", mesh.IsSkeletalMesh ? "Skeletal Mesh" : "Static Mesh");
        if (typeof mesh.PackageDiskSize === "number") {
            ui.addSummaryItem(summary, "Disk size", ui.formatBytes(mesh.PackageDiskSize));
        }
        ui.addNumberItem(summary, "LODs", mesh.LODCount);
        if (typeof mesh.LODGroup === "string" && !mesh.IsSkeletalMesh) {
            ui.addSummaryItem(summary, "LOD group", mesh.LODGroup);
        }
        ui.addNumberItem(summary, "LOD 0 triangles", mesh.LOD0Triangles);
        if (typeof mesh.NaniteEnabled === "boolean") {
            ui.addSummaryItem(summary, "Nanite", mesh.NaniteEnabled ? "Enabled" : "Disabled");
        }
        if (mesh.NaniteEnabled === true) {
            ui.addNumberItem(summary, "Nanite triangles", mesh.NaniteTriangles);
            if (typeof mesh.NaniteFallbackPercent === "number") {
                ui.addSummaryItem(summary, "Nanite fallback", `${roundTo(mesh.NaniteFallbackPercent, 2)}%`);
            }
        }
        if (typeof mesh.CollisionComplexity === "string") {
            ui.addSummaryItem(summary, "Collision", mesh.CollisionComplexity);
        }
        ui.addNumberItem(summary, "Simple collision primitives", mesh.SimpleCollisionPrimitives);
        ui.addNumberItem(summary, "Complex collision verts", mesh.ComplexCollisionVertices);
        if (typeof mesh.PhysicsSize === "number") {
            ui.addSummaryItem(summary, "Physics size", ui.formatBytes(mesh.PhysicsSize));
        }
        ui.addNumberItem(summary, "Material slots", mesh.MaterialSlots);
        ui.addNumberItem(summary, "Unique textures", mesh.UniqueTextures);
        ui.addNumberItem(summary, "Max texture resolution", mesh.MaxTextureResolution);
        ui.addNumberItem(summary, "UV channels", mesh.UVChannels);
        ui.addNumberItem(summary, "Lightmap resolution", mesh.LightmapResolution);
        if (typeof mesh.MaxBoundsLengthM === "number") {
            ui.addSummaryItem(summary, "Max bound length", `${roundTo(mesh.MaxBoundsLengthM, 2)} m`);
        }
        panel.append(summary);

        if (Array.isArray(mesh.Materials)) {
            ui.appendPathSection(panel, "Referenced materials", mesh.Materials, "material",
                "The mesh references no materials.");
        }
        if (Array.isArray(mesh.Textures)) {
            if (Array.isArray(mesh.TextureSizes) && mesh.TextureSizes.length === mesh.Textures.length &&
                mesh.Textures.length > 0) {
                panel.append(createTextureSection(mesh, ui));
            } else {
                // Reports written before texture sizes were added show the paths only.
                ui.appendPathSection(panel, "Textures used by its materials", mesh.Textures, "texture",
                    "Its materials use no textures.");
            }
        }
    }

    // Returns the heading and an open table of the textures the mesh's materials use, each with its resolution
    // and, for reports that include it, whether Virtual Texture Streaming is enabled.
    function createTextureSection(mesh, ui) {
        const fragment = document.createDocumentFragment();
        const heading = document.createElement("h3");
        heading.textContent = "Textures used by its materials";

        const list = document.createElement("details");
        list.className = "path-list";
        list.open = true;
        const summary = document.createElement("summary");
        const count = mesh.Textures.length;
        summary.textContent = `${count.toLocaleString()} ${count === 1 ? "texture" : "textures"}`;

        const table = document.createElement("table");
        table.className = "details-table";
        const hasVirtualFlags = Array.isArray(mesh.TextureVirtual) && mesh.TextureVirtual.length === count;
        const headRow = table.createTHead().insertRow();
        const headers = hasVirtualFlags ? ["Texture", "Resolution", "Virtual Texture"] : ["Texture", "Resolution"];
        headers.forEach(text => {
            const header = document.createElement("th");
            header.textContent = text;
            headRow.append(header);
        });
        const body = table.createTBody();
        mesh.Textures.forEach((path, index) => {
            const row = body.insertRow();
            row.insertCell().append(ui.createPathLink(path));
            const [width, height] = mesh.TextureSizes[index] ?? [];
            const sizeCell = row.insertCell();
            sizeCell.className = "texture-size";
            sizeCell.textContent = typeof width === "number" && typeof height === "number"
                ? `${width.toLocaleString()} × ${height.toLocaleString()}`
                : "Unknown";
            if (hasVirtualFlags) {
                const virtualCell = row.insertCell();
                virtualCell.className = "texture-virtual";
                const isVirtual = mesh.TextureVirtual[index];
                virtualCell.textContent = typeof isVirtual === "boolean" ? (isVirtual ? "Yes" : "No") : "Unknown";
            }
        });
        list.append(summary, table);
        fragment.append(heading, list);
        return fragment;
    }

    (window.AssetAnalyticsReportTypes ??= []).push({
        id: "mesh",
        title: "Mesh Analytics",
        listKey: "Meshes",
        noun: "mesh",
        nounPlural: "meshes",
        dataRoute: "mesh-data",
        defaultFileName: "MeshAnalytics.json",
        columns,
        defaultAxes: ["MaxBoundsLengthM", "LOD0Triangles"],
        defaultFilterColumn: "LODCount",
        // Colors skeletal and static meshes differently.
        pointStyle: row => row.IsSkeletalMesh === "True" ? skeletalMeshStyle : staticMeshStyle,
        renderDetails
    });
})();
