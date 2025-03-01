// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chromecast/browser/general_audience_browsing_service.h"

#include "base/logging.h"
#include "chromecast/common/mojom/constants.mojom.h"
#include "components/policy/core/browser/url_util.h"
#include "components/safe_search_api/safe_search/safe_search_url_checker_client.h"
#include "components/safe_search_api/url_checker.h"
#include "content/public/common/service_manager_connection.h"
#include "net/base/net_errors.h"
#include "services/network/public/cpp/shared_url_loader_factory.h"
#include "services/service_manager/public/cpp/connector.h"

namespace chromecast {

namespace {

// Calls the CheckURLCallback with the result of the Safe Search API check.
void CheckURLCallbackWrapper(
    GeneralAudienceBrowsingService::CheckURLCallback callback,
    const GURL& /* url */,
    safe_search_api::Classification classification,
    bool /* uncertain */) {
  std::move(callback).Run(classification ==
                          safe_search_api::Classification::SAFE);
}

net::NetworkTrafficAnnotationTag CreateNetworkTrafficAnnotationTag() {
  return net::DefineNetworkTrafficAnnotation(
      "cast_general_audience_browsing_throttle", R"(
          semantics {
            sender: "Cast Safe Search"
            description:
              "Checks whether a given URL (or set of URLs) is considered "
              "safe by Google SafeSearch."
            trigger:
              "This is sent for every navigation."
            data: "URL to be checked."
            destination: GOOGLE_OWNED_SERVICE
          }
          policy {
            cookies_allowed: NO
            setting:
              "This fearture is always enabled"
            chrome_policy {
              SafeSitesFilterBehavior {
                SafeSitesFilterBehavior: 0
              }
            }
          })");
}

}  // namespace

GeneralAudienceBrowsingService::GeneralAudienceBrowsingService(
    scoped_refptr<network::SharedURLLoaderFactory> shared_url_loader_factory)
    : shared_url_loader_factory_(shared_url_loader_factory),
      general_audience_browsing_api_key_observer_binding_(this) {
  mojom::GeneralAudienceBrowsingAPIKeyObserverPtr observer_ptr;
  general_audience_browsing_api_key_observer_binding_.Bind(
      mojo::MakeRequest(&observer_ptr));
  content::ServiceManagerConnection::GetForProcess()
      ->GetConnector()
      ->BindInterface(mojom::kChromecastServiceName,
                      &general_audience_browsing_api_key_subject_ptr_);
  general_audience_browsing_api_key_subject_ptr_
      ->AddGeneralAudienceBrowsingAPIKeyObserver(std::move(observer_ptr));
}

GeneralAudienceBrowsingService::~GeneralAudienceBrowsingService() = default;

bool GeneralAudienceBrowsingService::CheckURL(const GURL& url,
                                              CheckURLCallback callback) {
  if (!safe_search_url_checker_) {
    safe_search_url_checker_ = CreateSafeSearchURLChecker();
  }

  return safe_search_url_checker_->CheckURL(
      policy::url_util::Normalize(url),
      base::BindOnce(&CheckURLCallbackWrapper, std::move(callback)));
}

void GeneralAudienceBrowsingService::SetSafeSearchURLCheckerForTest(
    std::unique_ptr<safe_search_api::URLChecker> safe_search_url_checker) {
  safe_search_url_checker_ = std::move(safe_search_url_checker);
}

void GeneralAudienceBrowsingService::OnGeneralAudienceBrowsingAPIKeyChanged(
    const std::string& api_key) {
  if (api_key != api_key_) {
    api_key_ = api_key;
    if (safe_search_url_checker_) {
      // The URLChecker only accepts API key in constructor, no way to change it
      // after it's been created. So we'll have to recreate one if the API key
      // has been changed. (This should rarely happen though.)
      safe_search_url_checker_ = CreateSafeSearchURLChecker();
    }
  }
}

std::unique_ptr<safe_search_api::URLChecker>
GeneralAudienceBrowsingService::CreateSafeSearchURLChecker() {
  return std::make_unique<safe_search_api::URLChecker>(
      std::make_unique<safe_search_api::SafeSearchURLCheckerClient>(
          shared_url_loader_factory_, CreateNetworkTrafficAnnotationTag(),
          std::string(), api_key_),
      /* cache size */ 1000);
}

}  // namespace chromecast
