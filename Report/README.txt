ASSET ANALYTICS REPORT

One page shows both Mesh Analytics and Material Analytics reports. The page
switches to the report type of the JSON you load.

1. Open index.html in a modern browser.
2. Choose a report JSON (Saved/AssetAnalytics/MeshAnalytics.json or
   Saved/AssetAnalytics/MaterialAnalytics.json), or drag it anywhere onto the page.
3. Add as many filter conditions as needed above the graph. Numeric columns
   support less than, equal to, and greater than. Text columns provide grouped
   value selection, and True/False columns use a dropdown. An asset must match
   every active condition; multiple text values within one condition are ORed
   together.
4. Use the X-Axis and Y-Axis menus to compare different numeric parameters.
   A material whose users the report could not list is plotted as a hollow
   grey point on the dotted Unknown line next to 0 Total Users. It may still
   be used by levels, widgets or other assets the report does not check.
   Total Users filters do not match these materials; filter Users Known by
   No to show only them, or by Yes to hide them.
   Assets with no value for another axis are listed below the graph when
   nothing is selected; click one to show its details.
5. Use Box Zoom in the graph toolbar to draw one zoom rectangle. The graph then
   returns to pan mode automatically; Home restores the full view.
6. Click a point to show the asset's details below the graph: for a mesh, the
   materials it references and the textures they use; for a material, its
   vertex factories and instances, followed by a colour-coded grid of every
   instance's base property overrides and static parameters. Under each
   parameter name, the yellow number counts instances that override it with
   their parent's value and the red number counts instances that override it
   with a different value. Click an empty area of the graph to clear the
   selection.
7. Press Show in Content Browser, or click an asset path in the details, to
   select that asset in an open Unreal Editor. The editor plugin must be loaded
   for this to work.
8. For a Material Analytics report, press Load Cook Stats (or drop the CSV) to
   load a cook's Saved/MaterialStats CSV. Shader Count then shows the real
   cooked value instead of the report's estimate.

The report works locally and does not upload the JSON. Files inside the core
folder are required by the report and do not need to be edited.
