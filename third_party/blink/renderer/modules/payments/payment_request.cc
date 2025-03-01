// Copyright 2016 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "third_party/blink/renderer/modules/payments/payment_request.h"

#include <stddef.h>
#include <utility>

#include "base/location.h"
#include "base/stl_util.h"
#include "mojo/public/cpp/bindings/interface_request.h"
#include "services/service_manager/public/cpp/interface_provider.h"
#include "third_party/blink/public/mojom/devtools/console_message.mojom-shared.h"
#include "third_party/blink/public/mojom/feature_policy/feature_policy.mojom-blink.h"
#include "third_party/blink/public/mojom/web_feature/web_feature.mojom-blink.h"
#include "third_party/blink/public/platform/platform.h"
#include "third_party/blink/public/platform/task_type.h"
#include "third_party/blink/renderer/bindings/core/v8/script_promise_resolver.h"
#include "third_party/blink/renderer/bindings/core/v8/v8_string_resource.h"
#include "third_party/blink/renderer/bindings/modules/v8/v8_android_pay_method_data.h"
#include "third_party/blink/renderer/bindings/modules/v8/v8_basic_card_request.h"
#include "third_party/blink/renderer/bindings/modules/v8/v8_payment_details_update.h"
#include "third_party/blink/renderer/core/dom/document.h"
#include "third_party/blink/renderer/core/dom/dom_exception.h"
#include "third_party/blink/renderer/core/dom/events/event.h"
#include "third_party/blink/renderer/core/dom/events/event_queue.h"
#include "third_party/blink/renderer/core/event_type_names.h"
#include "third_party/blink/renderer/core/frame/deprecation.h"
#include "third_party/blink/renderer/core/frame/frame_owner.h"
#include "third_party/blink/renderer/core/frame/local_dom_window.h"
#include "third_party/blink/renderer/core/frame/local_frame.h"
#include "third_party/blink/renderer/core/frame/settings.h"
#include "third_party/blink/renderer/core/frame/use_counter.h"
#include "third_party/blink/renderer/core/frame/web_feature.h"
#include "third_party/blink/renderer/core/html/html_iframe_element.h"
#include "third_party/blink/renderer/core/inspector/console_message.h"
#include "third_party/blink/renderer/modules/event_target_modules_names.h"
#include "third_party/blink/renderer/modules/payments/address_errors.h"
#include "third_party/blink/renderer/modules/payments/android_pay_method_data.h"
#include "third_party/blink/renderer/modules/payments/android_pay_tokenization.h"
#include "third_party/blink/renderer/modules/payments/basic_card_helper.h"
#include "third_party/blink/renderer/modules/payments/basic_card_request.h"
#include "third_party/blink/renderer/modules/payments/html_iframe_element_payments.h"
#include "third_party/blink/renderer/modules/payments/payer_errors.h"
#include "third_party/blink/renderer/modules/payments/payment_address.h"
#include "third_party/blink/renderer/modules/payments/payment_details_init.h"
#include "third_party/blink/renderer/modules/payments/payment_details_update.h"
#include "third_party/blink/renderer/modules/payments/payment_item.h"
#include "third_party/blink/renderer/modules/payments/payment_method_change_event.h"
#include "third_party/blink/renderer/modules/payments/payment_request_update_event.h"
#include "third_party/blink/renderer/modules/payments/payment_response.h"
#include "third_party/blink/renderer/modules/payments/payment_shipping_option.h"
#include "third_party/blink/renderer/modules/payments/payment_validation_errors.h"
#include "third_party/blink/renderer/modules/payments/payments_validators.h"
#include "third_party/blink/renderer/modules/payments/update_payment_details_function.h"
#include "third_party/blink/renderer/platform/bindings/exception_state.h"
#include "third_party/blink/renderer/platform/bindings/script_state.h"
#include "third_party/blink/renderer/platform/mojo/mojo_helper.h"
#include "third_party/blink/renderer/platform/runtime_enabled_features.h"
#include "third_party/blink/renderer/platform/uuid.h"
#include "third_party/blink/renderer/platform/wtf/functional.h"
#include "third_party/blink/renderer/platform/wtf/hash_set.h"
#include "third_party/blink/renderer/platform/wtf/text/string_builder.h"

namespace {

using ::payments::mojom::blink::AddressErrors;
using ::payments::mojom::blink::AddressErrorsPtr;
using ::payments::mojom::blink::CanMakePaymentQueryResult;
using ::payments::mojom::blink::HasEnrolledInstrumentQueryResult;
using ::payments::mojom::blink::PayerErrors;
using ::payments::mojom::blink::PayerErrorsPtr;
using ::payments::mojom::blink::PaymentAddress;
using ::payments::mojom::blink::PaymentAddressPtr;
using ::payments::mojom::blink::PaymentCurrencyAmount;
using ::payments::mojom::blink::PaymentCurrencyAmountPtr;
using ::payments::mojom::blink::PaymentDetailsModifierPtr;
using ::payments::mojom::blink::PaymentDetailsPtr;
using ::payments::mojom::blink::PaymentErrorReason;
using ::payments::mojom::blink::PaymentItemPtr;
using ::payments::mojom::blink::PaymentMethodDataPtr;
using ::payments::mojom::blink::PaymentOptionsPtr;
using ::payments::mojom::blink::PaymentResponsePtr;
using ::payments::mojom::blink::PaymentShippingOptionPtr;
using ::payments::mojom::blink::PaymentShippingType;
using ::payments::mojom::blink::PaymentValidationErrors;
using ::payments::mojom::blink::PaymentValidationErrorsPtr;

const char kCanMakePaymentDebugName[] = "canMakePayment";
const char kHasEnrolledInstrumentDebugName[] = "hasEnrolledInstrument";

}  // namespace

namespace mojo {

template <>
struct TypeConverter<PaymentCurrencyAmountPtr, blink::PaymentCurrencyAmount*> {
  static PaymentCurrencyAmountPtr Convert(
      const blink::PaymentCurrencyAmount* input) {
    PaymentCurrencyAmountPtr output = PaymentCurrencyAmount::New();
    output->currency = input->currency().UpperASCII();
    output->value = input->value();
    return output;
  }
};

template <>
struct TypeConverter<PaymentItemPtr, blink::PaymentItem*> {
  static PaymentItemPtr Convert(const blink::PaymentItem* input) {
    PaymentItemPtr output = payments::mojom::blink::PaymentItem::New();
    output->label = input->label();
    output->amount = PaymentCurrencyAmount::From(input->amount());
    output->pending = input->pending();
    return output;
  }
};

template <>
struct TypeConverter<PaymentShippingOptionPtr, blink::PaymentShippingOption*> {
  static PaymentShippingOptionPtr Convert(
      const blink::PaymentShippingOption* input) {
    PaymentShippingOptionPtr output =
        payments::mojom::blink::PaymentShippingOption::New();
    output->id = input->id();
    output->label = input->label();
    output->amount = PaymentCurrencyAmount::From(input->amount());
    output->selected = input->hasSelected() && input->selected();
    return output;
  }
};

template <>
struct TypeConverter<PaymentOptionsPtr, blink::PaymentOptions*> {
  static PaymentOptionsPtr Convert(const blink::PaymentOptions* input) {
    PaymentOptionsPtr output = payments::mojom::blink::PaymentOptions::New();
    output->request_payer_name = input->requestPayerName();
    output->request_payer_email = input->requestPayerEmail();
    output->request_payer_phone = input->requestPayerPhone();
    output->request_shipping = input->requestShipping();

    if (input->shippingType() == "delivery")
      output->shipping_type = PaymentShippingType::DELIVERY;
    else if (input->shippingType() == "pickup")
      output->shipping_type = PaymentShippingType::PICKUP;
    else
      output->shipping_type = PaymentShippingType::SHIPPING;

    return output;
  }
};

template <>
struct TypeConverter<PaymentValidationErrorsPtr,
                     blink::PaymentValidationErrors*> {
  static PaymentValidationErrorsPtr Convert(
      const blink::PaymentValidationErrors* input) {
    PaymentValidationErrorsPtr output =
        payments::mojom::blink::PaymentValidationErrors::New();
    output->error = input->hasError() ? input->error() : g_empty_string;
    output->payer = input->hasPayer()
                        ? PayerErrors::From(input->payer())
                        : PayerErrors::From(blink::PayerErrors::Create());
    output->shipping_address =
        input->hasShippingAddress()
            ? AddressErrors::From(input->shippingAddress())
            : AddressErrors::From(blink::AddressErrors::Create());
    return output;
  }
};

template <>
struct TypeConverter<PayerErrorsPtr, blink::PayerErrors*> {
  static PayerErrorsPtr Convert(const blink::PayerErrors* input) {
    PayerErrorsPtr output = payments::mojom::blink::PayerErrors::New();
    output->email = input->hasEmail() ? input->email() : g_empty_string;
    output->name = input->hasName() ? input->name() : g_empty_string;
    output->phone = input->hasPhone() ? input->phone() : g_empty_string;
    return output;
  }
};

template <>
struct TypeConverter<AddressErrorsPtr, blink::AddressErrors*> {
  static AddressErrorsPtr Convert(const blink::AddressErrors* input) {
    AddressErrorsPtr output = payments::mojom::blink::AddressErrors::New();
    output->address_line =
        input->hasAddressLine() ? input->addressLine() : g_empty_string;
    output->city = input->hasCity() ? input->city() : g_empty_string;
    output->country = input->hasCountry() ? input->country() : g_empty_string;
    output->dependent_locality = input->hasDependentLocality()
                                     ? input->dependentLocality()
                                     : g_empty_string;
    output->organization =
        input->hasOrganization() ? input->organization() : g_empty_string;
    output->phone = input->hasPhone() ? input->phone() : g_empty_string;
    output->postal_code =
        input->hasPostalCode() ? input->postalCode() : g_empty_string;
    output->recipient =
        input->hasRecipient() ? input->recipient() : g_empty_string;
    output->region = input->hasRegion() ? input->region() : g_empty_string;
    output->sorting_code =
        input->hasSortingCode() ? input->sortingCode() : g_empty_string;
    return output;
  }
};

}  // namespace mojo

namespace blink {
namespace {

// Validates ShippingOption or PaymentItem, which happen to have identical
// fields, except for "id", which is present only in ShippingOption.
template <typename T>
void ValidateShippingOptionOrPaymentItem(const T* item,
                                         const String& item_name,
                                         ExecutionContext& execution_context,
                                         ExceptionState& exception_state) {
  DCHECK(item->hasLabel());
  DCHECK(item->hasAmount());
  DCHECK(item->amount()->hasValue());
  DCHECK(item->amount()->hasCurrency());

  if (item->label().length() > PaymentRequest::kMaxStringLength) {
    exception_state.ThrowTypeError("The label for " + item_name +
                                   " cannot be longer than 1024 characters");
    return;
  }

  if (item->amount()->currency().length() > PaymentRequest::kMaxStringLength) {
    exception_state.ThrowTypeError("The currency code for " + item_name +
                                   " cannot be longer than 1024 characters");
    return;
  }

  if (item->amount()->value().length() > PaymentRequest::kMaxStringLength) {
    exception_state.ThrowTypeError("The amount value for " + item_name +
                                   " cannot be longer than 1024 characters");
    return;
  }

  String error_message;
  if (!PaymentsValidators::IsValidCurrencyCodeFormat(item->amount()->currency(),
                                                     &error_message)) {
    exception_state.ThrowRangeError(error_message);
    return;
  }

  if (!PaymentsValidators::IsValidAmountFormat(item->amount()->value(),
                                               item_name, &error_message)) {
    exception_state.ThrowTypeError(error_message);
    return;
  }

  if (item->label().IsEmpty()) {
    execution_context.AddConsoleMessage(ConsoleMessage::Create(
        mojom::ConsoleMessageSource::kJavaScript,
        mojom::ConsoleMessageLevel::kError,
        "Empty " + item_name + " label may be confusing the user"));
    return;
  }
}

void ValidateAndConvertDisplayItems(
    const HeapVector<Member<PaymentItem>>& input,
    const String& item_names,
    Vector<PaymentItemPtr>& output,
    ExecutionContext& execution_context,
    ExceptionState& exception_state) {
  if (input.size() > PaymentRequest::kMaxListSize) {
    exception_state.ThrowTypeError("At most 1024 " + item_names + " allowed");
    return;
  }

  for (PaymentItem* item : input) {
    ValidateShippingOptionOrPaymentItem(item, item_names, execution_context,
                                        exception_state);
    if (exception_state.HadException())
      return;
    output.push_back(payments::mojom::blink::PaymentItem::From(item));
  }
}

// Validates and converts |input| shipping options into |output|. Throws an
// exception if the data is not valid, except for duplicate identifiers, which
// returns an empty |output| instead of throwing an exception. There's no need
// to clear |output| when an exception is thrown, because the caller takes care
// of deleting |output|.
void ValidateAndConvertShippingOptions(
    const HeapVector<Member<PaymentShippingOption>>& input,
    Vector<PaymentShippingOptionPtr>& output,
    String& shipping_option_output,
    ExecutionContext& execution_context,
    ExceptionState& exception_state) {
  if (input.size() > PaymentRequest::kMaxListSize) {
    exception_state.ThrowTypeError("At most 1024 shipping options allowed");
    return;
  }

  HashSet<String> unique_ids;
  for (PaymentShippingOption* option : input) {
    ValidateShippingOptionOrPaymentItem(option, "shippingOptions",
                                        execution_context, exception_state);
    if (exception_state.HadException())
      return;

    DCHECK(option->hasId());
    if (option->id().length() > PaymentRequest::kMaxStringLength) {
      exception_state.ThrowTypeError(
          "Shipping option ID cannot be longer than 1024 characters");
      return;
    }

    if (option->id().IsEmpty()) {
      execution_context.AddConsoleMessage(ConsoleMessage::Create(
          mojom::ConsoleMessageSource::kJavaScript,
          mojom::ConsoleMessageLevel::kWarning,
          "Empty shipping option ID may be hard to debug"));
      return;
    }

    if (unique_ids.Contains(option->id())) {
      exception_state.ThrowTypeError(
          "Cannot have duplicate shipping option identifiers");
      return;
    }

    if (option->selected())
      shipping_option_output = option->id();

    unique_ids.insert(option->id());

    output.push_back(
        payments::mojom::blink::PaymentShippingOption::From(option));
  }
}

void ValidateAndConvertTotal(const PaymentItem* input,
                             const String& item_name,
                             PaymentItemPtr& output,
                             ExecutionContext& execution_context,
                             ExceptionState& exception_state) {
  ValidateShippingOptionOrPaymentItem(input, item_name, execution_context,
                                      exception_state);
  if (exception_state.HadException())
    return;

  if (input->amount()->value()[0] == '-') {
    exception_state.ThrowTypeError("Total amount value should be non-negative");
    return;
  }

  output = payments::mojom::blink::PaymentItem::From(
      const_cast<PaymentItem*>(input));
}

// Parses Android Pay data to avoid parsing JSON in the browser.
void SetAndroidPayMethodData(const ScriptValue& input,
                             PaymentMethodDataPtr& output,
                             ExceptionState& exception_state) {
  AndroidPayMethodData* android_pay = AndroidPayMethodData::Create();
  V8AndroidPayMethodData::ToImpl(input.GetIsolate(), input.V8Value(),
                                 android_pay, exception_state);
  if (exception_state.HadException())
    return;

  if (android_pay->hasEnvironment() && android_pay->environment() == "TEST")
    output->environment = payments::mojom::blink::AndroidPayEnvironment::TEST;

  if (android_pay->hasMerchantName() &&
      android_pay->merchantName().length() > PaymentRequest::kMaxStringLength) {
    exception_state.ThrowTypeError(
        "Android Pay merchant name cannot be longer than 1024 characters");
    return;
  }
  output->merchant_name = android_pay->merchantName();

  if (android_pay->hasMerchantId() &&
      android_pay->merchantId().length() > PaymentRequest::kMaxStringLength) {
    exception_state.ThrowTypeError(
        "Android Pay merchant id cannot be longer than 1024 characters");
    return;
  }
  output->merchant_id = android_pay->merchantId();

  // 0 means the merchant did not specify or it was an invalid value
  output->min_google_play_services_version = 0;
  if (android_pay->hasMinGooglePlayServicesVersion()) {
    bool ok = false;
    int min_google_play_services_version =
        android_pay->minGooglePlayServicesVersion().ToIntStrict(&ok);
    if (ok) {
      output->min_google_play_services_version =
          min_google_play_services_version;
    }
  }

  // 0 means the merchant did not specify or it was an invalid value
  output->api_version = 0;
  if (android_pay->hasApiVersion())
    output->api_version = android_pay->apiVersion();

  if (android_pay->hasAllowedCardNetworks()) {
    using ::payments::mojom::blink::AndroidPayCardNetwork;

    const struct {
      const AndroidPayCardNetwork code;
      const char* const name;
    } kAndroidPayNetwork[] = {{AndroidPayCardNetwork::AMEX, "AMEX"},
                              {AndroidPayCardNetwork::DISCOVER, "DISCOVER"},
                              {AndroidPayCardNetwork::MASTERCARD, "MASTERCARD"},
                              {AndroidPayCardNetwork::VISA, "VISA"}};

    for (const String& allowed_card_network :
         android_pay->allowedCardNetworks()) {
      for (size_t i = 0; i < base::size(kAndroidPayNetwork); ++i) {
        if (allowed_card_network == kAndroidPayNetwork[i].name) {
          output->allowed_card_networks.push_back(kAndroidPayNetwork[i].code);
          break;
        }
      }
    }
  }

  if (android_pay->hasPaymentMethodTokenizationParameters()) {
    const blink::AndroidPayTokenization* tokenization =
        android_pay->paymentMethodTokenizationParameters();
    output->tokenization_type =
        payments::mojom::blink::AndroidPayTokenization::UNSPECIFIED;
    if (tokenization->hasTokenizationType()) {
      using ::payments::mojom::blink::AndroidPayTokenization;

      const struct {
        const AndroidPayTokenization code;
        const char* const name;
      } kAndroidPayTokenization[] = {
          {AndroidPayTokenization::GATEWAY_TOKEN, "GATEWAY_TOKEN"},
          {AndroidPayTokenization::NETWORK_TOKEN, "NETWORK_TOKEN"}};

      for (size_t i = 0; i < base::size(kAndroidPayTokenization); ++i) {
        if (tokenization->tokenizationType() ==
            kAndroidPayTokenization[i].name) {
          output->tokenization_type = kAndroidPayTokenization[i].code;
          break;
        }
      }
    }

    if (tokenization->hasParameters()) {
      const Vector<String>& keys =
          tokenization->parameters().GetPropertyNames(exception_state);
      if (exception_state.HadException())
        return;
      if (keys.size() > PaymentRequest::kMaxListSize) {
        exception_state.ThrowTypeError(
            "At most 1024 tokenization parameters allowed for Android Pay");
        return;
      }
      String value;
      for (const String& key : keys) {
        if (!DictionaryHelper::Get(tokenization->parameters(), key, value))
          continue;
        if (key.length() > PaymentRequest::kMaxStringLength) {
          exception_state.ThrowTypeError(
              "Android Pay tokenization parameter key cannot be longer than "
              "1024 characters");
          return;
        }
        if (value.length() > PaymentRequest::kMaxStringLength) {
          exception_state.ThrowTypeError(
              "Android Pay tokenization parameter value cannot be longer than "
              "1024 characters");
          return;
        }
        output->parameters.push_back(
            payments::mojom::blink::AndroidPayTokenizationParameter::New());
        output->parameters.back()->key = key;
        output->parameters.back()->value = value;
      }
    }
  }
}

// Parses basic-card data to avoid parsing JSON in the browser.
void SetBasicCardMethodData(const ScriptValue& input,
                            PaymentMethodDataPtr& output,
                            ExceptionState& exception_state) {
  BasicCardHelper::ParseBasiccardData(input, output->supported_networks,
                                      output->supported_types, exception_state);
}

void StringifyAndParseMethodSpecificData(ExecutionContext& execution_context,
                                         const String& supported_method,
                                         const ScriptValue& input,
                                         PaymentMethodDataPtr& output,
                                         ExceptionState& exception_state) {
  DCHECK(!input.IsEmpty());
  v8::Local<v8::String> value;
  if (!input.V8Value()->IsObject() ||
      !v8::JSON::Stringify(input.GetContext(), input.V8Value().As<v8::Object>())
           .ToLocal(&value)) {
    exception_state.ThrowTypeError(
        "Payment method data should be a JSON-serializable object");
    return;
  }

  output->stringified_data = ToBlinkString<String>(value, kDoNotExternalize);

  if (output->stringified_data.length() >
      PaymentRequest::kMaxJSONStringLength) {
    exception_state.ThrowTypeError(
        "JSON serialization of payment method data should be no longer than "
        "1048576 characters");
    return;
  }

  // Serialize payment method specific data to be sent to the payment apps. The
  // payment apps are responsible for validating and processing their method
  // data asynchronously. Do not throw exceptions here.
  if (supported_method == "https://android.com/pay" ||
      supported_method == "https://google.com/pay") {
    SetAndroidPayMethodData(input, output, exception_state);
    if (exception_state.HadException())
      exception_state.ClearException();
  }

  if (supported_method == "basic-card") {
    SetBasicCardMethodData(input, output, exception_state);
    if (exception_state.HadException()) {
      UseCounter::Count(&execution_context,
                        WebFeature::kInvalidBasicCardMethodData);
      exception_state.ClearException();
    }
  }
}

void CountPaymentRequestNetworkNameInSupportedMethod(
    const String& supported_method,
    ExecutionContext& execution_context) {
  if (BasicCardHelper::IsNetworkName(supported_method)) {
    Deprecation::CountDeprecation(
        &execution_context,
        WebFeature::kPaymentRequestNetworkNameInSupportedMethods);
  }
}

void ValidateAndConvertPaymentDetailsModifiers(
    const HeapVector<Member<PaymentDetailsModifier>>& input,
    Vector<PaymentDetailsModifierPtr>& output,
    ExecutionContext& execution_context,
    ExceptionState& exception_state) {
  if (input.size() > PaymentRequest::kMaxListSize) {
    exception_state.ThrowTypeError("At most 1024 modifiers allowed");
    return;
  }

  for (const PaymentDetailsModifier* modifier : input) {
    output.push_back(payments::mojom::blink::PaymentDetailsModifier::New());
    if (modifier->hasTotal()) {
      ValidateAndConvertTotal(modifier->total(), "modifier total",
                              output.back()->total, execution_context,
                              exception_state);
      if (exception_state.HadException())
        return;
    }

    if (modifier->hasAdditionalDisplayItems()) {
      ValidateAndConvertDisplayItems(modifier->additionalDisplayItems(),
                                     "additional display items in modifier",
                                     output.back()->additional_display_items,
                                     execution_context, exception_state);
      if (exception_state.HadException())
        return;
    }

    if (!PaymentsValidators::IsValidMethodFormat(modifier->supportedMethod())) {
      exception_state.ThrowRangeError(
          "Invalid payment method identifier format");
      return;
    }

    CountPaymentRequestNetworkNameInSupportedMethod(modifier->supportedMethod(),
                                                    execution_context);

    output.back()->method_data =
        payments::mojom::blink::PaymentMethodData::New();
    output.back()->method_data->supported_method = modifier->supportedMethod();

    if (modifier->hasData() && !modifier->data().IsEmpty()) {
      StringifyAndParseMethodSpecificData(
          execution_context, modifier->supportedMethod(), modifier->data(),
          output.back()->method_data, exception_state);
    } else {
      output.back()->method_data->stringified_data = "";
    }
  }
}

void ValidateAndConvertPaymentDetailsBase(const PaymentDetailsBase* input,
                                          const PaymentOptions* options,
                                          PaymentDetailsPtr& output,
                                          String& shipping_option_output,
                                          ExecutionContext& execution_context,
                                          ExceptionState& exception_state) {
  if (input->hasDisplayItems()) {
    ValidateAndConvertDisplayItems(input->displayItems(), "display items",
                                   output->display_items, execution_context,
                                   exception_state);
    if (exception_state.HadException())
      return;
  }

  // If requestShipping is specified and there are shipping options to validate,
  // proceed with validation.
  if (options->requestShipping() && input->hasShippingOptions()) {
    output->shipping_options = Vector<PaymentShippingOptionPtr>();
    ValidateAndConvertShippingOptions(
        input->shippingOptions(), *output->shipping_options,
        shipping_option_output, execution_context, exception_state);
    if (exception_state.HadException())
      return;
  } else {
    shipping_option_output = String();
  }

  if (input->hasModifiers()) {
    ValidateAndConvertPaymentDetailsModifiers(
        input->modifiers(), output->modifiers, execution_context,
        exception_state);
  }
}

void ValidateAndConvertPaymentDetailsInit(const PaymentDetailsInit* input,
                                          const PaymentOptions* options,
                                          PaymentDetailsPtr& output,
                                          String& shipping_option_output,
                                          ExecutionContext& execution_context,
                                          ExceptionState& exception_state) {
  DCHECK(input->hasTotal());
  ValidateAndConvertTotal(input->total(), "total", output->total,
                          execution_context, exception_state);
  if (exception_state.HadException())
    return;

  ValidateAndConvertPaymentDetailsBase(input, options, output,
                                       shipping_option_output,
                                       execution_context, exception_state);
}

void ValidateAndConvertPaymentDetailsUpdate(const PaymentDetailsUpdate* input,
                                            const PaymentOptions* options,
                                            PaymentDetailsPtr& output,
                                            String& shipping_option_output,
                                            ExecutionContext& execution_context,
                                            ExceptionState& exception_state) {
  ValidateAndConvertPaymentDetailsBase(input, options, output,
                                       shipping_option_output,
                                       execution_context, exception_state);
  if (exception_state.HadException())
    return;

  if (input->hasTotal()) {
    ValidateAndConvertTotal(input->total(), "total", output->total,
                            execution_context, exception_state);
    if (exception_state.HadException())
      return;
  }

  if (input->hasError()) {
    String error_message;
    if (!PaymentsValidators::IsValidErrorMsgFormat(input->error(),
                                                   &error_message)) {
      exception_state.ThrowTypeError(error_message);
      return;
    }
    output->error = input->error();
  }

  if (input->hasShippingAddressErrors()) {
    String error_message;
    if (!PaymentsValidators::IsValidAddressErrorsFormat(
            input->shippingAddressErrors(), &error_message)) {
      exception_state.ThrowTypeError(error_message);
      return;
    }
    output->shipping_address_errors =
        payments::mojom::blink::AddressErrors::From(
            input->shippingAddressErrors());
  }
}

void ValidateAndConvertPaymentMethodData(
    const HeapVector<Member<PaymentMethodData>>& input,
    Vector<payments::mojom::blink::PaymentMethodDataPtr>& output,
    HashSet<String>& method_names,
    ExecutionContext& execution_context,
    ExceptionState& exception_state) {
  if (input.IsEmpty()) {
    exception_state.ThrowTypeError("At least one payment method is required");
    return;
  }

  if (input.size() > PaymentRequest::kMaxListSize) {
    exception_state.ThrowTypeError(
        "At most 1024 payment methods are supported");
    return;
  }

  for (const PaymentMethodData* payment_method_data : input) {
    if (!PaymentsValidators::IsValidMethodFormat(
            payment_method_data->supportedMethod())) {
      exception_state.ThrowRangeError(
          "Invalid payment method identifier format");
      return;
    }
    method_names.insert(payment_method_data->supportedMethod());

    CountPaymentRequestNetworkNameInSupportedMethod(
        payment_method_data->supportedMethod(), execution_context);

    output.push_back(payments::mojom::blink::PaymentMethodData::New());

    output.back()->supported_method = payment_method_data->supportedMethod();

    if (payment_method_data->hasData() &&
        !payment_method_data->data().IsEmpty()) {
      StringifyAndParseMethodSpecificData(
          execution_context, payment_method_data->supportedMethod(),
          payment_method_data->data(), output.back(), exception_state);
    } else {
      output.back()->stringified_data = "";
    }
  }
}

bool AllowedToUsePaymentRequest(const ExecutionContext* execution_context) {
  // To determine whether a Document object |document| is allowed to use the
  // feature indicated by attribute name |allowpaymentrequest|, run these steps:

  // Note: PaymentRequest is only exposed to Window and not workers.
  // 1. If |document| has no browsing context, then return false.
  const Document* document = To<Document>(execution_context);
  if (!document->GetFrame())
    return false;

  // 2. If Feature Policy is enabled, return the policy for "payment" feature.
  return document->IsFeatureEnabled(mojom::FeaturePolicyFeature::kPayment,
                                    ReportOptions::kReportOnFailure);
}

void WarnIgnoringQueryQuotaForCanMakePayment(
    ExecutionContext& execution_context,
    const char* method_name) {
  const String& error = String::Format(
      "Quota reached for PaymentRequest.%s(). This would normally "
      "reject the promise, but allowing continued usage on localhost and "
      "file:// scheme origins.",
      method_name);
  execution_context.AddConsoleMessage(
      ConsoleMessage::Create(mojom::ConsoleMessageSource::kJavaScript,
                             mojom::ConsoleMessageLevel::kWarning, error));
}

}  // namespace

PaymentRequest* PaymentRequest::Create(
    ExecutionContext* execution_context,
    const HeapVector<Member<PaymentMethodData>>& method_data,
    const PaymentDetailsInit* details,
    ExceptionState& exception_state) {
  return MakeGarbageCollected<PaymentRequest>(execution_context, method_data,
                                              details, PaymentOptions::Create(),
                                              exception_state);
}

PaymentRequest* PaymentRequest::Create(
    ExecutionContext* execution_context,
    const HeapVector<Member<PaymentMethodData>>& method_data,
    const PaymentDetailsInit* details,
    const PaymentOptions* options,
    ExceptionState& exception_state) {
  return MakeGarbageCollected<PaymentRequest>(
      execution_context, method_data, details, options, exception_state);
}

PaymentRequest::~PaymentRequest() = default;

ScriptPromise PaymentRequest::show(ScriptState* script_state) {
  return show(script_state, ScriptPromise());
}

ScriptPromise PaymentRequest::show(ScriptState* script_state,
                                   ScriptPromise details_promise) {
  if (!script_state->ContextIsValid() || !LocalDOMWindow::From(script_state) ||
      !LocalDOMWindow::From(script_state)->GetFrame()) {
    return ScriptPromise::RejectWithDOMException(
        script_state, DOMException::Create(DOMExceptionCode::kAbortError,
                                           "Cannot show the payment request"));
  }

  if (!payment_provider_.is_bound() || accept_resolver_) {
    return ScriptPromise::RejectWithDOMException(
        script_state, DOMException::Create(DOMExceptionCode::kInvalidStateError,
                                           "Already called show() once"));
  }

  // TODO(crbug.com/825270): Reject with SecurityError DOMException if triggered
  // without user activation.
  bool is_user_gesture = LocalFrame::HasTransientUserActivation(GetFrame());
  if (!is_user_gesture) {
    UseCounter::Count(GetExecutionContext(),
                      WebFeature::kPaymentRequestShowWithoutGesture);
  }

  // TODO(crbug.com/779126): add support for handling payment requests in
  // immersive mode.
  if (GetFrame()->GetDocument()->GetSettings()->GetImmersiveModeEnabled()) {
    return ScriptPromise::RejectWithDOMException(
        script_state, DOMException::Create(DOMExceptionCode::kInvalidStateError,
                                           "Page popups are suppressed"));
  }

  is_waiting_for_show_promise_to_resolve_ = !details_promise.IsEmpty();
  payment_provider_->Show(is_user_gesture,
                          is_waiting_for_show_promise_to_resolve_);
  if (is_waiting_for_show_promise_to_resolve_) {
    // If the website does not calculate the final shopping cart contents within
    // 10 seconds, abort payment.
    update_payment_details_timer_.StartOneShot(TimeDelta::FromSeconds(10),
                                               FROM_HERE);
    details_promise.Then(
        UpdatePaymentDetailsFunction::CreateFunction(
            script_state, this,
            UpdatePaymentDetailsFunction::ResolveType::kFulfill),
        UpdatePaymentDetailsFunction::CreateFunction(
            script_state, this,
            UpdatePaymentDetailsFunction::ResolveType::kReject));
  }

  accept_resolver_ = MakeGarbageCollected<ScriptPromiseResolver>(script_state);
  return accept_resolver_->Promise();
}

ScriptPromise PaymentRequest::abort(ScriptState* script_state) {
  if (!script_state->ContextIsValid()) {
    return ScriptPromise::RejectWithDOMException(
        script_state, DOMException::Create(DOMExceptionCode::kInvalidStateError,
                                           "Cannot abort payment"));
  }

  if (abort_resolver_) {
    return ScriptPromise::RejectWithDOMException(
        script_state,
        DOMException::Create(DOMExceptionCode::kInvalidStateError,
                             "Cannot abort() again until the previous abort() "
                             "has resolved or rejected"));
  }

  if (!GetPendingAcceptPromiseResolver()) {
    return ScriptPromise::RejectWithDOMException(
        script_state,
        DOMException::Create(
            DOMExceptionCode::kInvalidStateError,
            "No show() or retry() in progress, so nothing to abort"));
  }

  abort_resolver_ = MakeGarbageCollected<ScriptPromiseResolver>(script_state);
  payment_provider_->Abort();
  return abort_resolver_->Promise();
}

ScriptPromise PaymentRequest::canMakePayment(ScriptState* script_state) {
  if (!payment_provider_.is_bound() || GetPendingAcceptPromiseResolver() ||
      can_make_payment_resolver_ || !script_state->ContextIsValid()) {
    return ScriptPromise::RejectWithDOMException(
        script_state, DOMException::Create(DOMExceptionCode::kInvalidStateError,
                                           "Cannot query payment request"));
  }

  bool legacy_mode =
      !RuntimeEnabledFeatures::PaymentRequestHasEnrolledInstrumentEnabled();
  payment_provider_->CanMakePayment(legacy_mode);

  can_make_payment_resolver_ =
      MakeGarbageCollected<ScriptPromiseResolver>(script_state);
  return can_make_payment_resolver_->Promise();
}

ScriptPromise PaymentRequest::hasEnrolledInstrument(ScriptState* script_state) {
  if (!payment_provider_.is_bound() || GetPendingAcceptPromiseResolver() ||
      has_enrolled_instrument_resolver_ || !script_state->ContextIsValid()) {
    return ScriptPromise::RejectWithDOMException(
        script_state, DOMException::Create(DOMExceptionCode::kInvalidStateError,
                                           "Cannot query payment request"));
  }

  bool per_method_quota =
      RuntimeEnabledFeatures::PerMethodCanMakePaymentQuotaEnabled(
          GetExecutionContext());
  if (per_method_quota) {
    UseCounter::Count(GetExecutionContext(),
                      WebFeature::kPerMethodCanMakePaymentQuota);
  }

  payment_provider_->HasEnrolledInstrument(per_method_quota);

  has_enrolled_instrument_resolver_ =
      MakeGarbageCollected<ScriptPromiseResolver>(script_state);
  return has_enrolled_instrument_resolver_->Promise();
}

bool PaymentRequest::HasPendingActivity() const {
  return GetPendingAcceptPromiseResolver() || complete_resolver_;
}

const AtomicString& PaymentRequest::InterfaceName() const {
  return event_target_names::kPaymentRequest;
}

ExecutionContext* PaymentRequest::GetExecutionContext() const {
  return ContextLifecycleObserver::GetExecutionContext();
}

ScriptPromise PaymentRequest::Retry(ScriptState* script_state,
                                    const PaymentValidationErrors* errors) {
  if (!script_state->ContextIsValid() || !LocalDOMWindow::From(script_state) ||
      !LocalDOMWindow::From(script_state)->GetFrame()) {
    return ScriptPromise::RejectWithDOMException(
        script_state, DOMException::Create(DOMExceptionCode::kAbortError,
                                           "Cannot retry the payment request"));
  }

  if (complete_resolver_) {
    return ScriptPromise::RejectWithDOMException(
        script_state,
        DOMException::Create(
            DOMExceptionCode::kInvalidStateError,
            "Cannot call retry() because already called complete()"));
  }

  if (retry_resolver_) {
    return ScriptPromise::RejectWithDOMException(
        script_state, DOMException::Create(DOMExceptionCode::kInvalidStateError,
                                           "Cannot call retry() again until "
                                           "the previous retry() is finished"));
  }

  if (!payment_provider_) {
    return ScriptPromise::RejectWithDOMException(
        script_state, DOMException::Create(DOMExceptionCode::kInvalidStateError,
                                           "Payment request terminated"));
  }

  String error_message;
  if (!PaymentsValidators::IsValidPaymentValidationErrorsFormat(
          errors, &error_message)) {
    return ScriptPromise::Reject(
        script_state, V8ThrowException::CreateTypeError(
                          script_state->GetIsolate(), error_message));
  }

  if (!options_->requestPayerName() && errors->hasPayer() &&
      errors->payer()->hasName()) {
    GetExecutionContext()->AddConsoleMessage(
        ConsoleMessage::Create(mojom::ConsoleMessageSource::kJavaScript,
                               mojom::ConsoleMessageLevel::kWarning,
                               "The payer.name passed to retry() may not be "
                               "shown because requestPayerName is false"));
  }

  if (!options_->requestPayerEmail() && errors->hasPayer() &&
      errors->payer()->hasEmail()) {
    GetExecutionContext()->AddConsoleMessage(
        ConsoleMessage::Create(mojom::ConsoleMessageSource::kJavaScript,
                               mojom::ConsoleMessageLevel::kWarning,
                               "The payer.email passed to retry() may not be "
                               "shown because requestPayerEmail is false"));
  }

  if (!options_->requestPayerPhone() && errors->hasPayer() &&
      errors->payer()->hasPhone()) {
    GetExecutionContext()->AddConsoleMessage(
        ConsoleMessage::Create(mojom::ConsoleMessageSource::kJavaScript,
                               mojom::ConsoleMessageLevel::kWarning,
                               "The payer.phone passed to retry() may not be "
                               "shown because requestPayerPhone is false"));
  }

  if (!options_->requestShipping() && errors->hasShippingAddress()) {
    GetExecutionContext()->AddConsoleMessage(
        ConsoleMessage::Create(mojom::ConsoleMessageSource::kJavaScript,
                               mojom::ConsoleMessageLevel::kWarning,
                               "The shippingAddress passed to retry() may not "
                               "be shown because requestShipping is false"));
  }

  complete_timer_.Stop();

  // The payment provider should respond in PaymentRequest::OnPaymentResponse().
  payment_provider_->Retry(
      payments::mojom::blink::PaymentValidationErrors::From(
          const_cast<PaymentValidationErrors*>(errors)));

  retry_resolver_ = MakeGarbageCollected<ScriptPromiseResolver>(script_state);

  return retry_resolver_->Promise();
}

ScriptPromise PaymentRequest::Complete(ScriptState* script_state,
                                       PaymentComplete result) {
  if (!script_state->ContextIsValid()) {
    return ScriptPromise::RejectWithDOMException(
        script_state, DOMException::Create(DOMExceptionCode::kInvalidStateError,
                                           "Cannot complete payment"));
  }

  if (complete_resolver_) {
    return ScriptPromise::RejectWithDOMException(
        script_state, DOMException::Create(DOMExceptionCode::kInvalidStateError,
                                           "Already called complete() once"));
  }

  if (retry_resolver_) {
    return ScriptPromise::RejectWithDOMException(
        script_state, DOMException::Create(
                          DOMExceptionCode::kInvalidStateError,
                          "Cannot call complete() before retry() is finished"));
  }

  if (!complete_timer_.IsActive()) {
    return ScriptPromise::RejectWithDOMException(
        script_state,
        DOMException::Create(
            DOMExceptionCode::kInvalidStateError,
            "Timed out after 60 seconds, complete() called too late"));
  }

  // User has cancelled the transaction while the website was processing it.
  if (!payment_provider_) {
    return ScriptPromise::RejectWithDOMException(
        script_state, DOMException::Create(DOMExceptionCode::kAbortError,
                                           "Request cancelled"));
  }

  complete_timer_.Stop();

  // The payment provider should respond in PaymentRequest::OnComplete().
  payment_provider_->Complete(payments::mojom::blink::PaymentComplete(result));

  complete_resolver_ =
      MakeGarbageCollected<ScriptPromiseResolver>(script_state);
  return complete_resolver_->Promise();
}

void PaymentRequest::OnUpdatePaymentDetails(
    const ScriptValue& details_script_value) {
  ScriptPromiseResolver* resolver = GetPendingAcceptPromiseResolver();
  if (!resolver || !payment_provider_ ||
      !update_payment_details_timer_.IsActive()) {
    return;
  }

  update_payment_details_timer_.Stop();

  PaymentDetailsUpdate* details = PaymentDetailsUpdate::Create();
  ExceptionState exception_state(v8::Isolate::GetCurrent(),
                                 ExceptionState::kConstructionContext,
                                 "PaymentDetailsUpdate");
  V8PaymentDetailsUpdate::ToImpl(details_script_value.GetIsolate(),
                                 details_script_value.V8Value(), details,
                                 exception_state);
  if (exception_state.HadException()) {
    resolver->Reject(exception_state.GetException());
    ClearResolversAndCloseMojoConnection();
    return;
  }

  PaymentDetailsPtr validated_details =
      payments::mojom::blink::PaymentDetails::New();
  ValidateAndConvertPaymentDetailsUpdate(
      details, options_, validated_details, shipping_option_,
      *GetExecutionContext(), exception_state);
  if (exception_state.HadException()) {
    resolver->Reject(exception_state.GetException());
    ClearResolversAndCloseMojoConnection();
    return;
  }

  if (!options_->requestShipping())
    validated_details->shipping_options = base::nullopt;

  if (is_waiting_for_show_promise_to_resolve_) {
    is_waiting_for_show_promise_to_resolve_ = false;

    if (!validated_details->error.IsEmpty()) {
      resolver->Reject(
          DOMException::Create(DOMExceptionCode::kInvalidStateError,
                               "Cannot specify 'error' when resolving the "
                               "promise passed into PaymentRequest.show()"));
      ClearResolversAndCloseMojoConnection();
      return;
    }
  }

  payment_provider_->UpdateWith(std::move(validated_details));
}

void PaymentRequest::OnUpdatePaymentDetailsFailure(const String& error) {
  if (!payment_provider_)
    return;
  if (update_payment_details_timer_.IsActive())
    update_payment_details_timer_.Stop();
  ScriptPromiseResolver* resolver = GetPendingAcceptPromiseResolver();
  if (resolver) {
    resolver->Reject(
        DOMException::Create(DOMExceptionCode::kAbortError, error));
  }
  if (complete_resolver_) {
    complete_resolver_->Reject(
        DOMException::Create(DOMExceptionCode::kAbortError, error));
  }
  ClearResolversAndCloseMojoConnection();
}

bool PaymentRequest::IsInteractive() const {
  return !!GetPendingAcceptPromiseResolver();
}

void PaymentRequest::Trace(blink::Visitor* visitor) {
  visitor->Trace(options_);
  visitor->Trace(shipping_address_);
  visitor->Trace(payment_response_);
  visitor->Trace(accept_resolver_);
  visitor->Trace(retry_resolver_);
  visitor->Trace(complete_resolver_);
  visitor->Trace(abort_resolver_);
  visitor->Trace(can_make_payment_resolver_);
  visitor->Trace(has_enrolled_instrument_resolver_);
  EventTargetWithInlineData::Trace(visitor);
  ContextLifecycleObserver::Trace(visitor);
}

void PaymentRequest::OnCompleteTimeoutForTesting() {
  complete_timer_.Stop();
  OnCompleteTimeout(nullptr);
}

void PaymentRequest::OnUpdatePaymentDetailsTimeoutForTesting() {
  update_payment_details_timer_.Stop();
  OnUpdatePaymentDetailsTimeout(nullptr);
}

PaymentRequest::PaymentRequest(
    ExecutionContext* execution_context,
    const HeapVector<Member<PaymentMethodData>>& method_data,
    const PaymentDetailsInit* details,
    const PaymentOptions* options,
    ExceptionState& exception_state)
    : ContextLifecycleObserver(execution_context),
      options_(options),
      client_binding_(this),
      complete_timer_(
          execution_context->GetTaskRunner(TaskType::kMiscPlatformAPI),
          this,
          &PaymentRequest::OnCompleteTimeout),
      update_payment_details_timer_(
          execution_context->GetTaskRunner(TaskType::kMiscPlatformAPI),
          this,
          &PaymentRequest::OnUpdatePaymentDetailsTimeout),
      is_waiting_for_show_promise_to_resolve_(false) {
  DCHECK(GetExecutionContext()->IsSecureContext());

  if (!AllowedToUsePaymentRequest(execution_context)) {
    exception_state.ThrowSecurityError(
        "Must be in a top-level browsing context or an iframe needs to specify "
        "'allowpaymentrequest' explicitly");
    return;
  }

  if (details->hasId() &&
      details->id().length() > PaymentRequest::kMaxStringLength) {
    exception_state.ThrowTypeError("ID cannot be longer than 1024 characters");
    return;
  }

  PaymentDetailsPtr validated_details =
      payments::mojom::blink::PaymentDetails::New();
  validated_details->id = id_ =
      details->hasId() ? details->id() : CreateCanonicalUUIDString();

  Vector<payments::mojom::blink::PaymentMethodDataPtr> validated_method_data;
  ValidateAndConvertPaymentMethodData(method_data, validated_method_data,
                                      method_names_, *GetExecutionContext(),
                                      exception_state);
  if (exception_state.HadException())
    return;

  ValidateAndConvertPaymentDetailsInit(details, options_, validated_details,
                                       shipping_option_, *GetExecutionContext(),
                                       exception_state);
  if (exception_state.HadException())
    return;

  if (options_->requestShipping())
    shipping_type_ = options_->shippingType();
  else
    validated_details->shipping_options = base::nullopt;

  DCHECK(shipping_type_.IsNull() || shipping_type_ == "shipping" ||
         shipping_type_ == "delivery" || shipping_type_ == "pickup");

  scoped_refptr<base::SingleThreadTaskRunner> task_runner =
      execution_context->GetTaskRunner(TaskType::kUserInteraction);

  GetFrame()->GetInterfaceProvider().GetInterface(
      mojo::MakeRequest(&payment_provider_, task_runner));
  payment_provider_.set_connection_error_handler(
      WTF::Bind(&PaymentRequest::OnError, WrapWeakPersistent(this),
                PaymentErrorReason::UNKNOWN));

  payments::mojom::blink::PaymentRequestClientPtr client;
  client_binding_.Bind(mojo::MakeRequest(&client, task_runner), task_runner);
  payment_provider_->Init(std::move(client), std::move(validated_method_data),
                          std::move(validated_details),
                          payments::mojom::blink::PaymentOptions::From(
                              const_cast<PaymentOptions*>(options_.Get())));
}

void PaymentRequest::ContextDestroyed(ExecutionContext*) {
  ClearResolversAndCloseMojoConnection();
}

void PaymentRequest::OnPaymentMethodChange(const String& method_name,
                                           const String& stringified_details) {
  DCHECK(GetPendingAcceptPromiseResolver());
  DCHECK(!complete_resolver_);

  if (!RuntimeEnabledFeatures::PaymentMethodChangeEventEnabled()) {
    payment_provider_->NoUpdatedPaymentDetails();
    return;
  }

  ScriptState* script_state =
      GetPendingAcceptPromiseResolver()->GetScriptState();
  ScriptState::Scope scope(script_state);

  PaymentMethodChangeEventInit* init = PaymentMethodChangeEventInit::Create();
  init->setMethodName(method_name);

  if (!stringified_details.IsEmpty()) {
    ExceptionState exception_state(script_state->GetIsolate(),
                                   ExceptionState::kConstructionContext,
                                   "PaymentMethodChangeEvent");
    v8::Local<v8::Value> parsed_value =
        FromJSONString(script_state->GetIsolate(), script_state->GetContext(),
                       stringified_details, exception_state);
    if (exception_state.HadException()) {
      GetPendingAcceptPromiseResolver()->Reject(DOMException::Create(
          DOMExceptionCode::kSyntaxError, exception_state.Message()));
      ClearResolversAndCloseMojoConnection();
      return;
    }
    init->setMethodDetails(ScriptValue(script_state, parsed_value));
  }

  PaymentRequestUpdateEvent* event = PaymentMethodChangeEvent::Create(
      script_state, event_type_names::kPaymentmethodchange, init);
  DispatchPaymentRequestUpdateEvent(this, event);
}

void PaymentRequest::OnShippingAddressChange(PaymentAddressPtr address) {
  DCHECK(GetPendingAcceptPromiseResolver());
  DCHECK(!complete_resolver_);

  String error_message;
  if (!PaymentsValidators::IsValidShippingAddress(address, &error_message)) {
    GetPendingAcceptPromiseResolver()->Reject(
        DOMException::Create(DOMExceptionCode::kSyntaxError, error_message));
    ClearResolversAndCloseMojoConnection();
    return;
  }

  shipping_address_ = MakeGarbageCollected<PaymentAddress>(std::move(address));

  PaymentRequestUpdateEvent* event = PaymentRequestUpdateEvent::Create(
      GetExecutionContext(), event_type_names::kShippingaddresschange);
  DispatchPaymentRequestUpdateEvent(this, event);
}

void PaymentRequest::OnShippingOptionChange(const String& shipping_option_id) {
  DCHECK(GetPendingAcceptPromiseResolver());
  DCHECK(!complete_resolver_);
  shipping_option_ = shipping_option_id;

  PaymentRequestUpdateEvent* event = PaymentRequestUpdateEvent::Create(
      GetExecutionContext(), event_type_names::kShippingoptionchange);
  DispatchPaymentRequestUpdateEvent(this, event);
}

void PaymentRequest::OnPayerDetailChange(
    payments::mojom::blink::PayerDetailPtr detail) {
  CHECK(RuntimeEnabledFeatures::PaymentRetryEnabled());
  DCHECK(payment_response_);
  DCHECK(GetPendingAcceptPromiseResolver());
  DCHECK(!complete_resolver_);

  payment_response_->UpdatePayerDetail(std::move(detail));
  PaymentRequestUpdateEvent* event = PaymentRequestUpdateEvent::Create(
      GetExecutionContext(), event_type_names::kPayerdetailchange);
  DispatchPaymentRequestUpdateEvent(payment_response_, event);
}

void PaymentRequest::OnPaymentResponse(PaymentResponsePtr response) {
  DCHECK(GetPendingAcceptPromiseResolver());
  DCHECK(!complete_resolver_);

  ScriptPromiseResolver* resolver = GetPendingAcceptPromiseResolver();
  if (options_->requestShipping()) {
    if (!response->shipping_address || response->shipping_option.IsEmpty()) {
      resolver->Reject(DOMException::Create(DOMExceptionCode::kSyntaxError));
      ClearResolversAndCloseMojoConnection();
      return;
    }

    String error_message;
    if (!PaymentsValidators::IsValidShippingAddress(response->shipping_address,
                                                    &error_message)) {
      resolver->Reject(
          DOMException::Create(DOMExceptionCode::kSyntaxError, error_message));
      ClearResolversAndCloseMojoConnection();
      return;
    }

    shipping_address_ = MakeGarbageCollected<PaymentAddress>(
        std::move(response->shipping_address));
    shipping_option_ = response->shipping_option;
  } else {
    if (response->shipping_address || !response->shipping_option.IsNull()) {
      resolver->Reject(DOMException::Create(DOMExceptionCode::kSyntaxError));
      ClearResolversAndCloseMojoConnection();
      return;
    }
  }

  DCHECK(response->payer);
  if ((options_->requestPayerName() && response->payer->name.IsEmpty()) ||
      (options_->requestPayerEmail() && response->payer->email.IsEmpty()) ||
      (options_->requestPayerPhone() && response->payer->phone.IsEmpty()) ||
      (!options_->requestPayerName() && !response->payer->name.IsNull()) ||
      (!options_->requestPayerEmail() && !response->payer->email.IsNull()) ||
      (!options_->requestPayerPhone() && !response->payer->phone.IsNull())) {
    resolver->Reject(DOMException::Create(DOMExceptionCode::kSyntaxError));
    ClearResolversAndCloseMojoConnection();
    return;
  }

  // If the website does not call complete() 60 seconds after show() has been
  // resolved, then behave as if the website called complete("fail").
  complete_timer_.StartOneShot(TimeDelta::FromSeconds(60), FROM_HERE);

  if (retry_resolver_) {
    DCHECK(payment_response_);
    payment_response_->Update(retry_resolver_->GetScriptState(),
                              std::move(response), shipping_address_.Get());
    retry_resolver_->Resolve();

    // Do not close the mojo connection here. The merchant website should call
    // PaymentResponse::complete(String), which will be forwarded over the mojo
    // connection to display a success or failure message to the user.
    retry_resolver_.Clear();
  } else if (accept_resolver_) {
    payment_response_ = MakeGarbageCollected<PaymentResponse>(
        accept_resolver_->GetScriptState(), std::move(response),
        shipping_address_.Get(), this, id_);
    accept_resolver_->Resolve(payment_response_);

    // Do not close the mojo connection here. The merchant website should call
    // PaymentResponse::complete(String), which will be forwarded over the mojo
    // connection to display a success or failure message to the user.
    accept_resolver_.Clear();
  }
}

void PaymentRequest::OnError(PaymentErrorReason error) {
  DOMExceptionCode exception_code = DOMExceptionCode::kUnknownError;
  String message;

  switch (error) {
    case PaymentErrorReason::USER_CANCEL: {
      exception_code = DOMExceptionCode::kAbortError;
      message = "Request cancelled";
      break;
    }

    case PaymentErrorReason::NOT_SUPPORTED: {
      exception_code = DOMExceptionCode::kNotSupportedError;
      DCHECK_LE(1U, method_names_.size());
      auto it = method_names_.begin();
      if (method_names_.size() == 1U) {
        message = "The payment method \"" + *it + "\" is not supported";
      } else {
        StringBuilder sb;
        sb.Append("The payment methods \"");
        sb.Append(*it);
        sb.Append("\"");
        while (++it != method_names_.end()) {
          sb.Append(", \"");
          sb.Append(*it);
          sb.Append("\"");
        }
        sb.Append(" are not supported");
        message = sb.ToString();
      }
      break;
    }

    case PaymentErrorReason::ALREADY_SHOWING: {
      exception_code = DOMExceptionCode::kAbortError;
      message =
          "Another PaymentRequest UI is already showing in a different tab or "
          "window";
      break;
    }

    case PaymentErrorReason::UNKNOWN: {
      exception_code = DOMExceptionCode::kUnknownError;
      message = "Request failed";
      break;
    }
  }

  DCHECK(!message.IsEmpty());

  // If the user closes PaymentRequest UI after PaymentResponse.complete() has
  // been called, the PaymentResponse.complete() promise should be resolved with
  // undefined instead of rejecting.
  if (complete_resolver_) {
    DCHECK(error == PaymentErrorReason::USER_CANCEL ||
           error == PaymentErrorReason::UNKNOWN);
    complete_resolver_->Resolve();
  }

  ScriptPromiseResolver* resolver = GetPendingAcceptPromiseResolver();
  if (resolver)
    resolver->Reject(DOMException::Create(exception_code, message));

  if (abort_resolver_)
    abort_resolver_->Reject(DOMException::Create(exception_code, message));

  if (can_make_payment_resolver_) {
    can_make_payment_resolver_->Reject(
        DOMException::Create(exception_code, message));
  }

  if (has_enrolled_instrument_resolver_) {
    has_enrolled_instrument_resolver_->Reject(
        DOMException::Create(exception_code, message));
  }

  ClearResolversAndCloseMojoConnection();
}

void PaymentRequest::OnComplete() {
  DCHECK(complete_resolver_);
  complete_resolver_->Resolve();
  ClearResolversAndCloseMojoConnection();
}

void PaymentRequest::OnAbort(bool aborted_successfully) {
  DCHECK(abort_resolver_);
  DCHECK(GetPendingAcceptPromiseResolver());

  if (!aborted_successfully) {
    abort_resolver_->Reject(DOMException::Create(
        DOMExceptionCode::kInvalidStateError, "Unable to abort the payment"));
    abort_resolver_.Clear();
    return;
  }

  ScriptPromiseResolver* resolver = GetPendingAcceptPromiseResolver();
  resolver->Reject(DOMException::Create(DOMExceptionCode::kAbortError,
                                        "The website has aborted the payment"));
  abort_resolver_->Resolve();
  ClearResolversAndCloseMojoConnection();
}

void PaymentRequest::OnCanMakePayment(CanMakePaymentQueryResult result) {
  // TODO(https://crbug.com/891371): Understand how the resolver could be null
  // here and prevent it.
  if (!can_make_payment_resolver_)
    return;

  switch (result) {
    case CanMakePaymentQueryResult::WARNING_CAN_MAKE_PAYMENT:
      WarnIgnoringQueryQuotaForCanMakePayment(*GetExecutionContext(),
                                              kCanMakePaymentDebugName);
      FALLTHROUGH;
    case CanMakePaymentQueryResult::CAN_MAKE_PAYMENT:
      can_make_payment_resolver_->Resolve(true);
      break;
    case CanMakePaymentQueryResult::WARNING_CANNOT_MAKE_PAYMENT:
      WarnIgnoringQueryQuotaForCanMakePayment(*GetExecutionContext(),
                                              kCanMakePaymentDebugName);
      FALLTHROUGH;
    case CanMakePaymentQueryResult::CANNOT_MAKE_PAYMENT:
      can_make_payment_resolver_->Resolve(false);
      break;
    case CanMakePaymentQueryResult::QUERY_QUOTA_EXCEEDED:
      can_make_payment_resolver_->Reject(DOMException::Create(
          DOMExceptionCode::kNotAllowedError,
          "Not allowed to check whether can make payment"));
      break;
  }

  can_make_payment_resolver_.Clear();
}

void PaymentRequest::OnHasEnrolledInstrument(
    HasEnrolledInstrumentQueryResult result) {
  // TODO(https://crbug.com/891371): Understand how the resolver could be null
  // here and prevent it.
  if (!has_enrolled_instrument_resolver_)
    return;

  switch (result) {
    case HasEnrolledInstrumentQueryResult::WARNING_HAS_ENROLLED_INSTRUMENT:
      WarnIgnoringQueryQuotaForCanMakePayment(*GetExecutionContext(),
                                              kHasEnrolledInstrumentDebugName);
      FALLTHROUGH;
    case HasEnrolledInstrumentQueryResult::HAS_ENROLLED_INSTRUMENT:
      has_enrolled_instrument_resolver_->Resolve(true);
      break;
    case HasEnrolledInstrumentQueryResult::WARNING_HAS_NO_ENROLLED_INSTRUMENT:
      WarnIgnoringQueryQuotaForCanMakePayment(*GetExecutionContext(),
                                              kHasEnrolledInstrumentDebugName);
      FALLTHROUGH;
    case HasEnrolledInstrumentQueryResult::HAS_NO_ENROLLED_INSTRUMENT:
      has_enrolled_instrument_resolver_->Resolve(false);
      break;
    case HasEnrolledInstrumentQueryResult::QUERY_QUOTA_EXCEEDED:
      has_enrolled_instrument_resolver_->Reject(DOMException::Create(
          DOMExceptionCode::kNotAllowedError,
          "Exceeded query quota for hasEnrolledInstrument"));
      break;
  }

  has_enrolled_instrument_resolver_.Clear();
}

void PaymentRequest::WarnNoFavicon() {
  GetExecutionContext()->AddConsoleMessage(
      ConsoleMessage::Create(mojom::ConsoleMessageSource::kJavaScript,
                             mojom::ConsoleMessageLevel::kWarning,
                             "Favicon not found for PaymentRequest UI. User "
                             "may not recognize the website."));
}

void PaymentRequest::OnCompleteTimeout(TimerBase*) {
  GetExecutionContext()->AddConsoleMessage(ConsoleMessage::Create(
      mojom::ConsoleMessageSource::kJavaScript,
      mojom::ConsoleMessageLevel::kError,
      "Timed out waiting for a PaymentResponse.complete() call."));
  payment_provider_->Complete(payments::mojom::blink::PaymentComplete(kFail));
  ClearResolversAndCloseMojoConnection();
}

void PaymentRequest::OnUpdatePaymentDetailsTimeout(TimerBase*) {
  OnUpdatePaymentDetailsFailure(
      is_waiting_for_show_promise_to_resolve_
          ? "Timed out waiting for a PaymentRequest.show(promise) to resolve."
          : "Timed out waiting for a "
            "PaymentRequestUpdateEvent.updateWith(promise) to resolve.");
}

void PaymentRequest::ClearResolversAndCloseMojoConnection() {
  complete_timer_.Stop();
  complete_resolver_.Clear();
  accept_resolver_.Clear();
  retry_resolver_.Clear();
  abort_resolver_.Clear();
  can_make_payment_resolver_.Clear();
  has_enrolled_instrument_resolver_.Clear();
  if (client_binding_.is_bound())
    client_binding_.Close();
  payment_provider_.reset();
}

ScriptPromiseResolver* PaymentRequest::GetPendingAcceptPromiseResolver() const {
  return retry_resolver_ ? retry_resolver_.Get() : accept_resolver_.Get();
}

void PaymentRequest::DispatchPaymentRequestUpdateEvent(
    EventTarget* event_target,
    PaymentRequestUpdateEvent* event) {
  event->SetTarget(event_target);
  event->SetPaymentRequest(this);

  // If the website does not calculate the updated shopping cart contents
  // within 60 seconds, abort payment.
  update_payment_details_timer_.StartOneShot(TimeDelta::FromSeconds(60),
                                             FROM_HERE);

  event_target->DispatchEvent(*event);
  if (!event->is_waiting_for_update()) {
    // DispatchEvent runs synchronously. The method is_waiting_for_update()
    // returns false if the merchant did not call event.updateWith() within the
    // event handler, which is optional, so the renderer sends a message to the
    // browser to re-enable UI interactions.
    const String& message = String::Format(
        "No updateWith() call in '%s' event handler. User may see outdated "
        "line items and total.",
        event->type().Ascii().data());
    GetExecutionContext()->AddConsoleMessage(
        ConsoleMessage::Create(mojom::ConsoleMessageSource::kJavaScript,
                               mojom::ConsoleMessageLevel::kWarning, message));
    payment_provider_->NoUpdatedPaymentDetails();
    // Make sure that updateWith() is only allowed to be called within the same
    // event loop as the event dispatch. See
    // https://w3c.github.io/payment-request/#paymentrequest-updated-algorithm
    event->start_waiting_for_update(true);
  }
}

}  // namespace blink
