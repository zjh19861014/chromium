// Copyright 2016 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/ui/webui/settings/chromeos/cups_printers_handler.h"

#include <utility>

#include "base/bind.h"
#include "base/bind_helpers.h"
#include "base/containers/flat_map.h"
#include "base/files/file_util.h"
#include "base/json/json_string_value_serializer.h"
#include "base/logging.h"
#include "base/metrics/histogram_macros.h"
#include "base/path_service.h"
#include "base/strings/string_util.h"
#include "base/strings/stringprintf.h"
#include "base/task/post_task.h"
#include "base/threading/sequenced_task_runner_handle.h"
#include "base/values.h"
#include "chrome/browser/browser_process.h"
#include "chrome/browser/chromeos/printing/cups_printers_manager.h"
#include "chrome/browser/chromeos/printing/cups_printers_manager_factory.h"
#include "chrome/browser/chromeos/printing/ppd_provider_factory.h"
#include "chrome/browser/chromeos/printing/printer_configurer.h"
#include "chrome/browser/chromeos/printing/printer_event_tracker.h"
#include "chrome/browser/chromeos/printing/printer_event_tracker_factory.h"
#include "chrome/browser/chromeos/printing/printer_info.h"
#include "chrome/browser/download/download_prefs.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser_finder.h"
#include "chrome/browser/ui/browser_window.h"
#include "chrome/browser/ui/chrome_select_file_policy.h"
#include "chrome/common/chrome_paths.h"
#include "chrome/common/pref_names.h"
#include "chromeos/dbus/dbus_thread_manager.h"
#include "chromeos/dbus/debug_daemon_client.h"
#include "chromeos/printing/ppd_cache.h"
#include "chromeos/printing/ppd_line_reader.h"
#include "chromeos/printing/ppd_provider.h"
#include "chromeos/printing/printer_configuration.h"
#include "chromeos/printing/printer_translator.h"
#include "chromeos/printing/printing_constants.h"
#include "chromeos/printing/uri_components.h"
#include "components/device_event_log/device_event_log.h"
#include "content/public/browser/browser_context.h"
#include "content/public/browser/browser_thread.h"
#include "content/public/browser/web_ui.h"
#include "google_apis/google_api_keys.h"
#include "net/base/filename_util.h"
#include "net/base/ip_endpoint.h"
#include "net/url_request/url_request_context_getter.h"
#include "printing/backend/print_backend.h"

namespace chromeos {
namespace settings {

namespace {

// These values are written to logs.  New enum values can be added, but existing
// enums must never be renumbered or deleted and reused.
enum PpdSourceForHistogram { kUser = 0, kScs = 1, kPpdSourceMax };

constexpr int kPpdMaxLineLength = 255;

void RecordPpdSource(const PpdSourceForHistogram& source) {
  UMA_HISTOGRAM_ENUMERATION("Printing.CUPS.PpdSource", source, kPpdSourceMax);
}

void OnRemovedPrinter(const Printer::PrinterProtocol& protocol, bool success) {
  if (success) {
    PRINTER_LOG(DEBUG) << "Printer removal succeeded.";
  } else {
    PRINTER_LOG(DEBUG) << "Printer removal failed.";
  }

  UMA_HISTOGRAM_ENUMERATION("Printing.CUPS.PrinterRemoved", protocol,
                            Printer::PrinterProtocol::kProtocolMax);
}

// Log if the IPP attributes request was succesful.
void RecordIppQuerySuccess(bool success) {
  UMA_HISTOGRAM_BOOLEAN("Printing.CUPS.IppAttributesSuccess", success);
}

// Returns true if |printer_uri| is an IPP uri.
bool IsIppUri(base::StringPiece printer_uri) {
  base::StringPiece::size_type separator_location =
      printer_uri.find(url::kStandardSchemeSeparator);
  if (separator_location == base::StringPiece::npos) {
    return false;
  }

  base::StringPiece scheme_part = printer_uri.substr(0, separator_location);
  return scheme_part == kIppScheme || scheme_part == kIppsScheme;
}

// Query an IPP printer to check for autoconf support where the printer is
// located at |printer_uri|.  Results are reported through |callback|.  It is an
// error to attempt this with a non-IPP printer.
void QueryAutoconf(const std::string& printer_uri,
                   const PrinterInfoCallback& callback) {
  auto optional = ParseUri(printer_uri);
  // Behavior for querying a non-IPP uri is undefined and disallowed.
  if (!IsIppUri(printer_uri) || !optional.has_value()) {
    PRINTER_LOG(ERROR) << "Printer uri is invalid: " << printer_uri;
    callback.Run(false, "", "", "", {}, false);
    return;
  }

  UriComponents uri = optional.value();
  QueryIppPrinter(uri.host(), uri.port(), uri.path(), uri.encrypted(),
                  callback);
}

// Returns the list of |printers| formatted as a CupsPrintersList.
base::Value BuildCupsPrintersList(const std::vector<Printer>& printers) {
  base::Value printers_list(base::Value::Type::LIST);
  for (const Printer& printer : printers) {
    // Some of these printers could be invalid but we want to allow the user
    // to edit them. crbug.com/778383
    printers_list.GetList().push_back(
        base::Value::FromUniquePtrValue(GetCupsPrinterInfo(printer)));
  }

  base::Value response(base::Value::Type::DICTIONARY);
  response.SetKey("printerList", std::move(printers_list));
  return response;
}

// Extracts a sanitized value of printerQueue from |printer_dict|.  Returns an
// empty string if the value was not present in the dictionary.
std::string GetPrinterQueue(const base::DictionaryValue& printer_dict) {
  std::string queue;
  if (!printer_dict.GetString("printerQueue", &queue)) {
    return queue;
  }

  if (!queue.empty() && queue[0] == '/') {
    // Strip the leading backslash. It is expected that this results in an
    // empty string if the input is just a backslash.
    queue = queue.substr(1);
  }

  return queue;
}

// Generates a Printer from |printer_dict| where |printer_dict| is a
// CupsPrinterInfo representation.  If any of the required fields are missing,
// returns nullptr.
std::unique_ptr<chromeos::Printer> DictToPrinter(
    const base::DictionaryValue& printer_dict) {
  std::string printer_id;
  std::string printer_name;
  std::string printer_description;
  std::string printer_manufacturer;
  std::string printer_model;
  std::string printer_make_and_model;
  std::string printer_address;
  std::string printer_protocol;

  if (!printer_dict.GetString("printerId", &printer_id) ||
      !printer_dict.GetString("printerName", &printer_name) ||
      !printer_dict.GetString("printerDescription", &printer_description) ||
      !printer_dict.GetString("printerManufacturer", &printer_manufacturer) ||
      !printer_dict.GetString("printerModel", &printer_model) ||
      !printer_dict.GetString("printerMakeAndModel", &printer_make_and_model) ||
      !printer_dict.GetString("printerAddress", &printer_address) ||
      !printer_dict.GetString("printerProtocol", &printer_protocol)) {
    return nullptr;
  }

  std::string printer_queue = GetPrinterQueue(printer_dict);

  std::string printer_uri =
      printer_protocol + url::kStandardSchemeSeparator + printer_address;
  if (!printer_queue.empty()) {
    printer_uri += "/" + printer_queue;
  }

  auto printer = std::make_unique<chromeos::Printer>(printer_id);
  printer->set_display_name(printer_name);
  printer->set_description(printer_description);
  printer->set_manufacturer(printer_manufacturer);
  printer->set_model(printer_model);
  printer->set_make_and_model(printer_make_and_model);
  printer->set_uri(printer_uri);

  return printer;
}

std::string ReadFileToStringWithMaxSize(const base::FilePath& path,
                                        int max_size) {
  std::string contents;
  // This call can fail, but it doesn't matter for our purposes. If it fails,
  // we simply return an empty string for the contents, and it will be rejected
  // as an invalid PPD.
  base::ReadFileToStringWithMaxSize(path, &contents, max_size);
  return contents;
}

// Determines whether changing the URI in |existing_printer| to the URI in
// |new_printer| would be valid. Network printers are not allowed to change
// their protocol to a non-network protocol, but can change anything else.
// Non-network printers are not allowed to change anything in their URI.
bool IsValidUriChange(const Printer& existing_printer,
                      const Printer& new_printer) {
  if (new_printer.GetProtocol() == Printer::PrinterProtocol::kUnknown) {
    return false;
  }
  if (existing_printer.HasNetworkProtocol()) {
    return new_printer.HasNetworkProtocol();
  }
  return existing_printer.uri() == new_printer.uri();
}

// Assumes |info| is a dictionary.
void SetPpdReference(const Printer::PpdReference& ppd_ref, base::Value* info) {
  if (!ppd_ref.user_supplied_ppd_url.empty()) {
    info->SetKey("ppdRefUserSuppliedPpdUrl",
                 base::Value(ppd_ref.user_supplied_ppd_url));
  } else if (!ppd_ref.effective_make_and_model.empty()) {
    info->SetKey("ppdRefEffectiveMakeAndModel",
                 base::Value(ppd_ref.effective_make_and_model));
  } else {  // Must be autoconf, shouldn't be possible
    NOTREACHED() << "Succeeded in PPD matching without emm";
  }
}

Printer::PpdReference GetPpdReference(const base::Value* info) {
  const char ppd_ref_pathname[] = "printerPpdReference";
  auto* user_supplied_ppd_url =
      info->FindPath({ppd_ref_pathname, "userSuppliedPPDUrl"});
  auto* effective_make_and_model =
      info->FindPath({ppd_ref_pathname, "effectiveMakeAndModel"});
  auto* autoconf = info->FindPath({ppd_ref_pathname, "autoconf"});

  if (user_supplied_ppd_url != nullptr) {
    DCHECK(!effective_make_and_model && !autoconf);
    return Printer::PpdReference{user_supplied_ppd_url->GetString(), "", false};
  }

  if (effective_make_and_model != nullptr) {
    DCHECK(!user_supplied_ppd_url && !autoconf);
    return Printer::PpdReference{"", effective_make_and_model->GetString(),
                                 false};
  }

  // Otherwise it must be autoconf
  DCHECK(autoconf && autoconf->GetBool());
  return Printer::PpdReference{"", "", true};
}

}  // namespace

CupsPrintersHandler::CupsPrintersHandler(
    Profile* profile,
    scoped_refptr<PpdProvider> ppd_provider,
    std::unique_ptr<PrinterConfigurer> printer_configurer,
    CupsPrintersManager* printers_manager)
    : profile_(profile),
      ppd_provider_(ppd_provider),
      printer_configurer_(std::move(printer_configurer)),
      printers_manager_(printers_manager),
      printers_manager_observer_(this),
      weak_factory_(this) {}

// static
std::unique_ptr<CupsPrintersHandler> CupsPrintersHandler::Create(
    content::WebUI* webui) {
  Profile* profile(Profile::FromWebUI(webui));
  auto ppd_provider = CreatePpdProvider(profile);
  auto printer_configurer = PrinterConfigurer::Create(profile);
  CupsPrintersManager* printers_manager =
      CupsPrintersManagerFactory::GetForBrowserContext(profile);
  // Using 'new' to access non-public constructor.
  return base::WrapUnique(new CupsPrintersHandler(
      profile, ppd_provider, std::move(printer_configurer), printers_manager));
}

// static
std::unique_ptr<CupsPrintersHandler> CupsPrintersHandler::CreateForTesting(
    Profile* profile,
    scoped_refptr<PpdProvider> ppd_provider,
    std::unique_ptr<PrinterConfigurer> printer_configurer,
    CupsPrintersManager* printers_manager) {
  // Using 'new' to access non-public constructor.
  return base::WrapUnique(new CupsPrintersHandler(
      profile, ppd_provider, std::move(printer_configurer), printers_manager));
}

CupsPrintersHandler::~CupsPrintersHandler() = default;

void CupsPrintersHandler::RegisterMessages() {
  web_ui()->RegisterMessageCallback(
      "getCupsPrintersList",
      base::BindRepeating(&CupsPrintersHandler::HandleGetCupsPrintersList,
                          base::Unretained(this)));
  web_ui()->RegisterMessageCallback(
      "updateCupsPrinter",
      base::BindRepeating(&CupsPrintersHandler::HandleUpdateCupsPrinter,
                          base::Unretained(this)));
  web_ui()->RegisterMessageCallback(
      "removeCupsPrinter",
      base::BindRepeating(&CupsPrintersHandler::HandleRemoveCupsPrinter,
                          base::Unretained(this)));
  web_ui()->RegisterMessageCallback(
      "addCupsPrinter",
      base::BindRepeating(&CupsPrintersHandler::HandleAddCupsPrinter,
                          base::Unretained(this)));
  web_ui()->RegisterMessageCallback(
      "reconfigureCupsPrinter",
      base::BindRepeating(&CupsPrintersHandler::HandleReconfigureCupsPrinter,
                          base::Unretained(this)));
  web_ui()->RegisterMessageCallback(
      "getPrinterInfo",
      base::BindRepeating(&CupsPrintersHandler::HandleGetPrinterInfo,
                          base::Unretained(this)));
  web_ui()->RegisterMessageCallback(
      "getCupsPrinterManufacturersList",
      base::BindRepeating(
          &CupsPrintersHandler::HandleGetCupsPrinterManufacturers,
          base::Unretained(this)));
  web_ui()->RegisterMessageCallback(
      "getCupsPrinterModelsList",
      base::BindRepeating(&CupsPrintersHandler::HandleGetCupsPrinterModels,
                          base::Unretained(this)));
  web_ui()->RegisterMessageCallback(
      "selectPPDFile",
      base::BindRepeating(&CupsPrintersHandler::HandleSelectPPDFile,
                          base::Unretained(this)));
  web_ui()->RegisterMessageCallback(
      "startDiscoveringPrinters",
      base::BindRepeating(&CupsPrintersHandler::HandleStartDiscovery,
                          base::Unretained(this)));
  web_ui()->RegisterMessageCallback(
      "stopDiscoveringPrinters",
      base::BindRepeating(&CupsPrintersHandler::HandleStopDiscovery,
                          base::Unretained(this)));
  web_ui()->RegisterMessageCallback(
      "getPrinterPpdManufacturerAndModel",
      base::BindRepeating(
          &CupsPrintersHandler::HandleGetPrinterPpdManufacturerAndModel,
          base::Unretained(this)));
  web_ui()->RegisterMessageCallback(
      "addDiscoveredPrinter",
      base::BindRepeating(&CupsPrintersHandler::HandleAddDiscoveredPrinter,
                          base::Unretained(this)));
  web_ui()->RegisterMessageCallback(
      "cancelPrinterSetUp",
      base::BindRepeating(&CupsPrintersHandler::HandleSetUpCancel,
                          base::Unretained(this)));
}

void CupsPrintersHandler::OnJavascriptAllowed() {
  if (!printers_manager_observer_.IsObservingSources()) {
    printers_manager_observer_.Add(printers_manager_);
  }
}

void CupsPrintersHandler::OnJavascriptDisallowed() {
  printers_manager_observer_.RemoveAll();
}

void CupsPrintersHandler::SetWebUIForTest(content::WebUI* web_ui) {
  set_web_ui(web_ui);
}

void CupsPrintersHandler::HandleGetCupsPrintersList(
    const base::ListValue* args) {
  AllowJavascript();

  CHECK_EQ(1U, args->GetSize());
  std::string callback_id;
  CHECK(args->GetString(0, &callback_id));

  std::vector<Printer> printers =
      printers_manager_->GetPrinters(CupsPrintersManager::kConfigured);

  auto response = BuildCupsPrintersList(printers);
  ResolveJavascriptCallback(base::Value(callback_id), response);
}

void CupsPrintersHandler::HandleUpdateCupsPrinter(const base::ListValue* args) {
  std::string printer_id;
  std::string printer_name;
  CHECK(args->GetString(0, &printer_id));
  CHECK(args->GetString(1, &printer_name));

  Printer printer(printer_id);
  printer.set_display_name(printer_name);

  if (!profile_->GetPrefs()->GetBoolean(prefs::kUserNativePrintersAllowed)) {
    PRINTER_LOG(DEBUG) << "HandleUpdateCupsPrinter() called when "
                          "kUserNativePrintersAllowed is set to false";
    // Used to log UMA metrics.
    OnAddedOrEditedPrinterCommon(
        printer, PrinterSetupResult::kNativePrintersNotAllowed, false);
    // Used to fire the web UI listener.
    OnAddOrEditPrinterError(PrinterSetupResult::kNativePrintersNotAllowed);
    return;
  }

  PRINTER_LOG(USER) << "Comitting printer update";
  printers_manager_->UpdateConfiguredPrinter(printer);

  // TODO(xdai): Replace "on-add-or-edit-cups-printer" callback with Promise
  // resolve function.
  FireWebUIListener("on-add-or-edit-cups-printer",
                    base::Value(PrinterSetupResult::kEditSuccess),
                    base::Value(printer_name));
}

void CupsPrintersHandler::HandleRemoveCupsPrinter(const base::ListValue* args) {
  PRINTER_LOG(USER) << "Removing printer";
  std::string printer_id;
  std::string printer_name;
  CHECK(args->GetString(0, &printer_id));
  CHECK(args->GetString(1, &printer_name));
  auto printer = printers_manager_->GetPrinter(printer_id);
  if (!printer)
    return;

  // Record removal before the printer is deleted.
  PrinterEventTrackerFactory::GetForBrowserContext(profile_)
      ->RecordPrinterRemoved(*printer);

  Printer::PrinterProtocol protocol = printer->GetProtocol();
  // Printer is deleted here.  Do not access after this line.
  printers_manager_->RemoveConfiguredPrinter(printer_id);

  DebugDaemonClient* client = DBusThreadManager::Get()->GetDebugDaemonClient();
  client->CupsRemovePrinter(printer_id,
                            base::BindOnce(&OnRemovedPrinter, protocol),
                            base::DoNothing());
}

void CupsPrintersHandler::HandleGetPrinterInfo(const base::ListValue* args) {
  DCHECK(args);
  std::string callback_id;
  if (!args->GetString(0, &callback_id)) {
    NOTREACHED() << "Expected request for a promise";
    return;
  }

  const base::DictionaryValue* printer_dict = nullptr;
  if (!args->GetDictionary(1, &printer_dict)) {
    NOTREACHED() << "Dictionary missing";
    return;
  }

  AllowJavascript();

  std::string printer_address;
  if (!printer_dict->GetString("printerAddress", &printer_address)) {
    NOTREACHED() << "Address missing";
    return;
  }

  if (printer_address.empty()) {
    // Run the failure callback.
    OnAutoconfQueried(callback_id, false, "", "", "", {}, false);
    return;
  }

  std::string printer_queue = GetPrinterQueue(*printer_dict);

  std::string printer_protocol;
  if (!printer_dict->GetString("printerProtocol", &printer_protocol)) {
    NOTREACHED() << "Protocol missing";
    return;
  }

  DCHECK(printer_protocol == kIppScheme || printer_protocol == kIppsScheme)
      << "Printer info requests only supported for IPP and IPPS printers";
  PRINTER_LOG(DEBUG) << "Querying printer info";
  std::string printer_uri =
      base::StringPrintf("%s://%s/%s", printer_protocol.c_str(),
                         printer_address.c_str(), printer_queue.c_str());
  QueryAutoconf(printer_uri,
                base::Bind(&CupsPrintersHandler::OnAutoconfQueried,
                           weak_factory_.GetWeakPtr(), callback_id));
}

void CupsPrintersHandler::OnAutoconfQueriedDiscovered(
    std::unique_ptr<Printer> printer,
    bool success,
    const std::string& make,
    const std::string& model,
    const std::string& make_and_model,
    const std::vector<std::string>& document_formats,
    bool ipp_everywhere) {
  RecordIppQuerySuccess(success);

  if (success) {
    // If we queried a valid make and model, use it.  The mDNS record isn't
    // guaranteed to have it.  However, don't overwrite it if the printer
    // advertises an empty value through printer-make-and-model.
    if (!make_and_model.empty()) {
      // manufacturer and model are set with make_and_model because they are
      // derived from make_and_model for compatability and are slated for
      // removal.
      printer->set_manufacturer(make);
      printer->set_model(model);
      printer->set_make_and_model(make_and_model);
      PRINTER_LOG(DEBUG) << "Printer queried for make and model "
                         << make_and_model;
    }

    // Autoconfig available, use it.
    if (ipp_everywhere) {
      PRINTER_LOG(DEBUG) << "Performing autoconf setup";
      printer->mutable_ppd_reference()->autoconf = true;
      printer_configurer_->SetUpPrinter(
          *printer, base::Bind(&CupsPrintersHandler::OnAddedDiscoveredPrinter,
                               weak_factory_.GetWeakPtr(), *printer));
      return;
    }
  }

  // We don't have enough from discovery to configure the printer.  Fill in as
  // much information as we can about the printer, and ask the user to supply
  // the rest.
  PRINTER_LOG(EVENT) << "Could not query printer.  Fallback to asking the user";
  FireManuallyAddDiscoveredPrinter(*printer);
}

void CupsPrintersHandler::OnAutoconfQueried(
    const std::string& callback_id,
    bool success,
    const std::string& make,
    const std::string& model,
    const std::string& make_and_model,
    const std::vector<std::string>& document_formats,
    bool ipp_everywhere) {
  RecordIppQuerySuccess(success);

  if (!success) {
    PRINTER_LOG(DEBUG) << "Could not query printer";
    base::DictionaryValue reject;
    reject.SetString("message", "Querying printer failed");
    RejectJavascriptCallback(base::Value(callback_id), reject);
    return;
  }

  PRINTER_LOG(DEBUG) << "Resolved printer information: make_and_model("
                     << make_and_model << ") autoconf(" << ipp_everywhere
                     << ")";

  // Bundle printer metadata
  base::Value info(base::Value::Type::DICTIONARY);
  info.SetKey("manufacturer", base::Value(make));
  info.SetKey("model", base::Value(model));
  info.SetKey("makeAndModel", base::Value(make_and_model));
  info.SetKey("autoconf", base::Value(ipp_everywhere));

  if (ipp_everywhere) {
    info.SetKey("ppdReferenceResolved", base::Value(true));
    ResolveJavascriptCallback(base::Value(callback_id), info);
    return;
  }

  PrinterSearchData ppd_search_data;
  ppd_search_data.discovery_type =
      PrinterSearchData::PrinterDiscoveryType::kManual;
  ppd_search_data.make_and_model.push_back(make_and_model);
  ppd_search_data.supported_document_formats = document_formats;

  // Try to resolve the PPD matching.
  ppd_provider_->ResolvePpdReference(
      ppd_search_data,
      base::BindOnce(&CupsPrintersHandler::OnPpdResolved,
                     weak_factory_.GetWeakPtr(), callback_id, std::move(info)));
}

void CupsPrintersHandler::OnPpdResolved(const std::string& callback_id,
                                        base::Value info,
                                        PpdProvider::CallbackResultCode res,
                                        const Printer::PpdReference& ppd_ref) {
  if (res != PpdProvider::CallbackResultCode::SUCCESS) {
    info.SetKey("ppdReferenceResolved", base::Value(false));
    ResolveJavascriptCallback(base::Value(callback_id), info);
    return;
  }

  SetPpdReference(ppd_ref, &info);
  info.SetKey("ppdReferenceResolved", base::Value(true));
  ResolveJavascriptCallback(base::Value(callback_id), info);
}

void CupsPrintersHandler::HandleAddCupsPrinter(const base::ListValue* args) {
  AllowJavascript();
  AddOrReconfigurePrinter(args, false /* is_printer_edit */);
}

void CupsPrintersHandler::HandleReconfigureCupsPrinter(
    const base::ListValue* args) {
  AllowJavascript();
  AddOrReconfigurePrinter(args, true /* is_printer_edit */);
}

void CupsPrintersHandler::AddOrReconfigurePrinter(const base::ListValue* args,
                                                  bool is_printer_edit) {
  const base::DictionaryValue* printer_dict = nullptr;
  CHECK(args->GetDictionary(0, &printer_dict));

  std::unique_ptr<Printer> printer = DictToPrinter(*printer_dict);
  if (!printer) {
    PRINTER_LOG(ERROR) << "Failed to parse printer URI";
    OnAddOrEditPrinterError(PrinterSetupResult::kFatalError);
    return;
  }

  if (!profile_->GetPrefs()->GetBoolean(prefs::kUserNativePrintersAllowed)) {
    PRINTER_LOG(DEBUG) << "AddOrReconfigurePrinter() called when "
                          "kUserNativePrintersAllowed is set to false";
    // Used to log UMA metrics.
    OnAddedOrEditedPrinterCommon(
        *printer, PrinterSetupResult::kNativePrintersNotAllowed, false);
    // Used to fire the web UI listener.
    OnAddOrEditPrinterError(PrinterSetupResult::kNativePrintersNotAllowed);
    return;
  }

  if (!printer->GetUriComponents().has_value()) {
    // If the returned optional does not contain a value then it means that the
    // printer's uri was not able to be parsed successfully.
    PRINTER_LOG(ERROR) << "Failed to parse printer URI";
    OnAddOrEditPrinterError(PrinterSetupResult::kFatalError);
    return;
  }

  // Grab the existing printer object and check that we are not making any
  // changes that will make |existing_printer_object| unusable.
  if (printer->id().empty()) {
    // If the printer object has not already been created, error out since this
    // is not a valid case.
    PRINTER_LOG(ERROR) << "Failed to parse printer ID";
    OnAddOrEditPrinterError(PrinterSetupResult::kFatalError);
    return;
  }

  std::unique_ptr<Printer> existing_printer_object =
      printers_manager_->GetPrinter(printer->id());
  if (existing_printer_object) {
    if (!IsValidUriChange(*existing_printer_object, *printer)) {
      OnAddOrEditPrinterError(PrinterSetupResult::kInvalidPrinterUpdate);
      return;
    }
  }

  // Read PPD selection if it was used.
  std::string ppd_manufacturer;
  std::string ppd_model;
  printer_dict->GetString("ppdManufacturer", &ppd_manufacturer);
  printer_dict->GetString("ppdModel", &ppd_model);

  // Read user provided PPD if it was used.
  std::string printer_ppd_path;
  printer_dict->GetString("printerPPDPath", &printer_ppd_path);

  // Checks whether a resolved PPD Reference is available.
  bool ppd_ref_resolved = false;
  printer_dict->GetBoolean("printerPpdReferenceResolved", &ppd_ref_resolved);

  // Verify that the printer is autoconf or a valid ppd path is present.
  if (ppd_ref_resolved) {
    *printer->mutable_ppd_reference() = GetPpdReference(printer_dict);
  } else if (!printer_ppd_path.empty()) {
    RecordPpdSource(kUser);
    GURL tmp = net::FilePathToFileURL(base::FilePath(printer_ppd_path));
    if (!tmp.is_valid()) {
      LOG(ERROR) << "Invalid ppd path: " << printer_ppd_path;
      OnAddOrEditPrinterError(PrinterSetupResult::kInvalidPpd);
      return;
    }
    printer->mutable_ppd_reference()->user_supplied_ppd_url = tmp.spec();
  } else if (!ppd_manufacturer.empty() && !ppd_model.empty()) {
    RecordPpdSource(kScs);
    // Pull out the ppd reference associated with the selected manufacturer and
    // model.
    bool found = false;
    for (const auto& resolved_printer : resolved_printers_[ppd_manufacturer]) {
      if (resolved_printer.name == ppd_model) {
        *printer->mutable_ppd_reference() = resolved_printer.ppd_ref;
        found = true;
        break;
      }
    }
    if (!found) {
      LOG(ERROR) << "Failed to get ppd reference";
      OnAddOrEditPrinterError(PrinterSetupResult::kPpdNotFound);
      return;
    }

    if (printer->make_and_model().empty()) {
      // In lieu of more accurate information, populate the make and model
      // fields with the PPD information.
      printer->set_manufacturer(ppd_manufacturer);
      printer->set_model(ppd_model);
      // PPD Model names are actually make and model.
      printer->set_make_and_model(ppd_model);
    }
  } else {
    // TODO(https://crbug.com/738514): Support PPD guessing for non-autoconf
    // printers. i.e. !autoconf && !manufacturer.empty() && !model.empty()
    NOTREACHED()
        << "A configuration option must have been selected to add a printer";
  }

  printer_configurer_->SetUpPrinter(
      *printer,
      base::BindOnce(&CupsPrintersHandler::OnAddedOrEditedSpecifiedPrinter,
                     weak_factory_.GetWeakPtr(), *printer, is_printer_edit));
}

void CupsPrintersHandler::OnAddedOrEditedPrinterCommon(
    const Printer& printer,
    PrinterSetupResult result_code,
    bool is_automatic) {
  UMA_HISTOGRAM_ENUMERATION("Printing.CUPS.PrinterSetupResult", result_code,
                            PrinterSetupResult::kMaxValue);
  switch (result_code) {
    case PrinterSetupResult::kSuccess:
      UMA_HISTOGRAM_ENUMERATION("Printing.CUPS.PrinterAdded",
                                printer.GetProtocol(), Printer::kProtocolMax);
      PRINTER_LOG(USER) << "Performing printer setup";
      printers_manager_->PrinterInstalled(printer, is_automatic);
      return;
    case PrinterSetupResult::kEditSuccess:
      PRINTER_LOG(USER) << "Printer updated";
      printers_manager_->UpdateConfiguredPrinter(printer);
      return;
    case PrinterSetupResult::kPpdNotFound:
      PRINTER_LOG(ERROR) << "Could not locate requested PPD";
      break;
    case PrinterSetupResult::kPpdTooLarge:
      PRINTER_LOG(ERROR) << "PPD is too large";
      break;
    case PrinterSetupResult::kPpdUnretrievable:
      PRINTER_LOG(ERROR) << "Could not retrieve PPD from server";
      break;
    case PrinterSetupResult::kInvalidPpd:
      PRINTER_LOG(ERROR) << "Provided PPD is invalid.";
      break;
    case PrinterSetupResult::kPrinterUnreachable:
      PRINTER_LOG(ERROR) << "Could not contact printer for configuration";
      break;
    case PrinterSetupResult::kComponentUnavailable:
      LOG(WARNING) << "Could not install component";
      break;
    case PrinterSetupResult::kDbusError:
    case PrinterSetupResult::kFatalError:
      PRINTER_LOG(ERROR) << "Unrecoverable error.  Reboot required.";
      break;
    case PrinterSetupResult::kNativePrintersNotAllowed:
      PRINTER_LOG(ERROR)
          << "Unable to add or edit printer due to enterprise policy.";
      break;
    case PrinterSetupResult::kInvalidPrinterUpdate:
      PRINTER_LOG(ERROR)
          << "Requested printer changes would make printer unusable";
      break;
    case PrinterSetupResult::kDbusNoReply:
      PRINTER_LOG(ERROR) << "Couldn't talk to debugd over D-Bus.";
      break;
    case PrinterSetupResult::kDbusTimeout:
      PRINTER_LOG(ERROR) << "Timed out trying to reach debugd over D-Bus.";
      break;
    case PrinterSetupResult::kMaxValue:
      NOTREACHED() << "This is not an expected value";
      break;
  }
  // Log an event that tells us this printer setup failed, so we can get
  // statistics about which printers are giving users difficulty.
  printers_manager_->RecordSetupAbandoned(printer);
}

void CupsPrintersHandler::OnAddedDiscoveredPrinter(
    const Printer& printer,
    PrinterSetupResult result_code) {
  OnAddedOrEditedPrinterCommon(printer, result_code, true);
  if (result_code == PrinterSetupResult::kSuccess) {
    FireWebUIListener("on-add-or-edit-cups-printer", base::Value(result_code),
                      base::Value(printer.display_name()));
  } else {
    PRINTER_LOG(EVENT) << "Automatic setup failed for discovered printer.  "
                          "Fall back to manual.";
    // Could not set up printer.  Asking user for manufacturer data.
    FireManuallyAddDiscoveredPrinter(printer);
  }
}

void CupsPrintersHandler::OnAddedOrEditedSpecifiedPrinter(
    const Printer& printer,
    bool is_printer_edit,
    PrinterSetupResult result_code) {
  if (is_printer_edit && result_code == PrinterSetupResult::kSuccess) {
    result_code = PrinterSetupResult::kEditSuccess;
  }
  PRINTER_LOG(EVENT) << "Add/Update manual printer: " << result_code;
  OnAddedOrEditedPrinterCommon(printer, result_code, false);
  FireWebUIListener("on-add-or-edit-cups-printer", base::Value(result_code),
                    base::Value(printer.display_name()));
}

void CupsPrintersHandler::OnAddOrEditPrinterError(
    PrinterSetupResult result_code) {
  PRINTER_LOG(EVENT) << "Add printer error: " << result_code;
  FireWebUIListener("on-add-or-edit-cups-printer", base::Value(result_code),
                    base::Value(""));
}

void CupsPrintersHandler::HandleGetCupsPrinterManufacturers(
    const base::ListValue* args) {
  AllowJavascript();
  std::string js_callback;
  CHECK_EQ(1U, args->GetSize());
  CHECK(args->GetString(0, &js_callback));
  ppd_provider_->ResolveManufacturers(
      base::Bind(&CupsPrintersHandler::ResolveManufacturersDone,
                 weak_factory_.GetWeakPtr(), js_callback));
}

void CupsPrintersHandler::HandleGetCupsPrinterModels(
    const base::ListValue* args) {
  AllowJavascript();
  std::string js_callback;
  std::string manufacturer;
  CHECK_EQ(2U, args->GetSize());
  CHECK(args->GetString(0, &js_callback));
  CHECK(args->GetString(1, &manufacturer));

  // Empty manufacturer queries may be triggered as a part of the ui
  // initialization, and should just return empty results.
  if (manufacturer.empty()) {
    base::DictionaryValue response;
    response.SetBoolean("success", true);
    response.Set("models", std::make_unique<base::ListValue>());
    ResolveJavascriptCallback(base::Value(js_callback), response);
    return;
  }

  ppd_provider_->ResolvePrinters(
      manufacturer,
      base::Bind(&CupsPrintersHandler::ResolvePrintersDone,
                 weak_factory_.GetWeakPtr(), manufacturer, js_callback));
}

void CupsPrintersHandler::HandleSelectPPDFile(const base::ListValue* args) {
  CHECK_EQ(1U, args->GetSize());
  CHECK(args->GetString(0, &webui_callback_id_));

  base::FilePath downloads_path =
      DownloadPrefs::FromDownloadManager(
          content::BrowserContext::GetDownloadManager(profile_))
          ->DownloadPath();

  select_file_dialog_ = ui::SelectFileDialog::Create(
      this,
      std::make_unique<ChromeSelectFilePolicy>(web_ui()->GetWebContents()));
  gfx::NativeWindow owning_window =
      chrome::FindBrowserWithWebContents(web_ui()->GetWebContents())
          ->window()
          ->GetNativeWindow();
  select_file_dialog_->SelectFile(
      ui::SelectFileDialog::SELECT_OPEN_FILE, base::string16(), downloads_path,
      nullptr, 0, FILE_PATH_LITERAL(""), owning_window, nullptr);
}

void CupsPrintersHandler::ResolveManufacturersDone(
    const std::string& js_callback,
    PpdProvider::CallbackResultCode result_code,
    const std::vector<std::string>& manufacturers) {
  auto manufacturers_value = std::make_unique<base::ListValue>();
  if (result_code == PpdProvider::SUCCESS) {
    manufacturers_value->AppendStrings(manufacturers);
  }
  base::DictionaryValue response;
  response.SetBoolean("success", result_code == PpdProvider::SUCCESS);
  response.Set("manufacturers", std::move(manufacturers_value));
  ResolveJavascriptCallback(base::Value(js_callback), response);
}

void CupsPrintersHandler::ResolvePrintersDone(
    const std::string& manufacturer,
    const std::string& js_callback,
    PpdProvider::CallbackResultCode result_code,
    const PpdProvider::ResolvedPrintersList& printers) {
  auto printers_value = std::make_unique<base::ListValue>();
  if (result_code == PpdProvider::SUCCESS) {
    resolved_printers_[manufacturer] = printers;
    for (const auto& printer : printers) {
      printers_value->AppendString(printer.name);
    }
  }
  base::DictionaryValue response;
  response.SetBoolean("success", result_code == PpdProvider::SUCCESS);
  response.Set("models", std::move(printers_value));
  ResolveJavascriptCallback(base::Value(js_callback), response);
}

void CupsPrintersHandler::FileSelected(const base::FilePath& path,
                                       int index,
                                       void* params) {
  DCHECK(!webui_callback_id_.empty());

  // Load the beggining contents of the file located at |path| and callback into
  // VerifyPpdContents() in order to determine whether the file appears to be a
  // PPD file. The task's priority is USER_BLOCKING because the this task
  // updates the UI as a result of a direct user action.
  base::PostTaskWithTraitsAndReplyWithResult(
      FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_BLOCKING},
      base::BindOnce(&ReadFileToStringWithMaxSize, path, kPpdMaxLineLength),
      base::BindOnce(&CupsPrintersHandler::VerifyPpdContents,
                     weak_factory_.GetWeakPtr(), path));
}

void CupsPrintersHandler::VerifyPpdContents(const base::FilePath& path,
                                            const std::string& contents) {
  std::string result = "";
  if (PpdLineReader::ContainsMagicNumber(contents, kPpdMaxLineLength))
    result = path.value();

  ResolveJavascriptCallback(base::Value(webui_callback_id_),
                            base::Value(result));
  webui_callback_id_.clear();
}

void CupsPrintersHandler::HandleStartDiscovery(const base::ListValue* args) {
  PRINTER_LOG(DEBUG) << "Start printer discovery";
  discovery_active_ = true;
  OnPrintersChanged(
      CupsPrintersManager::kAutomatic,
      printers_manager_->GetPrinters(CupsPrintersManager::kAutomatic));
  OnPrintersChanged(
      CupsPrintersManager::kDiscovered,
      printers_manager_->GetPrinters(CupsPrintersManager::kDiscovered));
  UMA_HISTOGRAM_COUNTS_100(
      "Printing.CUPS.PrintersDiscovered",
      discovered_printers_.size() + automatic_printers_.size());
  // Scan completes immediately right now.  Emit done.
  FireWebUIListener("on-printer-discovery-done");
}

void CupsPrintersHandler::HandleStopDiscovery(const base::ListValue* args) {
  PRINTER_LOG(DEBUG) << "Stop printer discovery";
  discovered_printers_.clear();
  automatic_printers_.clear();

  // Free up memory while we're not discovering.
  discovered_printers_.shrink_to_fit();
  automatic_printers_.shrink_to_fit();
  discovery_active_ = false;
}

void CupsPrintersHandler::HandleSetUpCancel(const base::ListValue* args) {
  PRINTER_LOG(DEBUG) << "Printer setup cancelled";
  const base::DictionaryValue* printer_dict;
  CHECK(args->GetDictionary(0, &printer_dict));

  std::unique_ptr<Printer> printer = DictToPrinter(*printer_dict);
  if (printer) {
    printers_manager_->RecordSetupAbandoned(*printer);
  }
}

void CupsPrintersHandler::OnPrintersChanged(
    CupsPrintersManager::PrinterClass printer_class,
    const std::vector<Printer>& printers) {
  switch (printer_class) {
    case CupsPrintersManager::kAutomatic:
      automatic_printers_ = printers;
      UpdateDiscoveredPrinters();
      break;
    case CupsPrintersManager::kDiscovered:
      discovered_printers_ = printers;
      UpdateDiscoveredPrinters();
      break;
    case CupsPrintersManager::kConfigured: {
      auto printers_list = BuildCupsPrintersList(printers);
      FireWebUIListener("on-printers-changed", printers_list);
      break;
    }
    case CupsPrintersManager::kEnterprise:
    case CupsPrintersManager::kNumPrinterClasses:
      // These classes are not shown.
      return;
  }
}

void CupsPrintersHandler::UpdateDiscoveredPrinters() {
  if (!discovery_active_) {
    return;
  }

  std::unique_ptr<base::ListValue> printers_list =
      std::make_unique<base::ListValue>();
  for (const Printer& printer : automatic_printers_) {
    printers_list->Append(GetCupsPrinterInfo(printer));
  }
  for (const Printer& printer : discovered_printers_) {
    printers_list->Append(GetCupsPrinterInfo(printer));
  }

  FireWebUIListener("on-printer-discovered", *printers_list);
}

void CupsPrintersHandler::HandleAddDiscoveredPrinter(
    const base::ListValue* args) {
  AllowJavascript();
  CHECK_EQ(1U, args->GetSize());
  std::string printer_id;
  CHECK(args->GetString(0, &printer_id));

  PRINTER_LOG(USER) << "Adding discovered printer";
  std::unique_ptr<Printer> printer = printers_manager_->GetPrinter(printer_id);
  if (printer == nullptr) {
    PRINTER_LOG(ERROR) << "Discovered printer disappeared";
    // Printer disappeared, so we don't have information about it anymore and
    // can't really do much. Fail the add.
    FireWebUIListener("on-add-or-edit-cups-printer", base::Value(false),
                      base::Value(printer_id));
    return;
  }

  if (!printer->GetUriComponents().has_value()) {
    PRINTER_LOG(DEBUG) << "Could not parse uri";
    // The printer uri was not parsed successfully. Fail the add.
    FireWebUIListener("on-add-or-edit-cups-printer", base::Value(false),
                      base::Value(printer_id));
    return;
  }

  if (printer->ppd_reference().autoconf ||
      !printer->ppd_reference().effective_make_and_model.empty() ||
      !printer->ppd_reference().user_supplied_ppd_url.empty()) {
    PRINTER_LOG(EVENT) << "Start setup of discovered printer";
    // If we have something that looks like a ppd reference for this printer,
    // try to configure it.
    printer_configurer_->SetUpPrinter(
        *printer, base::Bind(&CupsPrintersHandler::OnAddedDiscoveredPrinter,
                             weak_factory_.GetWeakPtr(), *printer));
    return;
  }

  // The mDNS record doesn't guarantee we can setup the printer.  Query it to
  // see if we want to try IPP.
  auto address = printer->GetHostAndPort();
  if (address.IsEmpty()) {
    PRINTER_LOG(ERROR) << "Address is invalid";
    OnAddedDiscoveredPrinter(*printer, PrinterSetupResult::kPrinterUnreachable);
    return;
  }
  endpoint_resolver_->Start(
      address, base::BindOnce(&CupsPrintersHandler::OnIpResolved,
                              weak_factory_.GetWeakPtr(), std::move(printer)));
}

void CupsPrintersHandler::HandleGetPrinterPpdManufacturerAndModel(
    const base::ListValue* args) {
  AllowJavascript();
  CHECK_EQ(2U, args->GetSize());
  std::string callback_id;
  CHECK(args->GetString(0, &callback_id));
  std::string printer_id;
  CHECK(args->GetString(1, &printer_id));

  auto printer = printers_manager_->GetPrinter(printer_id);
  if (!printer) {
    RejectJavascriptCallback(base::Value(callback_id), base::Value());
    return;
  }

  ppd_provider_->ReverseLookup(
      printer->ppd_reference().effective_make_and_model,
      base::Bind(&CupsPrintersHandler::OnGetPrinterPpdManufacturerAndModel,
                 weak_factory_.GetWeakPtr(), callback_id));
}

void CupsPrintersHandler::OnGetPrinterPpdManufacturerAndModel(
    const std::string& callback_id,
    PpdProvider::CallbackResultCode result_code,
    const std::string& manufacturer,
    const std::string& model) {
  if (result_code != PpdProvider::SUCCESS) {
    RejectJavascriptCallback(base::Value(callback_id), base::Value());
    return;
  }
  base::DictionaryValue info;
  info.SetString("ppdManufacturer", manufacturer);
  info.SetString("ppdModel", model);
  ResolveJavascriptCallback(base::Value(callback_id), info);
}

void CupsPrintersHandler::FireManuallyAddDiscoveredPrinter(
    const Printer& printer) {
  FireWebUIListener("on-manually-add-discovered-printer",
                    *GetCupsPrinterInfo(printer));
}

void CupsPrintersHandler::OnIpResolved(std::unique_ptr<Printer> printer,
                                       const net::IPEndPoint& endpoint) {
  bool address_resolved = endpoint.address().IsValid();
  UMA_HISTOGRAM_BOOLEAN("Printing.CUPS.AddressResolutionResult",
                        address_resolved);
  if (!address_resolved) {
    PRINTER_LOG(ERROR) << printer->make_and_model() << " IP Resolution failed";
    OnAddedDiscoveredPrinter(*printer, PrinterSetupResult::kPrinterUnreachable);
    return;
  }

  PRINTER_LOG(EVENT) << printer->make_and_model() << " IP Resolution succeeded";
  std::string resolved_uri = printer->ReplaceHostAndPort(endpoint);

  if (IsIppUri(resolved_uri)) {
    PRINTER_LOG(EVENT) << "Query printer for IPP attributes";
    QueryAutoconf(resolved_uri,
                  base::BindRepeating(
                      &CupsPrintersHandler::OnAutoconfQueriedDiscovered,
                      weak_factory_.GetWeakPtr(), base::Passed(&printer)));
    return;
  }

  PRINTER_LOG(EVENT) << "Request make and model from user";
  // If it's not an IPP printer, the user must choose a PPD.
  FireManuallyAddDiscoveredPrinter(*printer);
}

}  // namespace settings
}  // namespace chromeos
