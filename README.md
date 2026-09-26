# Mesh Analytics

## Overview

Mesh Analytics is an Unreal Editor tool for inspecting Static Mesh and Skeletal Mesh assets across selected Content Browser folders. It gathers useful optimization data, saves the results as a CSV file, and opens an interactive report in your default browser.

The report can help you find assets with high triangle counts, missing or excessive LODs, expensive collision, large physics data, too many material slots, oversized textures, or unusual dimensions. You can filter the results, compare any two numeric metrics on a scatter plot, and select a plotted asset in the Unreal Editor Content Browser.

All report files and data stay on your computer. The plugin serves the report only through the local loopback address and does not upload the CSV.

## Quick Start

1. In Unreal Editor, open **Edit > Editor Preferences**.
2. Select **Plugins > Asset Analytics**.
3. Add one or more project content folders to **Search Folders**.
4. Choose whether to include Static Meshes, Skeletal Meshes, or both, and enable the columns you want to collect.
5. Open **Window > Asset Analytics > Generate Mesh Analytics Report**.
6. Wait for the progress notification to finish. You can cancel the scan from the notification if necessary.
7. The completed interactive report opens automatically in your default browser.

The generated CSV is also available at:

```text
<Project>/Saved/AssetAnalytics/MeshAnalytics.csv
```

## Configuring Mesh Analytics

Mesh Analytics settings are located under **Edit > Editor Preferences > Plugins > Asset Analytics**. The selected folders, asset types, filters, and columns are saved as editor configuration and reused the next time you generate a report.

### Asset Filters

#### Ignore Unreferenced Assets

When enabled, the report skips meshes that have no project asset referencing them. This is useful when you want the report to focus on meshes that are currently used by the project.

An asset that is loaded or placed only at runtime may not have a discoverable Asset Registry reference. Disable this option if you need a complete inventory of every mesh in the selected folders.

#### Ignore Developer References

When enabled, references originating from `/Game/Developers` do not count when deciding whether a mesh is referenced. A mesh referenced only by content in a developer folder is therefore excluded when **Ignore Unreferenced Assets** is also enabled.

This option has no effect when **Ignore Unreferenced Assets** is disabled.

#### Search Folders

Add the Content Browser folders that Mesh Analytics should scan. Each folder is searched recursively, so its subfolders are included automatically. At least one valid project content folder and one asset type must be selected for the report to contain assets.

Overlapping folders do not create duplicate report rows.

### Asset Types

- **Static Meshes** includes `UStaticMesh` assets from the selected folders.
- **Skeletal Meshes** includes `USkeletalMesh` assets from the selected folders.

You can enable either type individually or include both in the same report. The `IsSkeletalMesh` column identifies the type of each row.

### Report Columns

`AssetName`, `PackagePath`, and `IsSkeletalMesh` are always included. The remaining columns can be enabled or disabled in Editor Preferences:

| Setting | CSV column | Description |
| --- | --- | --- |
| Package Disk Size | `PackageDiskSize` | Size of the asset package on disk, in bytes. |
| LOD Count | `LODCount` | Number of LODs available for the mesh. |
| LOD Group | `LODGroup` | Static Mesh LOD group. Skeletal Mesh rows report `None`. |
| Collision Info | `CollisionComplexity`, `SimpleCollisionPrimitives` | Effective collision trace mode and the number of simple collision primitives. Primarily useful for Static Meshes. |
| Physics Size | `PhysicsSizeMB` | Estimated memory used by the mesh body setup and, for Skeletal Meshes, its Physics Asset, in MiB. Requires asset loading. |
| Complex Collision Info | `ComplexCollisionVertices` | Number of vertices used by Static Mesh complex collision data. Requires asset loading. |
| Triangle Count | `LOD0Triangles` | Triangle count for LOD 0. |
| Material Count | `MaterialSlots` | Number of material slots. Requires asset loading for Skeletal Meshes. |
| Texture Count | `UniqueTextures` | Number of unique textures used by the mesh's assigned materials. Requires asset loading. |
| Max Texture Resolution | `MaxTextureResolution` | Largest texture dimension used by the mesh's assigned materials. Imported dimensions are used for Texture 2D assets and are capped by Maximum Texture Size when configured. Requires asset loading. |
| UV Channel Count | `UVChannels` | Number of UV channels in LOD 0. Requires asset loading for Skeletal Meshes. |
| Lightmap Resolution | `LightmapResolution` | Configured Static Mesh lightmap resolution. Requires asset loading and is not applicable to Skeletal Meshes. |
| Max Bounds Length | `MaxBoundsLengthM` | Longest dimension of the mesh bounds, in metres. Requires asset loading for Skeletal Meshes. |

A blank CSV value means that the value was unavailable or the metric does not apply to that asset type.

## Generating a Report

Run **Window > Asset Analytics > Generate Mesh Analytics Report** to start gathering data. The editor displays a progress notification with the number of assets processed and the completion percentage. Use **Cancel** on that notification to stop the scan and discard the unfinished results.

Only one scan can run at a time. Choosing the command again while a report is already running does not start a second scan.

When gathering completes, the plugin:

1. Sorts the rows by asset name and then package path.
2. Creates or replaces `<Project>/Saved/AssetAnalytics/MeshAnalytics.csv`.
3. Opens the local interactive report in your default browser.
4. Displays a success notification containing the number of assets included.

If the CSV cannot be written, the editor displays a failure notification and does not open the report.

## Using the Interactive Report

### Loading Data

The report opened by Unreal Editor automatically loads the newly generated `MeshAnalytics.csv`. The file name and number of rows appear at the top of the page.

To inspect another compatible report, select **Choose CSV** or drag a `.csv` file anywhere onto the page. The CSV must contain a header row, at least one data row, unique column names, and at least two numeric or True/False columns that can be plotted.

CSV parsing and chart rendering happen locally in the browser. The report, Plotly library, and generated data are served from `127.0.0.1`; no external service receives the report data.

### Filtering Assets

The report starts with one inactive filter condition. Select a column, choose or enter a value, and use **Add condition** to combine additional rules. Remove a condition with its delete button.

- Numeric columns support **Less than**, **Equal to**, and **Greater than** comparisons. A blank value leaves the condition inactive.
- True/False columns provide an **Any**, **True**, or **False** selector.
- Text columns list the distinct values found in the CSV and the number of matching rows. You can select more than one value or clear the selection to make the condition inactive.

Every active condition must match for a row to remain visible. Multiple values selected within a single text condition are alternatives, so matching any selected value satisfies that condition.

The status below the chart shows how many points are plotted, how many rows were filtered out, and how many were skipped because one of the selected axis values was missing.

### Comparing Metrics

Use the **X-Axis** and **Y-Axis** menus to choose the two numeric or True/False columns you want to compare. Each visible point represents one mesh. Hover over a point to see its asset name, package path, asset type, and axis values.

The chart supports panning, mouse-wheel zooming, and Plotly's toolbar controls. Select **Box Zoom** and drag a rectangle to zoom into an area; the chart automatically returns to pan mode after the zoom. Use **Home** in the chart toolbar to restore the full view.

### Locating Assets in Unreal Editor

Click a graph point to select it, then choose **Show in Content Browser**. The local report bridge asks the running editor to locate and select that asset in the Content Browser.

Click an empty area of the chart to clear the current selection. This feature requires the same Unreal project to remain open with the Asset Analytics plugin loaded. It will not work if the editor is closed, the local report bridge is unavailable, or the asset no longer exists at the path recorded in the CSV.

## Performance Considerations

Mesh Analytics uses cached Asset Registry data whenever possible. Registry-only columns are generally faster because their values can be gathered without loading every mesh asset.

The following options always require asset loading and can noticeably increase report time:

- Physics Size
- Complex Collision Info
- Texture Count
- Max Texture Resolution
- Lightmap Resolution

Material Count, UV Channel Count, and Max Bounds Length also require loading Skeletal Meshes, although their Static Mesh values can normally be read from the Asset Registry.

Scanning large folders or enabling several asset-loading columns increases both processing time and temporary memory use. Assets are processed in batches of ten to keep the editor responsive. Packages loaded specifically for the report are unloaded after each batch and when the report is cancelled.

For a quick first pass, enable only registry-backed columns. Run a more detailed report with asset-loading columns after filters or folder selection have narrowed the scope.

## Troubleshooting

| Problem | Suggested checks |
| --- | --- |
| The report contains no assets | Confirm that **Search Folders** contains at least one valid project content folder and that at least one asset type is enabled. Temporarily disable **Ignore Unreferenced Assets** to determine whether the reference filter excluded all meshes. |
| An expected mesh is missing | Confirm that the mesh is inside a selected folder and that its asset type is enabled. If reference filtering is active, check whether it is unreferenced or referenced only from `/Game/Developers`. Runtime-only references may not appear in the Asset Registry. |
| The CSV cannot be saved | Confirm that the project's `Saved` directory is writable and that `MeshAnalytics.csv` is not locked by another application. Close spreadsheet software that may have the file open, then generate the report again. |
| The browser report does not open | Check the editor notification for a CSV write failure. Confirm that a default browser is configured and that local connections to `127.0.0.1:19842` are not blocked. The generated CSV should still be available in `Saved/AssetAnalytics` if gathering completed successfully. |
| The browser rejects a CSV | Use a `.csv` file with unique headers, at least one data row, and at least two numeric or True/False columns. Also check for malformed quoted fields. A CSV generated by the current plugin already meets these requirements when it contains report rows. |
| **Show in Content Browser** does not work | Keep the same Unreal project open with the plugin loaded. Confirm that the asset still exists at the package path shown by the report and that local port `19842` is available. A report opened without the editor can still display data but cannot control the Content Browser. |
| Some values are blank | The metric may not apply to that asset type, or the required Asset Registry, render, source, collision, or package data may be unavailable. For columns marked as requiring asset loading, confirm that the corresponding option was enabled before generating the CSV. |
| Report generation is slow | Reduce the number of **Search Folders**, disable unnecessary asset-loading columns, or scan Static Meshes and Skeletal Meshes separately. Texture and collision analysis are typically more expensive than registry-only metrics. |
