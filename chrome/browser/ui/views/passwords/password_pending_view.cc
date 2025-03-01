// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/ui/views/passwords/password_pending_view.h"

#include <algorithm>
#include <memory>
#include <utility>
#include <vector>

#include "base/strings/utf_string_conversions.h"
#include "build/build_config.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/passwords/manage_passwords_view_utils.h"
#include "chrome/browser/ui/passwords/password_dialog_prompts.h"
#include "chrome/browser/ui/views/chrome_layout_provider.h"
#include "chrome/browser/ui/views/chrome_typography.h"
#include "chrome/browser/ui/views/passwords/credentials_item_view.h"
#include "chrome/browser/ui/views/passwords/password_items_view.h"
#include "chrome/browser/ui/views/passwords/password_sign_in_promo_view.h"
#include "chrome/grit/generated_resources.h"
#include "chrome/grit/theme_resources.h"
#include "content/public/browser/storage_partition.h"
#include "ui/base/l10n/l10n_util.h"
#include "ui/base/models/combobox_model.h"
#include "ui/base/models/combobox_model_observer.h"
#include "ui/base/models/simple_combobox_model.h"
#include "ui/base/resource/resource_bundle.h"
#include "ui/views/bubble/bubble_frame_view.h"
#include "ui/views/controls/button/image_button.h"
#include "ui/views/controls/button/md_text_button.h"
#include "ui/views/controls/editable_combobox/editable_combobox.h"
#include "ui/views/controls/textfield/textfield.h"
#include "ui/views/layout/fill_layout.h"
#include "ui/views/layout/grid_layout.h"
#include "ui/views/layout/layout_provider.h"
#include "ui/views/window/dialog_client_view.h"

namespace {

enum PasswordPendingViewColumnSetType {
  // | | (LEADING, FILL) | | (FILL, FILL) | |
  // Used for the username/password line of the bubble, for the pending view.
  DOUBLE_VIEW_COLUMN_SET_USERNAME,
  DOUBLE_VIEW_COLUMN_SET_PASSWORD,

  // | | (LEADING, FILL) | | (FILL, FILL) | | (TRAILING, FILL) | |
  // Used for the password line of the bubble, for the pending view.
  // Views are label, password and the eye icon.
  TRIPLE_VIEW_COLUMN_SET,
};

// Construct an appropriate ColumnSet for the given |type|, and add it
// to |layout|.
void BuildColumnSet(views::GridLayout* layout,
                    PasswordPendingViewColumnSetType type) {
  views::ColumnSet* column_set = layout->AddColumnSet(type);
  const int column_divider = ChromeLayoutProvider::Get()->GetDistanceMetric(
      views::DISTANCE_RELATED_CONTROL_HORIZONTAL);
  switch (type) {
    case DOUBLE_VIEW_COLUMN_SET_USERNAME:
    case DOUBLE_VIEW_COLUMN_SET_PASSWORD:
      column_set->AddColumn(views::GridLayout::LEADING, views::GridLayout::FILL,
                            views::GridLayout::kFixedSize,
                            views::GridLayout::USE_PREF, 0, 0);
      column_set->AddPaddingColumn(views::GridLayout::kFixedSize,
                                   column_divider);
      column_set->AddColumn(views::GridLayout::FILL, views::GridLayout::FILL,
                            1.0, views::GridLayout::USE_PREF, 0, 0);
      break;
    case TRIPLE_VIEW_COLUMN_SET:
      column_set->AddColumn(views::GridLayout::LEADING, views::GridLayout::FILL,
                            views::GridLayout::kFixedSize,
                            views::GridLayout::USE_PREF, 0, 0);
      column_set->AddPaddingColumn(views::GridLayout::kFixedSize,
                                   column_divider);
      column_set->AddColumn(views::GridLayout::FILL, views::GridLayout::FILL,
                            1.0, views::GridLayout::USE_PREF, 0, 0);
      column_set->AddPaddingColumn(views::GridLayout::kFixedSize,
                                   column_divider);
      column_set->AddColumn(
          views::GridLayout::TRAILING, views::GridLayout::FILL,
          views::GridLayout::kFixedSize, views::GridLayout::USE_PREF, 0, 0);
      break;
  }
}

// Builds a credential row, adds the given elements to the layout.
// |password_view_button| is an optional field. If it is a nullptr, a
// DOUBLE_VIEW_COLUMN_SET_PASSWORD will be used for password row instead of
// TRIPLE_VIEW_COLUMN_SET.
void BuildCredentialRows(views::GridLayout* layout,
                         views::View* username_field,
                         views::View* password_field,
                         views::ToggleImageButton* password_view_button) {
  // Username row.
  BuildColumnSet(layout, DOUBLE_VIEW_COLUMN_SET_USERNAME);
  layout->StartRow(views::GridLayout::kFixedSize,
                   DOUBLE_VIEW_COLUMN_SET_USERNAME);
  std::unique_ptr<views::Label> username_label(new views::Label(
      l10n_util::GetStringUTF16(IDS_PASSWORD_MANAGER_USERNAME_LABEL),
      views::style::CONTEXT_LABEL, views::style::STYLE_PRIMARY));
  username_label->SetHorizontalAlignment(gfx::HorizontalAlignment::ALIGN_LEFT);
  std::unique_ptr<views::Label> password_label(new views::Label(
      l10n_util::GetStringUTF16(IDS_PASSWORD_MANAGER_PASSWORD_LABEL),
      views::style::CONTEXT_LABEL, views::style::STYLE_PRIMARY));
  password_label->SetHorizontalAlignment(gfx::HorizontalAlignment::ALIGN_LEFT);
  int labels_width = std::max(username_label->GetPreferredSize().width(),
                              password_label->GetPreferredSize().width());
  int fields_height = std::max(username_field->GetPreferredSize().height(),
                               password_field->GetPreferredSize().height());

  layout->AddView(username_label.release(), 1, 1, views::GridLayout::LEADING,
                  views::GridLayout::FILL, labels_width, 0);
  layout->AddView(username_field, 1, 1, views::GridLayout::FILL,
                  views::GridLayout::FILL, 0, fields_height);

  layout->AddPaddingRow(views::GridLayout::kFixedSize,
                        ChromeLayoutProvider::Get()->GetDistanceMetric(
                            DISTANCE_CONTROL_LIST_VERTICAL));

  // Password row.
  PasswordPendingViewColumnSetType type = password_view_button
                                              ? TRIPLE_VIEW_COLUMN_SET
                                              : DOUBLE_VIEW_COLUMN_SET_PASSWORD;
  BuildColumnSet(layout, type);
  layout->StartRow(views::GridLayout::kFixedSize, type);
  layout->AddView(password_label.release(), 1, 1, views::GridLayout::LEADING,
                  views::GridLayout::FILL, labels_width, 0);
  layout->AddView(password_field, 1, 1, views::GridLayout::FILL,
                  views::GridLayout::FILL, 0, fields_height);
  // The eye icon is also added to the layout if it was passed.
  if (password_view_button) {
    layout->AddView(password_view_button);
  }
}

// Create a vector which contains only the values in |items| and no elements.
std::vector<base::string16> ToValues(
    const autofill::ValueElementVector& items) {
  std::vector<base::string16> passwords;
  passwords.reserve(items.size());
  for (auto& pair : items)
    passwords.push_back(pair.first);
  return passwords;
}

std::unique_ptr<views::ToggleImageButton> CreatePasswordViewButton(
    views::ButtonListener* listener,
    bool are_passwords_revealed) {
  auto button = std::make_unique<views::ToggleImageButton>(listener);
  button->SetFocusForPlatform();
  button->SetInstallFocusRingOnFocus(true);
  button->set_request_focus_on_press(true);
  button->SetTooltipText(
      l10n_util::GetStringUTF16(IDS_MANAGE_PASSWORDS_SHOW_PASSWORD));
  button->SetToggledTooltipText(
      l10n_util::GetStringUTF16(IDS_MANAGE_PASSWORDS_HIDE_PASSWORD));
  button->SetImage(views::ImageButton::STATE_NORMAL,
                   *ui::ResourceBundle::GetSharedInstance().GetImageSkiaNamed(
                       IDR_SHOW_PASSWORD_HOVER));
  button->SetToggledImage(
      views::ImageButton::STATE_NORMAL,
      ui::ResourceBundle::GetSharedInstance().GetImageSkiaNamed(
          IDR_HIDE_PASSWORD_HOVER));
  button->SetImageAlignment(views::ImageButton::ALIGN_CENTER,
                            views::ImageButton::ALIGN_MIDDLE);
  button->SetToggled(are_passwords_revealed);
  return button;
}

// Creates a dropdown from |PasswordForm.all_possible_passwords|.
std::unique_ptr<views::EditableCombobox> CreatePasswordDropdownView(
    const autofill::PasswordForm& form,
    bool are_passwords_revealed) {
  DCHECK(form.federation_origin.opaque());
  const std::vector<base::string16> passwords =
      form.all_possible_passwords.empty()
          ? std::vector<base::string16>(/*n=*/1, form.password_value)
          : ToValues(form.all_possible_passwords);
  auto combobox = std::make_unique<views::EditableCombobox>(
      std::make_unique<ui::SimpleComboboxModel>(passwords),
      /*filter_on_edit=*/false, /*show_on_empty=*/true,
      views::EditableCombobox::Type::kPassword, views::style::CONTEXT_BUTTON,
      STYLE_PRIMARY_MONOSPACED);
  combobox->SetText(form.password_value);
  combobox->RevealPasswords(are_passwords_revealed);
  combobox->SetAccessibleName(
      l10n_util::GetStringUTF16(IDS_PASSWORD_MANAGER_PASSWORD_LABEL));
  return combobox;
}
}  // namespace

PasswordPendingView::PasswordPendingView(content::WebContents* web_contents,
                                         views::View* anchor_view,
                                         const gfx::Point& anchor_point,
                                         DisplayReason reason)
    : PasswordBubbleViewBase(web_contents, anchor_view, anchor_point, reason),
      is_update_bubble_(model()->state() ==
                        password_manager::ui::PENDING_PASSWORD_UPDATE_STATE),
      sign_in_promo_(nullptr),
      username_field_(nullptr),
      password_view_button_(nullptr),
      initially_focused_view_(nullptr),
      password_dropdown_(nullptr),
      are_passwords_revealed_(
          model()->are_passwords_revealed_when_bubble_is_opened()) {
  DCHECK(model()->state() == password_manager::ui::PENDING_PASSWORD_STATE ||
         model()->state() ==
             password_manager::ui::PENDING_PASSWORD_UPDATE_STATE);
  const autofill::PasswordForm& password_form = model()->pending_password();
  if (!password_form.federation_origin.opaque()) {
    // The credential to be saved doesn't contain password but just the identity
    // provider (e.g. "Sign in with Google"). Thus, the layout is different.
    SetLayoutManager(std::make_unique<views::FillLayout>());
    std::pair<base::string16, base::string16> titles =
        GetCredentialLabelsForAccountChooser(password_form);
    CredentialsItemView* credential_view = new CredentialsItemView(
        this, titles.first, titles.second, kButtonHoverColor, &password_form,
        content::BrowserContext::GetDefaultStoragePartition(
            model()->GetProfile())
            ->GetURLLoaderFactoryForBrowserProcess()
            .get());
    credential_view->SetEnabled(false);
    AddChildView(credential_view);
  } else {
    if (model()->enable_editing()) {
      views::Textfield* username_field =
          CreateUsernameEditable(model()->GetCurrentUsername()).release();
      username_field->set_controller(this);
      username_field_ = username_field;
    } else {
      username_field_ = CreateUsernameLabel(password_form).release();
    }

    password_dropdown_ =
        CreatePasswordDropdownView(password_form, are_passwords_revealed_)
            .release();

    password_view_button_ =
        CreatePasswordViewButton(this, are_passwords_revealed_).release();

    views::GridLayout* layout =
        SetLayoutManager(std::make_unique<views::GridLayout>(this));

    BuildCredentialRows(layout, username_field_, password_dropdown_,
                        password_view_button_);
    if (model()->enable_editing() &&
        model()->pending_password().username_value.empty()) {
      initially_focused_view_ = username_field_;
    }
  }
}

PasswordPendingView::~PasswordPendingView() = default;

bool PasswordPendingView::Accept() {
  if (sign_in_promo_)
    return sign_in_promo_->Accept();
  UpdateUsernameAndPasswordInModel();
  model()->OnSaveClicked();
  if (model()->ReplaceToShowPromotionIfNeeded()) {
    ReplaceWithPromo();
    return false;  // Keep open.
  }
  return true;
}

bool PasswordPendingView::Cancel() {
  if (sign_in_promo_)
    return sign_in_promo_->Cancel();
  UpdateUsernameAndPasswordInModel();
  if (is_update_bubble_) {
    model()->OnNopeUpdateClicked();
    return true;
  }
  model()->OnNeverForThisSiteClicked();
  return true;
}

bool PasswordPendingView::Close() {
  return true;
}

void PasswordPendingView::ButtonPressed(views::Button* sender,
                                        const ui::Event& event) {
  DCHECK(sender == password_view_button_);
  TogglePasswordVisibility();
}

void PasswordPendingView::ContentsChanged(views::Textfield* sender,
                                          const base::string16& new_contents) {
  bool is_update_before = model()->IsCurrentStateUpdate();
  UpdateUsernameAndPasswordInModel();
  // May be the buttons should be updated.
  if (is_update_before != model()->IsCurrentStateUpdate()) {
    DialogModelChanged();
    GetDialogClientView()->Layout();
  }
}

views::View* PasswordPendingView::CreateFootnoteView() {
  if (sign_in_promo_ || !model()->ShouldShowFooter())
    return nullptr;
  views::Label* label = new views::Label(
      l10n_util::GetStringUTF16(IDS_SAVE_PASSWORD_FOOTER),
      ChromeTextContext::CONTEXT_BODY_TEXT_SMALL, STYLE_SECONDARY);
  label->SetMultiLine(true);
  label->SetHorizontalAlignment(gfx::ALIGN_LEFT);
  return label;
}

gfx::Size PasswordPendingView::CalculatePreferredSize() const {
  const int width = ChromeLayoutProvider::Get()->GetDistanceMetric(
                        DISTANCE_BUBBLE_PREFERRED_WIDTH) -
                    margins().width();
  return gfx::Size(width, GetHeightForWidth(width));
}

views::View* PasswordPendingView::GetInitiallyFocusedView() {
  if (initially_focused_view_)
    return initially_focused_view_;
  return PasswordBubbleViewBase::GetInitiallyFocusedView();
}

int PasswordPendingView::GetDialogButtons() const {
  if (sign_in_promo_)
    return sign_in_promo_->GetDialogButtons();

  return PasswordBubbleViewBase::GetDialogButtons();
}

base::string16 PasswordPendingView::GetDialogButtonLabel(
    ui::DialogButton button) const {
  // TODO(pbos): Generalize the different promotion classes to not store and
  // ask each different possible promo.
  if (sign_in_promo_)
    return sign_in_promo_->GetDialogButtonLabel(button);

  int message = 0;
  if (button == ui::DIALOG_BUTTON_OK) {
    message = model()->IsCurrentStateUpdate()
                  ? IDS_PASSWORD_MANAGER_UPDATE_BUTTON
                  : IDS_PASSWORD_MANAGER_SAVE_BUTTON;
  } else {
    message = is_update_bubble_ ? IDS_PASSWORD_MANAGER_CANCEL_BUTTON
                                : IDS_PASSWORD_MANAGER_BUBBLE_BLACKLIST_BUTTON;
  }

  return l10n_util::GetStringUTF16(message);
}

gfx::ImageSkia PasswordPendingView::GetWindowIcon() {
  return gfx::ImageSkia();
}

bool PasswordPendingView::ShouldShowWindowIcon() const {
  return false;
}

bool PasswordPendingView::ShouldShowCloseButton() const {
  return true;
}

void PasswordPendingView::AddedToWidget() {
  static_cast<views::Label*>(GetBubbleFrameView()->title())
      ->SetAllowCharacterBreak(true);
}

void PasswordPendingView::TogglePasswordVisibility() {
  if (!are_passwords_revealed_ && !model()->RevealPasswords())
    return;

  are_passwords_revealed_ = !are_passwords_revealed_;
  password_view_button_->SetToggled(are_passwords_revealed_);
  DCHECK(password_dropdown_);
  password_dropdown_->RevealPasswords(are_passwords_revealed_);
}

void PasswordPendingView::UpdateUsernameAndPasswordInModel() {
  const bool username_editable = model()->enable_editing();
  if (!username_editable && !password_dropdown_)
    return;

  base::string16 new_username = model()->pending_password().username_value;
  base::string16 new_password = model()->pending_password().password_value;
  if (username_editable) {
    new_username = static_cast<views::Textfield*>(username_field_)->text();
    base::TrimString(new_username, base::ASCIIToUTF16(" "), &new_username);
  }
  if (password_dropdown_)
    new_password = password_dropdown_->GetText();
  model()->OnCredentialEdited(std::move(new_username), std::move(new_password));
}

void PasswordPendingView::ReplaceWithPromo() {
  RemoveAllChildViews(true);
  initially_focused_view_ = nullptr;
  SetLayoutManager(std::make_unique<views::FillLayout>());
  set_margins(ChromeLayoutProvider::Get()->GetDialogInsetsForContentType(
      views::TEXT, views::TEXT));
  if (model()->state() == password_manager::ui::CHROME_SIGN_IN_PROMO_STATE) {
    sign_in_promo_ = new PasswordSignInPromoView(model());
    AddChildView(sign_in_promo_);
  } else {
    NOTREACHED();
  }
  GetWidget()->UpdateWindowIcon();
  GetWidget()->UpdateWindowTitle();
  DialogModelChanged();

  SizeToContents();
}
