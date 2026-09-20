MESH ANALYTICS REPORT

1. Open index.html in a modern browser.
2. Choose a Mesh Analytics CSV, or drag the CSV anywhere onto the page.
3. Add as many filter conditions as needed above the graph. Numeric columns
   support less than, equal to, and greater than. Text columns provide grouped
   value selection, and True/False columns use a dropdown. A row must match every
   active condition; multiple text values within one condition are ORed together.
4. Use the X-Axis and Y-Axis menus to compare different numeric parameters.
5. Use Box Zoom in the graph toolbar to draw one zoom rectangle. The graph then
   returns to pan mode automatically; Home restores the full view.
6. Click a point, then press Show in Content Browser to select that mesh asset
   in an open Unreal Editor. Click an empty area of the graph to clear the point
   selection. The editor plugin must be loaded for this to work.

The report works locally and does not upload the CSV. Files inside the core
folder are required by the report and do not need to be edited.
