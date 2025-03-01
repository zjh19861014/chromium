// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "base/strings/utf_string_conversions.h"
#include "chrome/browser/ui/views/payments/payment_request_browsertest_base.h"
#include "chrome/browser/ui/views/payments/payment_request_dialog_view_ids.h"
#include "components/autofill/core/browser/autofill_profile.h"
#include "components/autofill/core/browser/autofill_test_utils.h"
#include "components/web_modal/web_contents_modal_dialog_manager.h"
#include "content/public/test/browser_test_utils.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "ui/views/controls/label.h"
#include "ui/views/view.h"

namespace payments {
namespace {

class PaymentRequestShowPromiseTest : public PaymentRequestBrowserTestBase {
 protected:
  PaymentRequestShowPromiseTest() {}
  ~PaymentRequestShowPromiseTest() override {}

  // Installs the payment handler for "basic-card" that responds to
  // "paymentrequest" events by echoing back the "total" object.
  void InstallEchoPaymentHandlerForBasicCard() {
    std::string contents;
    ASSERT_TRUE(content::ExecuteScriptAndExtractString(
        GetActiveWebContents(), "install();", &contents));
    ASSERT_EQ(contents, "instruments.set(): Payment handler installed.");
  }

  // Allows to skip UI into payment handler for "basic-card".
  void EnalbeSkipUIForForBasicCard() {
    std::vector<PaymentRequest*> requests =
        GetPaymentRequests(GetActiveWebContents());
    ASSERT_EQ(1U, requests.size());
    requests.front()
        ->set_skip_ui_for_non_url_payment_method_identifiers_for_test();
  }

  // Shows the browser payment sheet.
  void ShowBrowserPaymentSheet() {
    ResetEventWaiterForSequence({DialogEvent::PROCESSING_SPINNER_SHOWN,
                                 DialogEvent::PROCESSING_SPINNER_HIDDEN,
                                 DialogEvent::SPEC_DONE_UPDATING,
                                 DialogEvent::PROCESSING_SPINNER_HIDDEN,
                                 DialogEvent::DIALOG_OPENED});
    ASSERT_TRUE(content::ExecuteScript(GetActiveWebContents(), "buy();"));
    WaitForObservedEvent();
    EXPECT_TRUE(web_modal::WebContentsModalDialogManager::FromWebContents(
                    GetActiveWebContents())
                    ->IsDialogActive());
  }

  // Verifies that the payment sheet total is |total_amount_string|.
  void ExpectTotal(const std::string& total_amount_string) {
    EXPECT_EQ(base::ASCIIToUTF16(total_amount_string),
              GetLabelText(DialogViewID::ORDER_SUMMARY_TOTAL_AMOUNT_LABEL));
  }

  // Verifies that the shipping address section does not display any warning
  // messages.
  void ExpectNoShippingWarningMessage() {
    views::View* view = dialog_view()->GetViewByID(
        static_cast<int>(DialogViewID::WARNING_LABEL));
    if (!view || !view->visible())
      return;

    EXPECT_EQ(base::string16(), static_cast<views::Label*>(view)->text());
  }

  // Verifies that the shipping address section has |expected_message| in the
  // header.
  void ExpectShippingWarningMessage(const std::string& expected_message) {
    EXPECT_EQ(base::ASCIIToUTF16(expected_message),
              GetLabelText(DialogViewID::WARNING_LABEL));
  }

  // Selects another shipping address.
  void SelectAnotherShippingAddress() {
    ResetEventWaiterForSequence({DialogEvent::PROCESSING_SPINNER_SHOWN,
                                 DialogEvent::PROCESSING_SPINNER_HIDDEN,
                                 DialogEvent::SPEC_DONE_UPDATING});
    ClickOnChildInListViewAndWait(
        /*child_index=*/1, /*total_num_children=*/2,
        DialogViewID::SHIPPING_ADDRESS_SHEET_LIST_VIEW);
  }

  // Selects the only shipping address.
  void SelectTheOnlyShippingAddress() {
    ResetEventWaiterForSequence({DialogEvent::PROCESSING_SPINNER_SHOWN,
                                 DialogEvent::PROCESSING_SPINNER_HIDDEN,
                                 DialogEvent::SPEC_DONE_UPDATING});
    ClickOnChildInListViewAndWait(
        /*child_index=*/0, /*total_num_children=*/1,
        DialogViewID::SHIPPING_ADDRESS_SHEET_LIST_VIEW);
  }

  // Verifies that the first shipping option cost is |amount_string|.
  void ExpectShippingCost(const std::string& amount_string) {
    EXPECT_EQ(base::ASCIIToUTF16(amount_string),
              GetLabelText(DialogViewID::SHIPPING_OPTION_AMOUNT));
  }

  // Clicks the "Pay" button and waits for the dialog to close.
  void Pay() {
    ResetEventWaiterForSequence(
        {DialogEvent::PROCESSING_SPINNER_SHOWN, DialogEvent::DIALOG_CLOSED});
    ClickOnDialogViewAndWait(DialogViewID::PAY_BUTTON, dialog_view());
  }

 private:
  DISALLOW_COPY_AND_ASSIGN(PaymentRequestShowPromiseTest);
};

IN_PROC_BROWSER_TEST_F(PaymentRequestShowPromiseTest, DigitalGoods) {
  NavigateTo("/show_promise/digital_goods.html");
  InstallEchoPaymentHandlerForBasicCard();
  ASSERT_TRUE(content::ExecuteScript(GetActiveWebContents(), "create();"));
  ShowBrowserPaymentSheet();

  EXPECT_TRUE(IsPayButtonEnabled());

  OpenOrderSummaryScreen();

  ExpectTotal("$1.00");

  ClickOnBackArrow();
  Pay();

  ExpectBodyContains({R"({"currency":"USD","value":"1.00"})"});
}

IN_PROC_BROWSER_TEST_F(PaymentRequestShowPromiseTest, SingleOptionShipping) {
  NavigateTo("/show_promise/single_option_shipping.html");
  InstallEchoPaymentHandlerForBasicCard();
  AddAutofillProfile(autofill::test::GetFullProfile());
  AddAutofillProfile(autofill::test::GetFullProfile2());
  ShowBrowserPaymentSheet();

  EXPECT_TRUE(IsPayButtonEnabled());

  OpenOrderSummaryScreen();

  ExpectTotal("$1.00");

  ClickOnBackArrow();
  OpenShippingAddressSectionScreen();

  ExpectNoShippingWarningMessage();

  SelectAnotherShippingAddress();

  ExpectNoShippingWarningMessage();

  ClickOnBackArrow();
  OpenShippingOptionSectionScreen();

  ExpectShippingCost("$0.00");

  ClickOnBackArrow();

  EXPECT_TRUE(IsPayButtonEnabled());

  Pay();

  ExpectBodyContains({R"({"currency":"USD","value":"1.00"})"});
}

IN_PROC_BROWSER_TEST_F(PaymentRequestShowPromiseTest,
                       SingleOptionShippingWithUpdate) {
  NavigateTo("/show_promise/single_option_shipping_with_update.html");
  InstallEchoPaymentHandlerForBasicCard();
  AddAutofillProfile(autofill::test::GetFullProfile());
  AddAutofillProfile(autofill::test::GetFullProfile2());
  ShowBrowserPaymentSheet();

  EXPECT_TRUE(IsPayButtonEnabled());

  OpenOrderSummaryScreen();

  ExpectTotal("$1.00");

  ClickOnBackArrow();
  OpenShippingAddressSectionScreen();

  ExpectNoShippingWarningMessage();

  SelectAnotherShippingAddress();

  ExpectNoShippingWarningMessage();

  ClickOnBackArrow();
  OpenShippingOptionSectionScreen();

  ExpectShippingCost("$0.00");

  ClickOnBackArrow();

  EXPECT_TRUE(IsPayButtonEnabled());

  Pay();

  ExpectBodyContains({R"({"currency":"USD","value":"1.00"})"});
}

IN_PROC_BROWSER_TEST_F(PaymentRequestShowPromiseTest, CannotShipError) {
  NavigateTo("/show_promise/us_only_shipping.html");
  InstallEchoPaymentHandlerForBasicCard();
  AddAutofillProfile(autofill::test::GetFullCanadianProfile());
  ShowBrowserPaymentSheet();

  EXPECT_FALSE(IsPayButtonEnabled());

  OpenOrderSummaryScreen();

  ExpectTotal("$1.00");

  ClickOnBackArrow();
  OpenShippingAddressSectionScreen();

  ExpectShippingWarningMessage(
      "To see shipping methods and requirements, select an address");

  SelectTheOnlyShippingAddress();

  ExpectShippingWarningMessage("Cannot ship outside of US.");

  ClickOnBackArrow();

  EXPECT_FALSE(IsPayButtonEnabled());

  ClickOnCancel();
}

IN_PROC_BROWSER_TEST_F(PaymentRequestShowPromiseTest, SkipUI) {
  NavigateTo("/show_promise/digital_goods.html");
  InstallEchoPaymentHandlerForBasicCard();
  ASSERT_TRUE(content::ExecuteScript(GetActiveWebContents(), "create();"));
  EnalbeSkipUIForForBasicCard();
  ResetEventWaiterForSequence(
      {DialogEvent::PROCESSING_SPINNER_SHOWN,
       DialogEvent::PROCESSING_SPINNER_HIDDEN, DialogEvent::SPEC_DONE_UPDATING,
       DialogEvent::PROCESSING_SPINNER_HIDDEN, DialogEvent::DIALOG_OPENED,
       DialogEvent::PROCESSING_SPINNER_SHOWN, DialogEvent::DIALOG_CLOSED});
  ASSERT_TRUE(content::ExecuteScript(GetActiveWebContents(), "buy();"));
  WaitForObservedEvent();

  ExpectBodyContains({R"({"currency":"USD","value":"1.00"})"});
}

IN_PROC_BROWSER_TEST_F(PaymentRequestShowPromiseTest, Reject) {
  NavigateTo("/show_promise/reject.html");
  ResetEventWaiterForSequence(
      {DialogEvent::PROCESSING_SPINNER_SHOWN, DialogEvent::DIALOG_CLOSED});
  ASSERT_TRUE(content::ExecuteScript(GetActiveWebContents(), "buy();"));
  WaitForObservedEvent();

  ExpectBodyContains({R"(AbortError)"});
}

IN_PROC_BROWSER_TEST_F(PaymentRequestShowPromiseTest, Timeout) {
  NavigateTo("/show_promise/timeout.html");
  ResetEventWaiterForSequence(
      {DialogEvent::PROCESSING_SPINNER_SHOWN, DialogEvent::DIALOG_CLOSED});
  ASSERT_TRUE(content::ExecuteScript(GetActiveWebContents(), "buy();"));
  WaitForObservedEvent();

  ExpectBodyContains({R"(AbortError)"});
}

IN_PROC_BROWSER_TEST_F(PaymentRequestShowPromiseTest,
                       UnsupportedPaymentMethod) {
  NavigateTo("/show_promise/unsupported.html");
  ResetEventWaiterForSequence(
      {DialogEvent::PROCESSING_SPINNER_SHOWN,
       DialogEvent::PROCESSING_SPINNER_HIDDEN, DialogEvent::SPEC_DONE_UPDATING,
       DialogEvent::PROCESSING_SPINNER_HIDDEN, DialogEvent::NOT_SUPPORTED_ERROR,
       DialogEvent::DIALOG_CLOSED});
  ASSERT_TRUE(content::ExecuteScript(GetActiveWebContents(), "buy();"));
  WaitForObservedEvent();

  ExpectBodyContains(
      {R"(NotSupportedError: The payment method "foo" is not supported)"});
}

IN_PROC_BROWSER_TEST_F(PaymentRequestShowPromiseTest, InvalidDetails) {
  NavigateTo("/show_promise/invalid_details.html");
  ResetEventWaiterForSequence(
      {DialogEvent::PROCESSING_SPINNER_SHOWN, DialogEvent::DIALOG_CLOSED});
  ASSERT_TRUE(content::ExecuteScript(GetActiveWebContents(), "buy();"));
  WaitForObservedEvent();

  ExpectBodyContains({R"(Total amount value should be non-negative)"});
}

IN_PROC_BROWSER_TEST_F(PaymentRequestShowPromiseTest,
                       OptionalTotalWhenResolvingShowPromise) {
  NavigateTo("/show_promise/resolve_with_empty_dictionary.html");
  InstallEchoPaymentHandlerForBasicCard();
  ShowBrowserPaymentSheet();

  EXPECT_TRUE(IsPayButtonEnabled());

  OpenOrderSummaryScreen();

  ExpectTotal("$1.00");

  ClickOnBackArrow();
  Pay();

  ExpectBodyContains({R"({"currency":"USD","value":"1.00"})"});
}

}  // namespace
}  // namespace payments
