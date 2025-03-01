// Copyright 2015 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef UI_ACCESSIBILITY_PLATFORM_AX_PLATFORM_NODE_WIN_H_
#define UI_ACCESSIBILITY_PLATFORM_AX_PLATFORM_NODE_WIN_H_

#include <objbase.h>
#include <oleacc.h>
#include <oleauto.h>
#include <uiautomation.h>
#include <wrl/client.h>

#include <array>
#include <map>
#include <string>
#include <vector>

#include "base/compiler_specific.h"
#include "base/metrics/histogram_macros.h"
#include "base/observer_list.h"
#include "base/win/atl.h"
#include "third_party/iaccessible2/ia2_api_all.h"
#include "ui/accessibility/ax_export.h"
#include "ui/accessibility/ax_text_utils.h"
#include "ui/accessibility/platform/ax_platform_node_base.h"
#include "ui/gfx/range/range.h"

// IMPORTANT!
// These values are written to logs.  Do not renumber or delete
// existing items; add new entries to the end of the list.
enum {
  UMA_API_ACC_DO_DEFAULT_ACTION = 0,
  UMA_API_ACC_HIT_TEST = 1,
  UMA_API_ACC_LOCATION = 2,
  UMA_API_ACC_NAVIGATE = 3,
  UMA_API_ACC_SELECT = 4,
  UMA_API_ADD_SELECTION = 5,
  UMA_API_CONVERT_RETURNED_ELEMENT = 6,
  UMA_API_DO_ACTION = 7,
  UMA_API_GET_ACCESSIBLE_AT = 8,
  UMA_API_GET_ACC_CHILD = 9,
  UMA_API_GET_ACC_CHILD_COUNT = 10,
  UMA_API_GET_ACC_DEFAULT_ACTION = 11,
  UMA_API_GET_ACC_DESCRIPTION = 12,
  UMA_API_GET_ACC_FOCUS = 13,
  UMA_API_GET_ACC_HELP = 14,
  UMA_API_GET_ACC_HELP_TOPIC = 15,
  UMA_API_GET_ACC_KEYBOARD_SHORTCUT = 16,
  UMA_API_GET_ACC_NAME = 17,
  UMA_API_GET_ACC_PARENT = 18,
  UMA_API_GET_ACC_ROLE = 19,
  UMA_API_GET_ACC_SELECTION = 20,
  UMA_API_GET_ACC_STATE = 21,
  UMA_API_GET_ACC_VALUE = 22,
  UMA_API_GET_ANCHOR = 23,
  UMA_API_GET_ANCHOR_TARGET = 24,
  UMA_API_GET_APP_NAME = 25,
  UMA_API_GET_APP_VERSION = 26,
  UMA_API_GET_ATTRIBUTES_FOR_NAMES = 27,
  UMA_API_GET_CAPTION = 28,
  UMA_API_GET_CARET_OFFSET = 29,
  UMA_API_GET_CELL_AT = 30,
  UMA_API_GET_CHARACTER_EXTENTS = 31,
  UMA_API_GET_CHILD_AT = 32,
  UMA_API_GET_CHILD_INDEX = 33,
  UMA_API_GET_CLIPPED_SUBSTRING_BOUNDS = 34,
  UMA_API_GET_COLUMN_DESCRIPTION = 35,
  UMA_API_GET_COLUMN_EXTENT = 36,
  UMA_API_GET_COLUMN_EXTENT_AT = 37,
  UMA_API_GET_COLUMN_HEADER = 38,
  UMA_API_GET_COLUMN_HEADER_CELLS = 39,
  UMA_API_GET_COLUMN_INDEX = 40,
  UMA_API_GET_COMPUTED_STYLE = 41,
  UMA_API_GET_COMPUTED_STYLE_FOR_PROPERTIES = 42,
  UMA_API_GET_CURRENT_VALUE = 43,
  UMA_API_GET_DESCRIPTION = 44,
  UMA_API_GET_DOC_TYPE = 45,
  UMA_API_GET_DOM_TEXT = 46,
  UMA_API_GET_END_INDEX = 47,
  UMA_API_GET_EXTENDED_ROLE = 48,
  UMA_API_GET_EXTENDED_STATES = 49,
  UMA_API_GET_FIRST_CHILD = 50,
  UMA_API_GET_FONT_FAMILY = 51,
  UMA_API_GET_GROUP_POSITION = 52,
  UMA_API_GET_HOST_RAW_ELEMENT_PROVIDER = 53,
  UMA_API_GET_HYPERLINK = 54,
  UMA_API_GET_HYPERLINK_INDEX = 55,
  UMA_API_GET_IACCESSIBLE_PAIR = 56,
  UMA_API_GET_IMAGE_POSITION = 57,
  UMA_API_GET_IMAGE_SIZE = 58,
  UMA_API_GET_INDEX_IN_PARENT = 59,
  UMA_API_GET_INNER_HTML = 60,
  UMA_API_GET_IS_COLUMN_SELECTED = 61,
  UMA_API_GET_IS_ROW_SELECTED = 62,
  UMA_API_GET_IS_SELECTED = 63,
  UMA_API_GET_KEY_BINDING = 64,
  UMA_API_GET_LANGUAGE = 65,
  UMA_API_GET_LAST_CHILD = 66,
  UMA_API_GET_LOCALE = 67,
  UMA_API_GET_LOCALIZED_EXTENDED_ROLE = 68,
  UMA_API_GET_LOCALIZED_EXTENDED_STATES = 69,
  UMA_API_GET_LOCALIZED_NAME = 70,
  UMA_API_GET_LOCAL_INTERFACE = 71,
  UMA_API_GET_MAXIMUM_VALUE = 72,
  UMA_API_GET_MIME_TYPE = 73,
  UMA_API_GET_MINIMUM_VALUE = 74,
  UMA_API_GET_NAME = 75,
  UMA_API_GET_NAMESPACE_URI_FOR_ID = 76,
  UMA_API_GET_NEW_TEXT = 77,
  UMA_API_GET_NEXT_SIBLING = 78,
  UMA_API_GET_NODE_INFO = 79,
  UMA_API_GET_N_CHARACTERS = 80,
  UMA_API_GET_N_COLUMNS = 81,
  UMA_API_GET_N_EXTENDED_STATES = 82,
  UMA_API_GET_N_HYPERLINKS = 83,
  UMA_API_GET_N_RELATIONS = 84,
  UMA_API_GET_N_ROWS = 85,
  UMA_API_GET_N_SELECTED_CELLS = 86,
  UMA_API_GET_N_SELECTED_CHILDREN = 87,
  UMA_API_GET_N_SELECTED_COLUMNS = 88,
  UMA_API_GET_N_SELECTED_ROWS = 89,
  UMA_API_GET_N_SELECTIONS = 90,
  UMA_API_GET_OBJECT_FOR_CHILD = 91,
  UMA_API_GET_OFFSET_AT_POINT = 92,
  UMA_API_GET_OLD_TEXT = 93,
  UMA_API_GET_PARENT_NODE = 94,
  UMA_API_GET_PATTERN_PROVIDER = 95,
  UMA_API_GET_PREVIOUS_SIBLING = 96,
  UMA_API_GET_PROPERTY_VALUE = 97,
  UMA_API_GET_PROVIDER_OPTIONS = 98,
  UMA_API_GET_RELATION = 99,
  UMA_API_GET_RELATIONS = 100,
  UMA_API_GET_ROW_COLUMN_EXTENTS = 101,
  UMA_API_GET_ROW_COLUMN_EXTENTS_AT_INDEX = 102,
  UMA_API_GET_ROW_DESCRIPTION = 103,
  UMA_API_GET_ROW_EXTENT = 104,
  UMA_API_GET_ROW_EXTENT_AT = 105,
  UMA_API_GET_ROW_HEADER = 106,
  UMA_API_GET_ROW_HEADER_CELLS = 107,
  UMA_API_GET_ROW_INDEX = 108,
  UMA_API_GET_RUNTIME_ID = 109,
  UMA_API_GET_SELECTED_CELLS = 110,
  UMA_API_GET_SELECTED_CHILDREN = 111,
  UMA_API_GET_SELECTED_COLUMNS = 112,
  UMA_API_GET_SELECTED_ROWS = 113,
  UMA_API_GET_SELECTION = 114,
  UMA_API_GET_START_INDEX = 115,
  UMA_API_GET_STATES = 116,
  UMA_API_GET_SUMMARY = 117,
  UMA_API_GET_TABLE = 118,
  UMA_API_GET_TEXT = 119,
  UMA_API_GET_TEXT_AFTER_OFFSET = 120,
  UMA_API_GET_TEXT_AT_OFFSET = 121,
  UMA_API_GET_TEXT_BEFORE_OFFSET = 122,
  UMA_API_GET_TITLE = 123,
  UMA_API_GET_TOOLKIT_NAME = 124,
  UMA_API_GET_TOOLKIT_VERSION = 125,
  UMA_API_GET_UNCLIPPED_SUBSTRING_BOUNDS = 126,
  UMA_API_GET_UNIQUE_ID = 127,
  UMA_API_GET_URL = 128,
  UMA_API_GET_VALID = 129,
  UMA_API_GET_WINDOW_HANDLE = 130,
  UMA_API_IA2_GET_ATTRIBUTES = 131,
  UMA_API_IA2_SCROLL_TO = 132,
  UMA_API_IAACTION_GET_DESCRIPTION = 133,
  UMA_API_IATEXT_GET_ATTRIBUTES = 134,
  UMA_API_ISIMPLEDOMNODE_GET_ATTRIBUTES = 135,
  UMA_API_ISIMPLEDOMNODE_SCROLL_TO = 136,
  UMA_API_N_ACTIONS = 137,
  UMA_API_PUT_ALTERNATE_VIEW_MEDIA_TYPES = 138,
  UMA_API_QUERY_SERVICE = 139,
  UMA_API_REMOVE_SELECTION = 140,
  UMA_API_ROLE = 141,
  UMA_API_SCROLL_SUBSTRING_TO = 142,
  UMA_API_SCROLL_SUBSTRING_TO_POINT = 143,
  UMA_API_SCROLL_TO_POINT = 144,
  UMA_API_SCROLL_TO_SUBSTRING = 145,
  UMA_API_SELECT_COLUMN = 146,
  UMA_API_SELECT_ROW = 147,
  UMA_API_SET_CARET_OFFSET = 148,
  UMA_API_SET_CURRENT_VALUE = 149,
  UMA_API_SET_SELECTION = 150,
  UMA_API_TABLE2_GET_SELECTED_COLUMNS = 151,
  UMA_API_TABLE2_GET_SELECTED_ROWS = 152,
  UMA_API_TABLECELL_GET_COLUMN_INDEX = 153,
  UMA_API_TABLECELL_GET_IS_SELECTED = 154,
  UMA_API_TABLECELL_GET_ROW_INDEX = 155,
  UMA_API_UNSELECT_COLUMN = 156,
  UMA_API_UNSELECT_ROW = 157,
  UMA_API_GET_BOUNDINGRECTANGLE = 158,
  UMA_API_GET_FRAGMENTROOT = 159,
  UMA_API_GETEMBEDDEDFRAGMENTROOTS = 160,
  UMA_API_NAVIGATE = 161,
  UMA_API_SETFOCUS = 162,
  UMA_API_SHOWCONTEXTMENU = 163,
  UMA_API_EXPANDCOLLAPSE_COLLAPSE = 164,
  UMA_API_EXPANDCOLLAPSE_EXPAND = 165,
  UMA_API_EXPANDCOLLAPSE_GET_EXPANDCOLLAPSESTATE = 166,
  UMA_API_GRIDITEM_GET_COLUMN = 167,
  UMA_API_GRIDITEM_GET_COLUMNSPAN = 168,
  UMA_API_GRIDITEM_GET_CONTAININGGRID = 169,
  UMA_API_GRIDITEM_GET_ROW = 170,
  UMA_API_GRIDITEM_GET_ROWSPAN = 171,
  UMA_API_GRID_GETITEM = 172,
  UMA_API_GRID_GET_ROWCOUNT = 173,
  UMA_API_GRID_GET_COLUMNCOUNT = 174,
  UMA_API_INVOKE_INVOKE = 175,
  UMA_API_RANGEVALUE_SETVALUE = 176,
  UMA_API_RANGEVALUE_GET_LARGECHANGE = 177,
  UMA_API_RANGEVALUE_GET_MAXIMUM = 178,
  UMA_API_RANGEVALUE_GET_MINIMUM = 179,
  UMA_API_RANGEVALUE_GET_SMALLCHANGE = 180,
  UMA_API_RANGEVALUE_GET_VALUE = 181,
  UMA_API_SCROLLITEM_SCROLLINTOVIEW = 182,
  UMA_API_SCROLL_SCROLL = 183,
  UMA_API_SCROLL_SETSCROLLPERCENT = 184,
  UMA_API_SCROLL_GET_HORIZONTALLYSCROLLABLE = 185,
  UMA_API_SCROLL_GET_HORIZONTALSCROLLPERCENT = 186,
  UMA_API_SCROLL_GET_HORIZONTALVIEWSIZE = 187,
  UMA_API_SCROLL_GET_VERTICALLYSCROLLABLE = 188,
  UMA_API_SCROLL_GET_VERTICALSCROLLPERCENT = 189,
  UMA_API_SCROLL_GET_VERTICALVIEWSIZE = 190,
  UMA_API_SELECTIONITEM_ADDTOSELECTION = 191,
  UMA_API_SELECTIONITEM_REMOVEFROMSELECTION = 192,
  UMA_API_SELECTIONITEM_SELECT = 193,
  UMA_API_SELECTIONITEM_GET_ISSELECTED = 194,
  UMA_API_SELECTIONITEM_GET_SELECTIONCONTAINER = 195,
  UMA_API_SELECTION_GETSELECTION = 196,
  UMA_API_SELECTION_GET_CANSELECTMULTIPLE = 197,
  UMA_API_SELECTION_GET_ISSELECTIONREQUIRED = 198,
  UMA_API_TABLEITEM_GETCOLUMNHEADERITEMS = 199,
  UMA_API_TABLEITEM_GETROWHEADERITEMS = 200,
  UMA_API_TABLE_GETCOLUMNHEADERS = 201,
  UMA_API_TABLE_GETROWHEADERS = 202,
  UMA_API_TABLE_GET_ROWORCOLUMNMAJOR = 203,
  UMA_API_TEXT_GETSELECTION = 204,
  UMA_API_TEXT_GETVISIBLERANGES = 205,
  UMA_API_TEXT_RANGEFROMCHILD = 206,
  UMA_API_TEXT_RANGEFROMPOINT = 207,
  UMA_API_TEXT_GET_DOCUMENTRANGE = 208,
  UMA_API_TEXT_GET_SUPPORTEDTEXTSELECTION = 209,
  UMA_API_TEXTCHILD_GET_TEXTCONTAINER = 210,
  UMA_API_TEXTCHILD_GET_TEXTRANGE = 211,
  UMA_API_TEXTEDIT_GETACTIVECOMPOSITION = 212,
  UMA_API_TEXTEDIT_GETCONVERSIONTARGET = 213,
  UMA_API_TEXTRANGE_CLONE = 214,
  UMA_API_TEXTRANGE_COMPARE = 215,
  UMA_API_TEXTRANGE_COMPAREENDPOINTS = 216,
  UMA_API_TEXTRANGE_EXPANDTOENCLOSINGUNIT = 217,
  UMA_API_TEXTRANGE_FINDATTRIBUTE = 218,
  UMA_API_TEXTRANGE_FINDTEXT = 219,
  UMA_API_TEXTRANGE_GETATTRIBUTEVALUE = 220,
  UMA_API_TEXTRANGE_GETBOUNDINGRECTANGLES = 221,
  UMA_API_TEXTRANGE_GETENCLOSINGELEMENT = 222,
  UMA_API_TEXTRANGE_GETTEXT = 223,
  UMA_API_TEXTRANGE_MOVE = 224,
  UMA_API_TEXTRANGE_MOVEENDPOINTBYUNIT = 225,
  UMA_API_TEXTRANGE_MOVEENPOINTBYRANGE = 226,
  UMA_API_TEXTRANGE_SELECT = 227,
  UMA_API_TEXTRANGE_ADDTOSELECTION = 228,
  UMA_API_TEXTRANGE_REMOVEFROMSELECTION = 229,
  UMA_API_TEXTRANGE_SCROLLINTOVIEW = 230,
  UMA_API_TEXTRANGE_GETCHILDREN = 231,
  UMA_API_TOGGLE_TOGGLE = 232,
  UMA_API_TOGGLE_GET_TOGGLESTATE = 233,
  UMA_API_VALUE_SETVALUE = 234,
  UMA_API_VALUE_GET_ISREADONLY = 235,
  UMA_API_VALUE_GET_VALUE = 236,
  UMA_API_WINDOW_SETVISUALSTATE = 237,
  UMA_API_WINDOW_CLOSE = 238,
  UMA_API_WINDOW_WAITFORINPUTIDLE = 239,
  UMA_API_WINDOW_GET_CANMAXIMIZE = 240,
  UMA_API_WINDOW_GET_CANMINIMIZE = 241,
  UMA_API_WINDOW_GET_ISMODAL = 242,
  UMA_API_WINDOW_GET_WINDOWVISUALSTATE = 243,
  UMA_API_WINDOW_GET_WINDOWINTERACTIONSTATE = 244,
  UMA_API_WINDOW_GET_ISTOPMOST = 245,

  // This must always be the last enum. It's okay for its value to
  // increase, but none of the other enum values may change.
  UMA_API_MAX
};

#define WIN_ACCESSIBILITY_API_HISTOGRAM(enum_value) \
  UMA_HISTOGRAM_ENUMERATION("Accessibility.WinAPIs", enum_value, UMA_API_MAX)

//
// Macros to use at the top of any AXPlatformNodeWin (or derived class) method
// that implements a UIA COM interface. The error code UIA_E_ELEMENTNOTAVAILABLE
// signals to the OS that the object is no longer valid and no further methods
// should be called on it.
//
#define UIA_VALIDATE_CALL()               \
  if (!AXPlatformNodeBase::GetDelegate()) \
    return UIA_E_ELEMENTNOTAVAILABLE;
#define UIA_VALIDATE_CALL_1_ARG(arg)      \
  if (!AXPlatformNodeBase::GetDelegate()) \
    return UIA_E_ELEMENTNOTAVAILABLE;     \
  if (!arg)                               \
    return E_INVALIDARG;                  \
  *arg = {};

namespace ui {
class AXPlatformNodeWin;
class AXPlatformRelationWin;

// A simple interface for a class that wants to be notified when IAccessible2
// is used by a client, a strong indication that full accessibility support
// should be enabled.
//
// TODO(dmazzoni): Rename this to something more general.
class AX_EXPORT IAccessible2UsageObserver {
 public:
  IAccessible2UsageObserver();
  virtual ~IAccessible2UsageObserver();
  virtual void OnIAccessible2Used() = 0;
  virtual void OnScreenReaderHoneyPotQueried() = 0;
  virtual void OnAccNameCalled() = 0;
};

// Get an observer list that allows modules across the codebase to
// listen to when usage of IAccessible2 is detected.
extern AX_EXPORT base::ObserverList<IAccessible2UsageObserver>::Unchecked&
GetIAccessible2UsageObserverList();

// TODO(nektar): Remove multithread superclass since we don't support it.
class AX_EXPORT __declspec(uuid("26f5641a-246d-457b-a96d-07f3fae6acf2"))
    AXPlatformNodeWin : public CComObjectRootEx<CComMultiThreadModel>,
                        public IDispatchImpl<IAccessible2_4,
                                             &IID_IAccessible2_4,
                                             &LIBID_IAccessible2Lib>,
                        public IAccessibleEx,
                        public IAccessibleText,
                        public IAccessibleTable,
                        public IAccessibleTable2,
                        public IAccessibleTableCell,
                        public IExpandCollapseProvider,
                        public IGridItemProvider,
                        public IGridProvider,
                        public IInvokeProvider,
                        public IRangeValueProvider,
                        public IRawElementProviderFragment,
                        public IRawElementProviderSimple2,
                        public IScrollItemProvider,
                        public IScrollProvider,
                        public ISelectionItemProvider,
                        public ISelectionProvider,
                        public IServiceProvider,
                        public ITableItemProvider,
                        public ITableProvider,
                        public IToggleProvider,
                        public IValueProvider,
                        public IWindowProvider,
                        public AXPlatformNodeBase {
  using IDispatchImpl::Invoke;

 public:
  BEGIN_COM_MAP(AXPlatformNodeWin)
    // TODO(nektar): Change the following to COM_INTERFACE_ENTRY(IDispatch).
    COM_INTERFACE_ENTRY2(IDispatch, IAccessible2_2)
    COM_INTERFACE_ENTRY2(IUnknown, IDispatchImpl)
    // TODO(nektar): Find a way to remove the following entry because it's not
    // an interface.
    COM_INTERFACE_ENTRY(AXPlatformNodeWin)
    COM_INTERFACE_ENTRY(IAccessible)
    COM_INTERFACE_ENTRY(IAccessible2)
    COM_INTERFACE_ENTRY(IAccessible2_2)
    COM_INTERFACE_ENTRY(IAccessible2_3)
    COM_INTERFACE_ENTRY(IAccessible2_4)
    COM_INTERFACE_ENTRY(IAccessibleEx)
    COM_INTERFACE_ENTRY(IAccessibleText)
    COM_INTERFACE_ENTRY(IAccessibleTable)
    COM_INTERFACE_ENTRY(IAccessibleTable2)
    COM_INTERFACE_ENTRY(IAccessibleTableCell)
    COM_INTERFACE_ENTRY(IExpandCollapseProvider)
    COM_INTERFACE_ENTRY(IGridItemProvider)
    COM_INTERFACE_ENTRY(IGridProvider)
    COM_INTERFACE_ENTRY(IInvokeProvider)
    COM_INTERFACE_ENTRY(IRangeValueProvider)
    COM_INTERFACE_ENTRY(IRawElementProviderFragment)
    COM_INTERFACE_ENTRY(IRawElementProviderSimple)
    COM_INTERFACE_ENTRY(IRawElementProviderSimple2)
    COM_INTERFACE_ENTRY(IScrollItemProvider)
    COM_INTERFACE_ENTRY(IScrollProvider)
    COM_INTERFACE_ENTRY(ISelectionItemProvider)
    COM_INTERFACE_ENTRY(ISelectionProvider)
    COM_INTERFACE_ENTRY(ITableItemProvider)
    COM_INTERFACE_ENTRY(ITableProvider)
    COM_INTERFACE_ENTRY(IToggleProvider)
    COM_INTERFACE_ENTRY(IValueProvider)
    COM_INTERFACE_ENTRY(IWindowProvider)
    COM_INTERFACE_ENTRY(IServiceProvider)
  END_COM_MAP()

  ~AXPlatformNodeWin() override;

  void Init(AXPlatformNodeDelegate* delegate) override;

  // Clear any AXPlatformRelationWin nodes owned by this node.
  void ClearOwnRelations();

  // AXPlatformNode overrides.
  gfx::NativeViewAccessible GetNativeViewAccessible() override;
  void NotifyAccessibilityEvent(ax::mojom::Event event_type) override;

  // AXPlatformNodeBase overrides.
  void Destroy() override;
  int GetIndexInParent() override;
  base::string16 GetValue() const override;
  base::string16 GetText() const override;

  //
  // IAccessible methods.
  //

  // Retrieves the child element or child object at a given point on the screen.
  IFACEMETHODIMP accHitTest(LONG x_left, LONG y_top, VARIANT* child) override;

  // Performs the object's default action.
  IFACEMETHODIMP accDoDefaultAction(VARIANT var_id) override;

  // Retrieves the specified object's current screen location.
  IFACEMETHODIMP accLocation(LONG* x_left,
                             LONG* y_top,
                             LONG* width,
                             LONG* height,
                             VARIANT var_id) override;

  // Traverses to another UI element and retrieves the object.
  IFACEMETHODIMP accNavigate(LONG nav_dir,
                             VARIANT start,
                             VARIANT* end) override;

  // Retrieves an IDispatch interface pointer for the specified child.
  IFACEMETHODIMP get_accChild(VARIANT var_child,
                              IDispatch** disp_child) override;

  // Retrieves the number of accessible children.
  IFACEMETHODIMP get_accChildCount(LONG* child_count) override;

  // Retrieves a string that describes the object's default action.
  IFACEMETHODIMP get_accDefaultAction(VARIANT var_id,
                                      BSTR* default_action) override;

  // Retrieves the tooltip description.
  IFACEMETHODIMP get_accDescription(VARIANT var_id, BSTR* desc) override;

  // Retrieves the object that has the keyboard focus.
  IFACEMETHODIMP get_accFocus(VARIANT* focus_child) override;

  // Retrieves the specified object's shortcut.
  IFACEMETHODIMP get_accKeyboardShortcut(VARIANT var_id,
                                         BSTR* access_key) override;

  // Retrieves the name of the specified object.
  IFACEMETHODIMP get_accName(VARIANT var_id, BSTR* name) override;

  // Retrieves the IDispatch interface of the object's parent.
  IFACEMETHODIMP get_accParent(IDispatch** disp_parent) override;

  // Retrieves information describing the role of the specified object.
  IFACEMETHODIMP get_accRole(VARIANT var_id, VARIANT* role) override;

  // Retrieves the current state of the specified object.
  IFACEMETHODIMP get_accState(VARIANT var_id, VARIANT* state) override;

  // Gets the help string for the specified object.
  IFACEMETHODIMP get_accHelp(VARIANT var_id, BSTR* help) override;

  // Retrieve or set the string value associated with the specified object.
  // Setting the value is not typically used by screen readers, but it's
  // used frequently by automation software.
  IFACEMETHODIMP get_accValue(VARIANT var_id, BSTR* value) override;
  IFACEMETHODIMP put_accValue(VARIANT var_id, BSTR new_value) override;

  // IAccessible methods not implemented.
  IFACEMETHODIMP get_accSelection(VARIANT* selected) override;
  IFACEMETHODIMP accSelect(LONG flags_sel, VARIANT var_id) override;
  IFACEMETHODIMP get_accHelpTopic(BSTR* help_file,
                                  VARIANT var_id,
                                  LONG* topic_id) override;
  IFACEMETHODIMP put_accName(VARIANT var_id, BSTR put_name) override;

  //
  // IAccessible2 methods.
  //

  IFACEMETHODIMP role(LONG* role) override;

  IFACEMETHODIMP get_states(AccessibleStates* states) override;

  IFACEMETHODIMP get_uniqueID(LONG* unique_id) override;

  IFACEMETHODIMP get_windowHandle(HWND* window_handle) override;

  IFACEMETHODIMP get_relationTargetsOfType(BSTR type,
                                           LONG max_targets,
                                           IUnknown*** targets,
                                           LONG* n_targets) override;

  IFACEMETHODIMP get_attributes(BSTR* attributes) override;

  IFACEMETHODIMP get_indexInParent(LONG* index_in_parent) override;

  IFACEMETHODIMP get_nRelations(LONG* n_relations) override;

  IFACEMETHODIMP get_relation(LONG relation_index,
                              IAccessibleRelation** relation) override;

  IFACEMETHODIMP get_relations(LONG max_relations,
                               IAccessibleRelation** relations,
                               LONG* n_relations) override;

  IFACEMETHODIMP get_attribute(BSTR name, VARIANT* attribute) override;
  IFACEMETHODIMP get_extendedRole(BSTR* extended_role) override;
  IFACEMETHODIMP scrollTo(enum IA2ScrollType scroll_type) override;
  IFACEMETHODIMP scrollToPoint(enum IA2CoordinateType coordinate_type,
                               LONG x,
                               LONG y) override;
  IFACEMETHODIMP get_groupPosition(LONG* group_level,
                                   LONG* similar_items_in_group,
                                   LONG* position_in_group) override;
  IFACEMETHODIMP get_localizedExtendedRole(
      BSTR* localized_extended_role) override;
  IFACEMETHODIMP get_nExtendedStates(LONG* n_extended_states) override;
  IFACEMETHODIMP get_extendedStates(LONG max_extended_states,
                                    BSTR** extended_states,
                                    LONG* n_extended_states) override;
  IFACEMETHODIMP get_localizedExtendedStates(
      LONG max_localized_extended_states,
      BSTR** localized_extended_states,
      LONG* n_localized_extended_states) override;
  IFACEMETHODIMP get_locale(IA2Locale* locale) override;
  IFACEMETHODIMP get_accessibleWithCaret(IUnknown** accessible,
                                         LONG* caret_offset) override;

  //
  // IAccessible2_3 methods.
  //

  IFACEMETHODIMP get_selectionRanges(IA2Range** ranges, LONG* nRanges);

  //
  // IAccessible2_4 methods.
  //

  IFACEMETHODIMP setSelectionRanges(LONG nRanges, IA2Range* ranges);

  //
  // IAccessibleEx methods.
  //

  IFACEMETHODIMP GetObjectForChild(LONG child_id,
                                   IAccessibleEx** result) override;

  IFACEMETHODIMP GetIAccessiblePair(IAccessible** accessible,
                                    LONG* child_id) override;

  //
  // IExpandCollapseProvider methods.
  //

  IFACEMETHODIMP Collapse() override;

  IFACEMETHODIMP Expand() override;

  IFACEMETHODIMP get_ExpandCollapseState(ExpandCollapseState* result) override;

  //
  // IGridItemProvider methods.
  //

  IFACEMETHODIMP get_Column(int* result) override;

  IFACEMETHODIMP get_ColumnSpan(int* result) override;

  IFACEMETHODIMP get_ContainingGrid(
      IRawElementProviderSimple** result) override;

  IFACEMETHODIMP get_Row(int* result) override;

  IFACEMETHODIMP get_RowSpan(int* result) override;

  //
  // IGridProvider methods.
  //

  IFACEMETHODIMP GetItem(int row,
                         int column,
                         IRawElementProviderSimple** result) override;

  IFACEMETHODIMP get_RowCount(int* result) override;

  IFACEMETHODIMP get_ColumnCount(int* result) override;

  //
  // IInvokeProvider methods.
  //

  IFACEMETHODIMP Invoke() override;

  //
  // IScrollItemProvider methods.
  //

  IFACEMETHODIMP ScrollIntoView() override;

  //
  // IScrollProvider methods.
  //

  IFACEMETHODIMP Scroll(ScrollAmount horizontal_amount,
                        ScrollAmount vertical_amount) override;

  IFACEMETHODIMP SetScrollPercent(double horizontal_percent,
                                  double vertical_percent) override;

  IFACEMETHODIMP get_HorizontallyScrollable(BOOL* result) override;

  IFACEMETHODIMP get_HorizontalScrollPercent(double* result) override;

  // Horizontal size of the viewable region as a percentage of the total content
  // area.
  IFACEMETHODIMP get_HorizontalViewSize(double* result) override;

  IFACEMETHODIMP get_VerticallyScrollable(BOOL* result) override;

  IFACEMETHODIMP get_VerticalScrollPercent(double* result) override;

  // Vertical size of the viewable region as a percentage of the total content
  // area.
  IFACEMETHODIMP get_VerticalViewSize(double* result) override;

  //
  // ISelectionItemProvider methods.
  //

  IFACEMETHODIMP AddToSelection() override;

  IFACEMETHODIMP RemoveFromSelection() override;

  IFACEMETHODIMP Select() override;

  IFACEMETHODIMP get_IsSelected(BOOL* result) override;

  IFACEMETHODIMP get_SelectionContainer(
      IRawElementProviderSimple** result) override;

  //
  // ISelectionProvider methods.
  //

  IFACEMETHODIMP GetSelection(SAFEARRAY** result) override;

  IFACEMETHODIMP get_CanSelectMultiple(BOOL* result) override;

  IFACEMETHODIMP get_IsSelectionRequired(BOOL* result) override;

  //
  // ITableItemProvider methods.
  //

  IFACEMETHODIMP GetColumnHeaderItems(SAFEARRAY** result) override;

  IFACEMETHODIMP GetRowHeaderItems(SAFEARRAY** result) override;

  //
  // ITableProvider methods.
  //

  IFACEMETHODIMP GetColumnHeaders(SAFEARRAY** result) override;

  IFACEMETHODIMP GetRowHeaders(SAFEARRAY** result) override;

  IFACEMETHODIMP get_RowOrColumnMajor(RowOrColumnMajor* result) override;

  //
  // IToggleProvider methods.
  //

  IFACEMETHODIMP Toggle() override;

  IFACEMETHODIMP get_ToggleState(ToggleState* result) override;

  //
  // IValueProvider methods.
  //

  IFACEMETHODIMP SetValue(LPCWSTR val) override;

  IFACEMETHODIMP get_IsReadOnly(BOOL* result) override;

  IFACEMETHODIMP get_Value(BSTR* result) override;

  //
  // IWindowProvider methods.
  //

  IFACEMETHODIMP SetVisualState(WindowVisualState window_visual_state) override;

  IFACEMETHODIMP Close() override;

  IFACEMETHODIMP WaitForInputIdle(int milliseconds, BOOL* result) override;

  IFACEMETHODIMP get_CanMaximize(BOOL* result) override;

  IFACEMETHODIMP get_CanMinimize(BOOL* result) override;

  IFACEMETHODIMP get_IsModal(BOOL* result) override;

  IFACEMETHODIMP get_WindowVisualState(WindowVisualState* result) override;

  IFACEMETHODIMP get_WindowInteractionState(
      WindowInteractionState* result) override;

  IFACEMETHODIMP get_IsTopmost(BOOL* result) override;

  //
  // IRangeValueProvider methods.
  //

  IFACEMETHODIMP SetValue(double val) override;

  IFACEMETHODIMP get_LargeChange(double* result) override;

  IFACEMETHODIMP get_Maximum(double* result) override;

  IFACEMETHODIMP get_Minimum(double* result) override;

  IFACEMETHODIMP get_SmallChange(double* result) override;

  IFACEMETHODIMP get_Value(double* result) override;

  // IAccessibleEx methods not implemented.
  IFACEMETHODIMP
  ConvertReturnedElement(IRawElementProviderSimple* element,
                         IAccessibleEx** acc) override;

  //
  // IAccessibleText methods.
  //

  IFACEMETHODIMP get_nCharacters(LONG* n_characters) override;

  IFACEMETHODIMP get_caretOffset(LONG* offset) override;

  IFACEMETHODIMP get_nSelections(LONG* n_selections) override;

  IFACEMETHODIMP get_selection(LONG selection_index,
                               LONG* start_offset,
                               LONG* end_offset) override;

  IFACEMETHODIMP get_text(LONG start_offset,
                          LONG end_offset,
                          BSTR* text) override;

  IFACEMETHODIMP get_textAtOffset(LONG offset,
                                  enum IA2TextBoundaryType boundary_type,
                                  LONG* start_offset,
                                  LONG* end_offset,
                                  BSTR* text) override;

  IFACEMETHODIMP get_textBeforeOffset(LONG offset,
                                      enum IA2TextBoundaryType boundary_type,
                                      LONG* start_offset,
                                      LONG* end_offset,
                                      BSTR* text) override;

  IFACEMETHODIMP get_textAfterOffset(LONG offset,
                                     enum IA2TextBoundaryType boundary_type,
                                     LONG* start_offset,
                                     LONG* end_offset,
                                     BSTR* text) override;

  IFACEMETHODIMP get_offsetAtPoint(LONG x,
                                   LONG y,
                                   enum IA2CoordinateType coord_type,
                                   LONG* offset) override;

  //
  // IAccessibleTable methods.
  //

  // get_description - also used by IAccessibleImage

  IFACEMETHODIMP get_accessibleAt(LONG row,
                                  LONG column,
                                  IUnknown** accessible) override;

  IFACEMETHODIMP get_caption(IUnknown** accessible) override;

  IFACEMETHODIMP get_childIndex(LONG row_index,
                                LONG column_index,
                                LONG* cell_index) override;

  IFACEMETHODIMP get_columnDescription(LONG column, BSTR* description) override;

  IFACEMETHODIMP
  get_columnExtentAt(LONG row, LONG column, LONG* n_columns_spanned) override;

  IFACEMETHODIMP
  get_columnHeader(IAccessibleTable** accessible_table,
                   LONG* starting_row_index) override;

  IFACEMETHODIMP get_columnIndex(LONG cell_index, LONG* column_index) override;

  IFACEMETHODIMP get_nColumns(LONG* column_count) override;

  IFACEMETHODIMP get_nRows(LONG* row_count) override;

  IFACEMETHODIMP get_nSelectedChildren(LONG* cell_count) override;

  IFACEMETHODIMP get_nSelectedColumns(LONG* column_count) override;

  IFACEMETHODIMP get_nSelectedRows(LONG* row_count) override;

  IFACEMETHODIMP get_rowDescription(LONG row, BSTR* description) override;

  IFACEMETHODIMP get_rowExtentAt(LONG row,
                                 LONG column,
                                 LONG* n_rows_spanned) override;

  IFACEMETHODIMP
  get_rowHeader(IAccessibleTable** accessible_table,
                LONG* starting_column_index) override;

  IFACEMETHODIMP get_rowIndex(LONG cell_index, LONG* row_index) override;

  IFACEMETHODIMP get_selectedChildren(LONG max_children,
                                      LONG** children,
                                      LONG* n_children) override;

  IFACEMETHODIMP get_selectedColumns(LONG max_columns,
                                     LONG** columns,
                                     LONG* n_columns) override;

  IFACEMETHODIMP get_selectedRows(LONG max_rows,
                                  LONG** rows,
                                  LONG* n_rows) override;

  IFACEMETHODIMP get_summary(IUnknown** accessible) override;

  IFACEMETHODIMP
  get_isColumnSelected(LONG column, boolean* is_selected) override;

  IFACEMETHODIMP get_isRowSelected(LONG row, boolean* is_selected) override;

  IFACEMETHODIMP get_isSelected(LONG row,
                                LONG column,
                                boolean* is_selected) override;

  IFACEMETHODIMP
  get_rowColumnExtentsAtIndex(LONG index,
                              LONG* row,
                              LONG* column,
                              LONG* row_extents,
                              LONG* column_extents,
                              boolean* is_selected) override;

  IFACEMETHODIMP selectRow(LONG row) override;

  IFACEMETHODIMP selectColumn(LONG column) override;

  IFACEMETHODIMP unselectRow(LONG row) override;

  IFACEMETHODIMP unselectColumn(LONG column) override;

  IFACEMETHODIMP
  get_modelChange(IA2TableModelChange* model_change) override;

  //
  // IAccessibleTable2 methods.
  //
  // (Most of these are duplicates of IAccessibleTable methods, only the
  // unique ones are included here.)
  //

  IFACEMETHODIMP get_cellAt(LONG row, LONG column, IUnknown** cell) override;

  IFACEMETHODIMP get_nSelectedCells(LONG* cell_count) override;

  IFACEMETHODIMP
  get_selectedCells(IUnknown*** cells, LONG* n_selected_cells) override;

  IFACEMETHODIMP get_selectedColumns(LONG** columns, LONG* n_columns) override;

  IFACEMETHODIMP get_selectedRows(LONG** rows, LONG* n_rows) override;

  //
  // IAccessibleTableCell methods.
  //

  IFACEMETHODIMP
  get_columnExtent(LONG* n_columns_spanned) override;

  IFACEMETHODIMP
  get_columnHeaderCells(IUnknown*** cell_accessibles,
                        LONG* n_column_header_cells) override;

  IFACEMETHODIMP get_columnIndex(LONG* column_index) override;

  IFACEMETHODIMP get_rowExtent(LONG* n_rows_spanned) override;

  IFACEMETHODIMP
  get_rowHeaderCells(IUnknown*** cell_accessibles,
                     LONG* n_row_header_cells) override;

  IFACEMETHODIMP get_rowIndex(LONG* row_index) override;

  IFACEMETHODIMP get_isSelected(boolean* is_selected) override;

  IFACEMETHODIMP
  get_rowColumnExtents(LONG* row,
                       LONG* column,
                       LONG* row_extents,
                       LONG* column_extents,
                       boolean* is_selected) override;

  IFACEMETHODIMP get_table(IUnknown** table) override;

  //
  // IAccessibleText methods not implemented.
  //

  IFACEMETHODIMP get_newText(IA2TextSegment* new_text) override;
  IFACEMETHODIMP get_oldText(IA2TextSegment* old_text) override;
  IFACEMETHODIMP addSelection(LONG start_offset, LONG end_offset) override;
  IFACEMETHODIMP get_attributes(LONG offset,
                                LONG* start_offset,
                                LONG* end_offset,
                                BSTR* text_attributes) override;
  IFACEMETHODIMP get_characterExtents(LONG offset,
                                      enum IA2CoordinateType coord_type,
                                      LONG* x,
                                      LONG* y,
                                      LONG* width,
                                      LONG* height) override;
  IFACEMETHODIMP removeSelection(LONG selection_index) override;
  IFACEMETHODIMP setCaretOffset(LONG offset) override;
  IFACEMETHODIMP setSelection(LONG selection_index,
                              LONG start_offset,
                              LONG end_offset) override;
  IFACEMETHODIMP scrollSubstringTo(LONG start_index,
                                   LONG end_index,
                                   enum IA2ScrollType scroll_type) override;
  IFACEMETHODIMP scrollSubstringToPoint(LONG start_index,
                                        LONG end_index,
                                        enum IA2CoordinateType coordinate_type,
                                        LONG x,
                                        LONG y) override;

  //
  // IRawElementProviderFragment methods.
  //

  IFACEMETHODIMP Navigate(
      NavigateDirection direction,
      IRawElementProviderFragment** element_provider) override;
  IFACEMETHODIMP GetRuntimeId(SAFEARRAY** runtime_id) override;
  IFACEMETHODIMP get_BoundingRectangle(UiaRect* bounding_rectangle) override;
  IFACEMETHODIMP GetEmbeddedFragmentRoots(
      SAFEARRAY** embedded_fragment_roots) override;
  IFACEMETHODIMP SetFocus() override;
  IFACEMETHODIMP get_FragmentRoot(
      IRawElementProviderFragmentRoot** fragment_root) override;

  //
  // IRawElementProviderSimple methods.
  //

  IFACEMETHODIMP GetPatternProvider(PATTERNID pattern_id,
                                    IUnknown** result) override;

  IFACEMETHODIMP GetPropertyValue(PROPERTYID property_id,
                                  VARIANT* result) override;

  IFACEMETHODIMP
  get_ProviderOptions(enum ProviderOptions* ret) override;

  IFACEMETHODIMP
  get_HostRawElementProvider(IRawElementProviderSimple** provider) override;

  //
  // IRawElementProviderSimple2 methods.
  //

  IFACEMETHODIMP ShowContextMenu() override;

  //
  // IServiceProvider methods.
  //

  IFACEMETHODIMP QueryService(REFGUID guidService,
                              REFIID riid,
                              void** object) override;

 public:
  // Support method for ITextRangeProvider::GetAttributeValue
  HRESULT GetTextAttributeValue(TEXTATTRIBUTEID attribute_id, VARIANT* result);

  // IRawElementProviderSimple support method.
  bool IsPatternProviderSupported(PATTERNID pattern_id);

  // Helper to return the runtime id (without going through a SAFEARRAY)
  using RuntimeIdArray = std::array<int, 2>;
  void GetRuntimeIdArray(RuntimeIdArray& runtime_id);

  // Notifies accessibility about active composition.
  void OnActiveComposition(const gfx::Range& range);
  // Returns true if there is an active composition
  bool HasActiveComposition() const;
  // Returns the start/end offsets of the active composition
  gfx::Range GetActiveCompositionOffsets() const;

 protected:
  // This is hard-coded; all products based on the Chromium engine will have the
  // same framework name, so that assistive technology can detect any
  // Chromium-based product.
  static constexpr const base::char16* FRAMEWORK_ID = L"Chrome";

  AXPlatformNodeWin();

  int MSAAState() const;

  int MSAARole();
  std::string StringOverrideForMSAARole();

  int32_t ComputeIA2State();

  int32_t ComputeIA2Role();

  std::vector<base::string16> ComputeIA2Attributes();

  base::string16 UIAAriaRole();

  base::string16 ComputeUIAProperties();

  LONG ComputeUIAControlType();

  bool IsUIAControl() const;

  base::Optional<LONG> ComputeUIALandmarkType() const;

  // AXPlatformNodeBase overrides.
  void Dispose() override;

  // Relationships between this node and other nodes.
  std::vector<Microsoft::WRL::ComPtr<AXPlatformRelationWin>> relations_;

  AXHypertext old_hypertext_;

  // These protected methods are still used by BrowserAccessibilityComWin. At
  // some point post conversion, we can probably move these to be private
  // methods.
  //
  // Helper methods for IA2 hyperlinks.
  //
  // Hyperlink is an IA2 misnomer. It refers to objects embedded within other
  // objects, such as a numbered list within a contenteditable div.
  // Also, in IA2, text that includes embedded objects is called hypertext.
  // Returns true if the current object is an IA2 hyperlink.
  bool IsHyperlink();
  void ComputeHypertextRemovedAndInserted(size_t* start,
                                          size_t* old_len,
                                          size_t* new_len);

  // If offset is a member of IA2TextSpecialOffsets this function updates the
  // value of offset and returns, otherwise offset remains unchanged.
  void HandleSpecialTextOffset(LONG* offset);

  // Convert from a IA2TextBoundaryType to a TextBoundaryType.
  TextBoundaryType IA2TextBoundaryToTextBoundary(IA2TextBoundaryType type);

  // A helper to add the given string value to |attributes|.
  void AddAttributeToList(const char* name,
                          const char* value,
                          PlatformAttributeList* attributes) override;

 private:
  base::Optional<DWORD> MSAAEvent(ax::mojom::Event event);
  base::Optional<EVENTID> UIAEvent(ax::mojom::Event event);
  bool IsWebAreaForPresentationalIframe();
  bool ShouldNodeHaveFocusableState(const AXNodeData& data) const;

  // Get the value attribute as a Bstr, this means something different depending
  // on the type of element being queried. (e.g. kColorWell uses kColorValue).
  static BSTR GetValueAttributeAsBstr(AXPlatformNodeWin* target);

  HRESULT GetStringAttributeAsBstr(ax::mojom::StringAttribute attribute,
                                   BSTR* value_bstr) const;

  // Sets the selection given a start and end offset in IA2 Hypertext.
  void SetIA2HypertextSelection(LONG start_offset, LONG end_offset);

  // Escapes characters in string attributes as required by the UIA Aria
  // Property Spec. It's okay for input to be the same as output.
  static void SanitizeStringAttributeForUIAAriaProperty(
      const base::string16& input,
      base::string16* output);

  // If the string attribute |attribute| is present, add its value as a
  // UIA AriaProperties Property with the name |uia_aria_property|.
  void StringAttributeToUIAAriaProperty(std::vector<base::string16>& properties,
                                        ax::mojom::StringAttribute attribute,
                                        const char* uia_aria_property);

  // If the bool attribute |attribute| is present, add its value as a
  // UIA AriaProperties Property with the name |uia_aria_property|.
  void BoolAttributeToUIAAriaProperty(std::vector<base::string16>& properties,
                                      ax::mojom::BoolAttribute attribute,
                                      const char* uia_aria_property);

  // If the int attribute |attribute| is present, add its value as a
  // UIA AriaProperties Property with the name |uia_aria_property|.
  void IntAttributeToUIAAriaProperty(std::vector<base::string16>& properties,
                                     ax::mojom::IntAttribute attribute,
                                     const char* uia_aria_property);

  // If the float attribute |attribute| is present, add its value as a
  // UIA AriaProperties Property with the name |uia_aria_property|.
  void FloatAttributeToUIAAriaProperty(std::vector<base::string16>& properties,
                                       ax::mojom::FloatAttribute attribute,
                                       const char* uia_aria_property);

  // If the state |state| exists, set the
  // UIA AriaProperties Property with the name |uia_aria_property| to "true".
  // Otherwise set the AriaProperties Property to "false".
  void StateToUIAAriaProperty(std::vector<base::string16>& properties,
                              ax::mojom::State state,
                              const char* uia_aria_property);

  // If the Html attribute |html_attribute_name| is present, add its value as a
  // UIA AriaProperties Property with the name |uia_aria_property|.
  void HtmlAttributeToUIAAriaProperty(std::vector<base::string16>& properties,
                                      const char* html_attribute_name,
                                      const char* uia_aria_property);

  // If the IntList attribute |attribute| is present, return an array
  // of automation elements referenced by the ids in the
  // IntList attribute. Otherwise return an empty array.
  // The function will skip over any ids that cannot be resolved.
  SAFEARRAY* CreateUIAElementsArrayForRelation(
      const ax::mojom::IntListAttribute& attribute);

  // Return an unordered array of automation elements which reference this node
  // for the given attribute.
  SAFEARRAY* CreateUIAElementsArrayForReverseRelation(
      const ax::mojom::IntListAttribute& attribute);

  // Return an array of automation elements given a vector
  // of |AXNode| ids.
  SAFEARRAY* CreateUIAElementsArrayFromIdVector(std::vector<int32_t>& ids);

  // Return an array that contains the center x, y coordinates of the
  // clickable point.
  SAFEARRAY* CreateClickablePointArray();

  // Returns the scroll offsets to which UI Automation should scroll an
  // accessible object, given the horizontal and vertical scroll amounts.
  gfx::Vector2d CalculateUIAScrollPoint(
      const ScrollAmount horizontal_amount,
      const ScrollAmount vertical_amount) const;

  void AddAlertTarget();
  void RemoveAlertTarget();

  // Return the text to use for IAccessibleText.
  base::string16 TextForIAccessibleText();

  // Search forwards (direction == 1) or backwards (direction == -1)
  // from the given offset until the given boundary is found, and
  // return the offset of that boundary.
  LONG FindBoundary(const base::string16& text,
                    IA2TextBoundaryType ia2_boundary,
                    LONG start_offset,
                    TextBoundaryDirection direction);

  // Many MSAA methods take a var_id parameter indicating that the operation
  // should be performed on a particular child ID, rather than this object.
  // This method tries to figure out the target object from |var_id| and
  // returns a pointer to the target object if it exists, otherwise nullptr.
  // Does not return a new reference.
  AXPlatformNodeWin* GetTargetFromChildID(const VARIANT& var_id);

  // Returns true if this node is in a treegrid.
  bool IsInTreeGrid();

  // Helper method for returning selected indicies. It is expected that the
  // caller ensures that the input has been validated.
  HRESULT AllocateComArrayFromVector(std::vector<LONG>& results,
                                     LONG max,
                                     LONG** selected,
                                     LONG* n_selected);

  // Helper method for mutating the ISelectionItemProvider selected state
  HRESULT ISelectionItemProviderSetSelected(bool selected);

  bool IsAncestorComboBox();

  // Helper method for getting the horizontal scroll percent.
  double GetHorizontalScrollPercent();

  // Helper method for getting the vertical scroll percent.
  double GetVerticalScrollPercent();

  // Helper to get the UIA FontName for this node as a BSTR.
  BSTR GetFontNameAttributeAsBSTR() const;

  // Helper to get the UIA StyleName for this node as a BSTR.
  BSTR GetStyleNameAttributeAsBSTR() const;

  // IRawElementProviderSimple support methods.

  using PatternProviderFactoryMethod = HRESULT (*)(AXPlatformNodeWin*,
                                                   IUnknown**);

  PatternProviderFactoryMethod GetPatternProviderFactoryMethod(
      PATTERNID pattern_id);

  // Start and end offsets of an active composition
  gfx::Range active_composition_range_;
};

}  // namespace ui

#endif  // UI_ACCESSIBILITY_PLATFORM_AX_PLATFORM_NODE_WIN_H_
