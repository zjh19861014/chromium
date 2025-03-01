// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef UI_ACCESSIBILITY_PLATFORM_AX_PLATFORM_NODE_DELEGATE_BASE_H_
#define UI_ACCESSIBILITY_PLATFORM_AX_PLATFORM_NODE_DELEGATE_BASE_H_

#include "ui/accessibility/platform/ax_platform_node_delegate.h"

#include <set>

namespace ui {

// Base implementation of AXPlatformNodeDelegate where all functions
// return a default value. Useful for classes that want to implement
// AXPlatformNodeDelegate but don't need to override much of its
// behavior.
class AX_EXPORT AXPlatformNodeDelegateBase : public AXPlatformNodeDelegate {
 public:
  AXPlatformNodeDelegateBase();
  ~AXPlatformNodeDelegateBase() override;

  // Get the accessibility data that should be exposed for this node.
  // Virtually all of the information is obtained from this structure
  // (role, state, name, cursor position, etc.) - the rest of this interface
  // is mostly to implement support for walking the accessibility tree.
  const AXNodeData& GetData() const override;

  // Get the accessibility tree data for this node.
  const AXTreeData& GetTreeData() const override;

  // Creates a text position rooted at this object.
  AXNodePosition::AXPositionInstance CreateTextPositionAt(
      int offset,
      ax::mojom::TextAffinity affinity =
          ax::mojom::TextAffinity::kDownstream) const override;

  // See comments in AXPlatformNodeDelegate.
  gfx::NativeViewAccessible GetNSWindow() override;

  // Get the parent of the node, which may be an AXPlatformNode or it may
  // be a native accessible object implemented by another class.
  gfx::NativeViewAccessible GetParent() override;

  // Get the index in parent. Typically this is the AXNode's index_in_parent_.
  int GetIndexInParent() const override;

  // Get the number of children of this node.
  int GetChildCount() override;

  // Get the child of a node given a 0-based index.
  gfx::NativeViewAccessible ChildAtIndex(int index) override;

  gfx::Rect GetBoundsRect(const AXCoordinateSystem coordinate_system,
                          const AXClippingBehavior clipping_behavior,
                          AXOffscreenResult* offscreen_result) const override;

  gfx::Rect GetRangeBoundsRect(
      const int start_offset,
      const int end_offset,
      const AXCoordinateSystem coordinate_system,
      const AXClippingBehavior clipping_behavior,
      AXOffscreenResult* offscreen_result) const override;

  // Derivative utils for AXPlatformNodeDelegate::GetBoundsRect
  gfx::Rect GetClippedScreenBoundsRect(
      AXOffscreenResult* offscreen_result = nullptr) const;
  gfx::Rect GetUnclippedScreenBoundsRect(
      AXOffscreenResult* offscreen_result = nullptr) const;

  // Do a *synchronous* hit test of the given location in global screen
  // coordinates, and the node within this node's subtree (inclusive) that's
  // hit, if any.
  //
  // If the result is anything other than this object or NULL, it will be
  // hit tested again recursively - that allows hit testing to work across
  // implementation classes. It's okay to take advantage of this and return
  // only an immediate child and not the deepest descendant.
  //
  // This function is mainly used by accessibility debugging software.
  // Platforms with touch accessibility use a different asynchronous interface.
  gfx::NativeViewAccessible HitTestSync(int x, int y) override;

  // Return the node within this node's subtree (inclusive) that currently
  // has focus.
  gfx::NativeViewAccessible GetFocus() override;

  // Get whether this node is offscreen.
  bool IsOffscreen() const override;

  // Get whether this node is in web content.
  bool IsWebContent() const override;

  AXPlatformNode* GetFromNodeID(int32_t id) override;

  // Given a node ID attribute (one where IsNodeIdIntAttribute is true), return
  // a target nodes for which this delegate's node has that relationship
  // attribute or NULL if there is no such relationship.
  AXPlatformNode* GetTargetNodeForRelation(
      ax::mojom::IntAttribute attr) override;

  // Given a node ID attribute (one where IsNodeIdIntListAttribute is true),
  // return a set of all target nodes for which this delegate's node has that
  // relationship attribute.
  std::set<AXPlatformNode*> GetTargetNodesForRelation(
      ax::mojom::IntListAttribute attr) override;

  // Given a node ID attribute (one where IsNodeIdIntAttribute is true), return
  // a set of all source nodes that have that relationship attribute between
  // them and this delegate's node.
  std::set<AXPlatformNode*> GetReverseRelations(
      ax::mojom::IntAttribute attr) override;

  // Given a node ID list attribute (one where IsNodeIdIntListAttribute is
  // true) return a set of all source nodes that have that relationship
  // attribute between them and this delegate's node.
  std::set<AXPlatformNode*> GetReverseRelations(
      ax::mojom::IntListAttribute attr) override;

  const AXUniqueId& GetUniqueId() const override;

  base::Optional<int> FindTextBoundary(
      TextBoundaryType boundary_type,
      int offset,
      TextBoundaryDirection direction,
      ax::mojom::TextAffinity affinity) const override;

  const std::vector<gfx::NativeViewAccessible> GetDescendants() const override;

  //
  // Tables. All of these should be called on a node that's a table-like
  // role.
  //
  bool IsTable() const override;
  int32_t GetTableColCount() const override;
  int32_t GetTableRowCount() const override;
  base::Optional<int32_t> GetTableAriaColCount() const override;
  base::Optional<int32_t> GetTableAriaRowCount() const override;
  int32_t GetTableCellCount() const override;
  const std::vector<int32_t> GetColHeaderNodeIds() const override;
  const std::vector<int32_t> GetColHeaderNodeIds(
      int32_t col_index) const override;
  const std::vector<int32_t> GetRowHeaderNodeIds() const override;
  const std::vector<int32_t> GetRowHeaderNodeIds(
      int32_t row_index) const override;
  AXPlatformNode* GetTableCaption() override;

  // Table row-like nodes.
  bool IsTableRow() const override;
  int32_t GetTableRowRowIndex() const override;

  // Table cell-like nodes.
  bool IsTableCellOrHeader() const override;
  int32_t GetTableCellIndex() const override;
  int32_t GetTableCellColIndex() const override;
  int32_t GetTableCellRowIndex() const override;
  int32_t GetTableCellColSpan() const override;
  int32_t GetTableCellRowSpan() const override;
  int32_t GetTableCellAriaColIndex() const override;
  int32_t GetTableCellAriaRowIndex() const override;
  int32_t GetCellId(int32_t row_index, int32_t col_index) const override;
  int32_t CellIndexToId(int32_t cell_index) const override;

  // Helper methods to check if a cell is an ARIA-1.1+ 'cell' or 'gridcell'
  bool IsCellOrHeaderOfARIATable() const override;
  bool IsCellOrHeaderOfARIAGrid() const override;

  // Ordered-set-like and item-like nodes.
  bool IsOrderedSetItem() const override;
  bool IsOrderedSet() const override;
  int32_t GetPosInSet() const override;
  int32_t GetSetSize() const override;

  //
  // Events.
  //

  // Return the platform-native GUI object that should be used as a target
  // for accessibility events.
  gfx::AcceleratedWidget GetTargetForNativeAccessibilityEvent() override;

  //
  // Actions.
  //

  // Perform an accessibility action, switching on the ax::mojom::Action
  // provided in |data|.
  bool AccessibilityPerformAction(const AXActionData& data) override;

  //
  // Localized strings.
  //

  base::string16 GetLocalizedStringForImageAnnotationStatus(
      ax::mojom::ImageAnnotationStatus status) const override;
  base::string16 GetLocalizedRoleDescriptionForUnlabeledImage() const override;
  base::string16 GetLocalizedStringForLandmarkType() const override;
  base::string16 GetStyleNameAttributeAsLocalizedString() const override;

  //
  // Testing.
  //

  // Accessibility objects can have the "hot tracked" state set when
  // the mouse is hovering over them, but this makes tests flaky because
  // the test behaves differently when the mouse happens to be over an
  // element. The default value should be falses if not in testing mode.
  bool ShouldIgnoreHoveredStateForTesting() override;

 protected:
  // Given a list of node ids, return the nodes in this delegate's tree to
  // which they correspond.
  std::set<ui::AXPlatformNode*> GetNodesForNodeIds(
      const std::set<int32_t>& ids);

 private:
  DISALLOW_COPY_AND_ASSIGN(AXPlatformNodeDelegateBase);
};

}  // namespace ui

#endif  // UI_ACCESSIBILITY_PLATFORM_AX_PLATFORM_NODE_DELEGATE_BASE_H_
