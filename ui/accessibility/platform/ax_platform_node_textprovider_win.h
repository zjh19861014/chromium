// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef UI_ACCESSIBILITY_PLATFORM_AX_PLATFORM_NODE_TEXTPROVIDER_WIN_H_
#define UI_ACCESSIBILITY_PLATFORM_AX_PLATFORM_NODE_TEXTPROVIDER_WIN_H_

#include "ui/accessibility/platform/ax_platform_node_win.h"

namespace ui {
class __declspec(uuid("3e1c192b-4348-45ac-8eb6-4b58eeb3dcca"))
    AXPlatformNodeTextProviderWin
    : public CComObjectRootEx<CComMultiThreadModel>,
      public ITextEditProvider {
 public:
  BEGIN_COM_MAP(AXPlatformNodeTextProviderWin)
  COM_INTERFACE_ENTRY(ITextProvider)
  COM_INTERFACE_ENTRY(ITextEditProvider)
  COM_INTERFACE_ENTRY(AXPlatformNodeTextProviderWin)
  END_COM_MAP()

  AXPlatformNodeTextProviderWin();
  ~AXPlatformNodeTextProviderWin();

  // Creates an instance of the class.
  // Returns true on success
  static HRESULT Create(ui::AXPlatformNodeWin* owner, IUnknown** provider);

  //
  // ITextProvider methods.
  //

  STDMETHOD(GetSelection)(SAFEARRAY** selection) override;

  STDMETHOD(GetVisibleRanges)(SAFEARRAY** visible_ranges) override;

  STDMETHOD(RangeFromChild)
  (IRawElementProviderSimple* child, ITextRangeProvider** range) override;

  STDMETHOD(RangeFromPoint)
  (UiaPoint point, ITextRangeProvider** range) override;

  STDMETHOD(get_DocumentRange)(ITextRangeProvider** range) override;

  STDMETHOD(get_SupportedTextSelection)
  (enum SupportedTextSelection* text_selection) override;

  //
  // ITextEditProvider methods.
  //

  STDMETHOD(GetActiveComposition)(ITextRangeProvider** range) override;

  STDMETHOD(GetConversionTarget)(ITextRangeProvider** range) override;

  // ITextProvider supporting methods.

  static ITextRangeProvider* GetRangeFromChild(
      ui::AXPlatformNodeWin* ancestor,
      ui::AXPlatformNodeWin* descendant);

 private:
  friend class AXPlatformNodeTextProviderTest;
  ui::AXPlatformNodeWin* owner() const;
  HRESULT GetTextRangeProviderFromActiveComposition(ITextRangeProvider** range);

  CComPtr<ui::AXPlatformNodeWin> owner_;
};

}  // namespace ui

#endif  // UI_ACCESSIBILITY_PLATFORM_AX_PLATFORM_NODE_TEXTPROVIDER_WIN_H_
