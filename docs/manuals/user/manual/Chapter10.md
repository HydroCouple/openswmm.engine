# Chapter 10 PRINTING AND COPYING

This chapter describes how to print, copy to the Windows clipboard, or copy to file the contents of the currently active window in the SWMM workspace. This can include the study area map, a graph, a table, or a report.

## 10.1 Selecting a Printer

To select a printer from among your installed Windows printers and set its properties:

1.  Select **File >> Page Setup** from the Main Menu.
2.  Click the **Printer** button on the Page Setup dialog that appears (see below).
3.  Select a printer from the choices available in the combo box in the Print Setup dialog that appears.
4.  Click the **Properties** button to select the appropriate printer properties (which vary with choice of printer).
5.  Click **OK** on each dialog to accept your selections.

![Page Setup dialog box, Margins tab.](../../Manual/images/page-setup-margins.png)

## 10.2 Setting the Page Format

To format the printed page:

1.  Select **File >> Page Setup** from the main menu.
2.  Use the **Margins** page of the Page Setup dialog form that appears (see above) to:
    *   Select a printer.
    *   Select the paper orientation (Portrait or Landscape).
    *   Set left, right, top, and bottom margins.
3.  Use the **Headers/Footers** page of the dialog box (see below) to:
    *   Supply the text for a header that will appear on each page.
    *   Indicate whether the header should be printed or not and how its text should be aligned.
    *   Supply the text for a footer that will appear on each page.
    *   Indicate whether the footer should be printed or not and how its text should be aligned.
    *   Indicate whether pages should be numbered.
4.  Click **OK** to accept your choices.

![Page Setup dialog box, Headers/Footers tab.](../../Manual/images/page-setup-headers.png)

## 10.3 Print Preview

To preview a printout select **File >> Print Preview** from the Main Menu. A Preview form will appear which shows how each page being printed will appear. While in preview mode, the left mouse button will re-center and zoom in on the image and the right mouse button will re-center and zoom out.

## 10.4 Printing the Current View

To print the contents of the current window being viewed in the SWMM workspace, either select **File >> Print** from the Main Menu or click the print icon on the Main Toolbar. The following views can be printed:

*   Study Area Map (at the current zoom level)
*   Status Report.
*   Summary report (for the current table being viewed)
*   Graphs (Time Series, Profile, and Scatter plots)
*   Tabular Reports
*   Statistical Reports.

## 10.5 Copying to the Clipboard or to a File

SWMM can copy the text and graphics of the current window being viewed to the Windows clipboard or to a file. Views that can be copied in this fashion include the Study Area Map, summary report tables, graphs, time series tables, and statistical reports. To copy the current view to the clipboard or to file:

1.  If the current view is a time series table, select the cells of the table to copy by dragging the mouse over them or copy the entire table by selecting **Edit >> Select All** from the Main Menu.
2.  Select **Edit >> Copy To** from the Main Menu or click the copy icon on the Main Toolbar.
3.  Select choices from the Copy dialog that appears (see below) and click the **OK** button.
4.  If copying to file, enter the name of the file in the **Save As** dialog that appears and click **OK**.

![Copy Chart dialog box.](../../Manual/images/copy-chart.png)

Use the Copy dialog as follows to define how you want your data copied and to where:

1.  Select a destination for the material being copied (**Clipboard** or **File**)
2.  Select a format to copy in:
    *   **Bitmap** (graphics only)
    *   **Metafile** (graphics only)
    *   **Data** (text, selected cells in a table, or data used to construct a graph)
3.  Click **OK** to accept your selections or **Cancel** to cancel the copy request.

The bitmap format copies the individual pixels of a graphic. The metafile format copies the instructions used to create the graphic and is more suitable for pasting into word processing documents where the graphic can be re-scaled without losing resolution. When data is copied, it can be pasted directly into a spreadsheet program to create customized tables or charts.