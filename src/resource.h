#pragma once

// Command IDs
#define IDM_FILE_OPEN        1001
#define IDM_FILE_PRINT       1002  // Phase 2
#define IDM_FILE_EXIT        1003

#define IDM_VIEW_ZOOMIN      1010
#define IDM_VIEW_ZOOMOUT     1011
#define IDM_VIEW_FITWIDTH    1012
#define IDM_VIEW_FITPAGE     1013
#define IDM_VIEW_ACTUALSIZE  1014
#define IDM_VIEW_CONTINUOUS  1015
#define IDM_VIEW_SINGLEPAGE  1016
#define IDM_VIEW_THUMBS      1017
#define IDM_VIEW_ZOOMLABEL   1018  // toolbar-only: shows current zoom %, click resets to 100%

#define IDM_GO_PREV          1020
#define IDM_GO_NEXT          1021

#define IDM_TAB_NEXT         1022  // Ctrl+Tab
#define IDM_TAB_PREV         1023  // Ctrl+Shift+Tab
#define IDM_TAB_CLOSE        1024  // Ctrl+W

#define IDM_EDIT_FIND        1040
#define IDM_EDIT_FINDNEXT    1041
#define IDM_EDIT_FINDPREV    1042
#define IDM_EDIT_COPY        1043
#define IDM_EDIT_SELECTALL   1044

#define IDM_HELP_ABOUT       1030
#define IDM_HELP_CHECKUPDATE 1031  // Tools menu: manual "Check for Updates"

// Annotation tools & file-save (Phase 3)
#define IDM_TOOL_SELECT      1200
#define IDM_TOOL_HIGHLIGHT   1201
#define IDM_TOOL_DRAW        1202
#define IDM_TOOL_TEXT        1203
#define IDM_TOOL_COLOR       1204
#define IDM_TOOL_WIDTH       1205
#define IDM_TOOL_OPACITY     1206
#define IDM_FILE_SAVE        1207
#define IDM_FILE_SAVEAS      1208

// Popup-menu item ID bases (ranges reserved)
#define IDM_WIDTH_BASE       1300  // + index
#define IDM_OPACITY_BASE     1320  // + index
#define IDM_FORMOPT_BASE     1400  // + option index for combo/list widgets

// Search-bar child control IDs
#define IDC_SEARCH_EDIT      1100
#define IDC_SEARCH_PREV      1101
#define IDC_SEARCH_NEXT      1102
#define IDC_SEARCH_CLOSE     1103
#define IDC_SEARCH_LABEL     1104
#define IDC_PAGE_EDIT        1105

// Password prompt bar (shown when an opened PDF needs a password)
#define IDC_PWD_EDIT         1106
#define IDC_PWD_UNLOCK       1107
#define IDC_PWD_CANCEL       1108
#define IDC_PWD_LABEL        1109

// Protection info bar (shown when the open PDF is encrypted/restricted)
#define IDC_PROT_LABEL              1110
#define IDC_PROT_REMOVE_PWD         1111
#define IDC_PROT_REMOVE_RESTRICTIONS 1112
#define IDC_PROT_CLOSE              1113

// Shared operation-result bar (shown after Organize/Merge/Resize/Flatten/
// Compress/Apply Redactions) -- offers Save vs. Save a Copy.
#define IDC_OPRESULT_LABEL   1114
#define IDC_OPRESULT_SAVE    1115
#define IDC_OPRESULT_SAVEAS  1116
#define IDC_OPRESULT_CLOSE   1117

// Split bar (page-range text box + Split button)
#define IDC_SPLIT_LABEL   1118
#define IDC_SPLIT_EDIT    1119
#define IDC_SPLIT_BUTTON  1120
#define IDC_SPLIT_CLOSE   1121
#define IDC_SPLIT_RESULT  1122

// Redact bar (shown while the Redact tool is active)
#define IDC_REDACT_LABEL   1123
#define IDC_REDACT_APPLY   1124
#define IDC_REDACT_CLEAR   1125
#define IDC_REDACT_DONE    1126

// Organize side-panel action strip (bottom of the thumbnail column)
#define IDC_ORGANIZE_INSERT  1127
#define IDC_ORGANIZE_DONE    1128
#define IDC_ORGANIZE_CANCEL  1129

// Redact tool + Tools popup menu items
#define IDM_TOOL_REDACT        1209
#define IDM_TOOLS_MENU         1210  // toolbar button that opens the popup below
#define IDM_TOOLS_ORGANIZE     1211
#define IDM_TOOLS_MERGE        1212
#define IDM_TOOLS_SPLIT        1213
#define IDM_TOOLS_RESIZE_A4    1214
#define IDM_TOOLS_FLATTEN      1215
#define IDM_TOOLS_COMPRESS     1216
#define IDM_VIEW_TOGGLETHEME   1217  // toolbar-only: flips light/dark and persists the choice

// Toolbar button command IDs reuse the menu IDs above.

// Menu / accelerator resource
#define IDR_MAINMENU         2001
#define IDR_ACCEL            2002
#define IDI_APPICON          2003
