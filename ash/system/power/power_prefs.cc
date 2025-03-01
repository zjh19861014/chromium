// Copyright 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "ash/system/power/power_prefs.h"

#include <string>
#include <vector>

#include "ash/public/cpp/ash_pref_names.h"
#include "ash/session/session_controller.h"
#include "ash/shell.h"
#include "base/bind.h"
#include "base/callback.h"
#include "base/time/default_tick_clock.h"
#include "chromeos/constants/chromeos_features.h"
#include "chromeos/dbus/power/power_policy_controller.h"
#include "chromeos/dbus/power_manager/idle.pb.h"
#include "components/pref_registry/pref_registry_syncable.h"
#include "components/prefs/pref_change_registrar.h"
#include "components/prefs/pref_registry.h"
#include "components/prefs/pref_service.h"

namespace ash {

namespace {

using PeakShiftDayConfiguration =
    chromeos::PowerPolicyController::PeakShiftDayConfiguration;

chromeos::PowerPolicyController::Action GetPowerPolicyAction(
    const PrefService* prefs,
    const std::string& pref_name) {
  const chromeos::PowerPolicyController::Action pref_action =
      static_cast<chromeos::PowerPolicyController::Action>(
          prefs->GetInteger(pref_name));

  // Transform the power policy action when the lock screen is disabled and
  // power preferences request to lock the screen: the session stop should be
  // requested instead.
  //
  // This resolves potential privacy issues when the device could suspend
  // before the session stop is fully finished and the login screen is shown.
  //
  // Note that the power policy prefs related to showing the lock screen on idle
  // don't have to be adjusted accordingly, as Chrome itself will perform
  // session stop instead of screen lock when the latter one is not available.
  if (pref_action == chromeos::PowerPolicyController::ACTION_SUSPEND &&
      prefs->GetBoolean(prefs::kEnableAutoScreenLock) &&
      !prefs->GetBoolean(prefs::kAllowScreenLock)) {
    return chromeos::PowerPolicyController::ACTION_STOP_SESSION;
  }

  return pref_action;
}

// Returns the PrefService that should be used for determining power-related
// behavior. When one or more users are logged in, the primary user's prefs are
// used: if more-restrictive power-related prefs are set by policy, it's most
// likely to be on this profile.
PrefService* GetPrefService() {
  ash::SessionController* controller = Shell::Get()->session_controller();
  PrefService* prefs = controller->GetPrimaryUserPrefService();
  return prefs ? prefs : controller->GetActivePrefService();
}

// Registers power prefs whose default values are the same in user prefs and
// signin prefs.
void RegisterProfilePrefs(PrefRegistrySimple* registry, bool for_test) {
  registry->RegisterIntegerPref(prefs::kPowerAcScreenBrightnessPercent, -1,
                                PrefRegistry::PUBLIC);
  registry->RegisterIntegerPref(prefs::kPowerAcScreenDimDelayMs, 420000,
                                PrefRegistry::PUBLIC);
  registry->RegisterIntegerPref(prefs::kPowerAcScreenOffDelayMs, 450000,
                                PrefRegistry::PUBLIC);
  registry->RegisterIntegerPref(prefs::kPowerAcScreenLockDelayMs, 0,
                                PrefRegistry::PUBLIC);
  registry->RegisterIntegerPref(prefs::kPowerAcIdleWarningDelayMs, 0,
                                PrefRegistry::PUBLIC);
  registry->RegisterIntegerPref(prefs::kPowerAcIdleDelayMs, 510000,
                                PrefRegistry::PUBLIC);
  registry->RegisterIntegerPref(prefs::kPowerBatteryScreenBrightnessPercent, -1,
                                PrefRegistry::PUBLIC);
  registry->RegisterIntegerPref(prefs::kPowerBatteryScreenDimDelayMs, 300000,
                                PrefRegistry::PUBLIC);
  registry->RegisterIntegerPref(prefs::kPowerBatteryScreenOffDelayMs, 330000,
                                PrefRegistry::PUBLIC);
  registry->RegisterIntegerPref(prefs::kPowerBatteryScreenLockDelayMs, 0,
                                PrefRegistry::PUBLIC);
  registry->RegisterIntegerPref(prefs::kPowerBatteryIdleWarningDelayMs, 0,
                                PrefRegistry::PUBLIC);
  registry->RegisterIntegerPref(prefs::kPowerBatteryIdleDelayMs, 390000,
                                PrefRegistry::PUBLIC);
  registry->RegisterIntegerPref(prefs::kPowerLockScreenDimDelayMs, 30000,
                                PrefRegistry::PUBLIC);
  registry->RegisterIntegerPref(prefs::kPowerLockScreenOffDelayMs, 40000,
                                PrefRegistry::PUBLIC);
  registry->RegisterIntegerPref(prefs::kPowerAcIdleAction,
                                chromeos::PowerPolicyController::ACTION_SUSPEND,
                                PrefRegistry::PUBLIC);
  registry->RegisterBooleanPref(prefs::kPowerUseAudioActivity, true,
                                PrefRegistry::PUBLIC);
  registry->RegisterBooleanPref(prefs::kPowerUseVideoActivity, true,
                                PrefRegistry::PUBLIC);
  registry->RegisterBooleanPref(prefs::kPowerAllowWakeLocks, true,
                                PrefRegistry::PUBLIC);
  registry->RegisterBooleanPref(prefs::kPowerAllowScreenWakeLocks, true,
                                PrefRegistry::PUBLIC);
  registry->RegisterDoublePref(prefs::kPowerPresentationScreenDimDelayFactor,
                               2.0, PrefRegistry::PUBLIC);
  registry->RegisterDoublePref(prefs::kPowerUserActivityScreenDimDelayFactor,
                               2.0, PrefRegistry::PUBLIC);
  registry->RegisterBooleanPref(prefs::kPowerWaitForInitialUserActivity, false,
                                PrefRegistry::PUBLIC);
  registry->RegisterBooleanPref(
      prefs::kPowerForceNonzeroBrightnessForUserActivity, true,
      PrefRegistry::PUBLIC);
  registry->RegisterBooleanPref(prefs::kPowerFastSuspendWhenBacklightsForcedOff,
                                true, PrefRegistry::PUBLIC);
  registry->RegisterBooleanPref(prefs::kPowerSmartDimEnabled, true,
                                PrefRegistry::PUBLIC);

  if (for_test) {
    registry->RegisterBooleanPref(prefs::kAllowScreenLock, true,
                                  PrefRegistry::PUBLIC);
    registry->RegisterBooleanPref(
        prefs::kEnableAutoScreenLock, false,
        user_prefs::PrefRegistrySyncable::SYNCABLE_PREF | PrefRegistry::PUBLIC);
  } else {
    registry->RegisterForeignPref(prefs::kAllowScreenLock);
    registry->RegisterForeignPref(prefs::kEnableAutoScreenLock);
  }
}

// Saves appropriate value to |week_day| and returns true if there is mapping
// between week day string and enum value.
bool GetWeekDayFromString(const std::string& week_day_str,
                          chromeos::PowerPolicyController::WeekDay* week_day) {
  DCHECK(week_day);
  if (week_day_str == "MONDAY") {
    *week_day = chromeos::PowerPolicyController::WeekDay::WEEK_DAY_MONDAY;
  } else if (week_day_str == "TUESDAY") {
    *week_day = chromeos::PowerPolicyController::WeekDay::WEEK_DAY_TUESDAY;
  } else if (week_day_str == "WEDNESDAY") {
    *week_day = chromeos::PowerPolicyController::WeekDay::WEEK_DAY_WEDNESDAY;
  } else if (week_day_str == "THURSDAY") {
    *week_day = chromeos::PowerPolicyController::WeekDay::WEEK_DAY_THURSDAY;
  } else if (week_day_str == "FRIDAY") {
    *week_day = chromeos::PowerPolicyController::WeekDay::WEEK_DAY_FRIDAY;
  } else if (week_day_str == "SATURDAY") {
    *week_day = chromeos::PowerPolicyController::WeekDay::WEEK_DAY_SATURDAY;
  } else if (week_day_str == "SUNDAY") {
    *week_day = chromeos::PowerPolicyController::WeekDay::WEEK_DAY_SUNDAY;
  } else {
    return false;
  }
  return true;
}

// Converts |base::Value| to |std::vector<PeakShiftDayConfiguration>| and
// returns true if there are no missing fields and errors.
bool GetPeakShiftDayConfigurations(
    const base::DictionaryValue& value,
    std::vector<PeakShiftDayConfiguration>* configs_out) {
  DCHECK(configs_out);
  configs_out->clear();

  const base::Value* entries =
      value.FindKeyOfType({"entries"}, base::Value::Type::LIST);
  if (!entries) {
    return false;
  }

  for (const base::Value& item : entries->GetList()) {
    const base::Value* week_day_value =
        item.FindKeyOfType({"day"}, base::Value::Type::STRING);
    const base::Value* start_time_hour =
        item.FindPathOfType({"start_time", "hour"}, base::Value::Type::INTEGER);
    const base::Value* start_time_minute = item.FindPathOfType(
        {"start_time", "minute"}, base::Value::Type::INTEGER);
    const base::Value* end_time_hour =
        item.FindPathOfType({"end_time", "hour"}, base::Value::Type::INTEGER);
    const base::Value* end_time_minute =
        item.FindPathOfType({"end_time", "minute"}, base::Value::Type::INTEGER);
    const base::Value* charge_start_time_hour = item.FindPathOfType(
        {"charge_start_time", "hour"}, base::Value::Type::INTEGER);
    const base::Value* charge_start_time_minute = item.FindPathOfType(
        {"charge_start_time", "minute"}, base::Value::Type::INTEGER);

    chromeos::PowerPolicyController::WeekDay week_day_enum;
    if (!week_day_value ||
        !GetWeekDayFromString(week_day_value->GetString(), &week_day_enum) ||
        !start_time_hour || !start_time_minute || !end_time_hour ||
        !end_time_minute || !charge_start_time_hour ||
        !charge_start_time_minute) {
      return false;
    }

    PeakShiftDayConfiguration config;
    config.day = week_day_enum;
    config.start_time.hour = start_time_hour->GetInt();
    config.start_time.minute = start_time_minute->GetInt();
    config.end_time.hour = end_time_hour->GetInt();
    config.end_time.minute = end_time_minute->GetInt();
    config.charge_start_time.hour = charge_start_time_hour->GetInt();
    config.charge_start_time.minute = charge_start_time_minute->GetInt();

    configs_out->push_back(std::move(config));
  }

  return true;
}

}  // namespace

PowerPrefs::PowerPrefs(chromeos::PowerPolicyController* power_policy_controller,
                       chromeos::PowerManagerClient* power_manager_client)
    : power_policy_controller_(power_policy_controller),
      power_manager_client_observer_(this),
      tick_clock_(base::DefaultTickClock::GetInstance()) {
  DCHECK(power_manager_client);
  DCHECK(power_policy_controller_);
  DCHECK(tick_clock_);

  Shell::Get()->AddShellObserver(this);

  power_manager_client_observer_.Add(power_manager_client);
  Shell::Get()->session_controller()->AddObserver(this);
}

PowerPrefs::~PowerPrefs() {
  Shell::Get()->RemoveShellObserver(this);
  Shell::Get()->session_controller()->RemoveObserver(this);
}

// static
void PowerPrefs::RegisterLocalStatePrefs(PrefRegistrySimple* registry) {
  registry->RegisterBooleanPref(prefs::kDevicePowerPeakShiftEnabled, false,
                                PrefRegistry::PUBLIC);
  registry->RegisterIntegerPref(prefs::kDevicePowerPeakShiftBatteryThreshold,
                                -1, PrefRegistry::PUBLIC);
  registry->RegisterDictionaryPref(prefs::kDevicePowerPeakShiftDayConfig,
                                   PrefRegistry::PUBLIC);

  registry->RegisterBooleanPref(prefs::kDeviceBootOnAcEnabled, false,
                                PrefRegistry::PUBLIC);

  registry->RegisterBooleanPref(prefs::kDeviceAdvancedBatteryChargeModeEnabled,
                                false, PrefRegistry::PUBLIC);
  registry->RegisterDictionaryPref(
      prefs::kDeviceAdvancedBatteryChargeModeDayConfig, PrefRegistry::PUBLIC);

  registry->RegisterIntegerPref(prefs::kDeviceBatteryChargeMode, -1,
                                PrefRegistry::PUBLIC);
  registry->RegisterIntegerPref(prefs::kDeviceBatteryChargeCustomStartCharging,
                                -1, PrefRegistry::PUBLIC);
  registry->RegisterIntegerPref(prefs::kDeviceBatteryChargeCustomStopCharging,
                                -1, PrefRegistry::PUBLIC);
}

// static
void PowerPrefs::RegisterSigninProfilePrefs(PrefRegistrySimple* registry,
                                            bool for_test) {
  RegisterProfilePrefs(registry, for_test);

  registry->RegisterIntegerPref(
      prefs::kPowerBatteryIdleAction,
      chromeos::PowerPolicyController::ACTION_SHUT_DOWN, PrefRegistry::PUBLIC);
  registry->RegisterIntegerPref(
      prefs::kPowerLidClosedAction,
      chromeos::PowerPolicyController::ACTION_SHUT_DOWN, PrefRegistry::PUBLIC);
}

// static
void PowerPrefs::RegisterUserProfilePrefs(PrefRegistrySimple* registry,
                                          bool for_test) {
  RegisterProfilePrefs(registry, for_test);

  registry->RegisterIntegerPref(prefs::kPowerBatteryIdleAction,
                                chromeos::PowerPolicyController::ACTION_SUSPEND,
                                PrefRegistry::PUBLIC);
  registry->RegisterIntegerPref(prefs::kPowerLidClosedAction,
                                chromeos::PowerPolicyController::ACTION_SUSPEND,
                                PrefRegistry::PUBLIC);
}

void PowerPrefs::ScreenIdleStateChanged(
    const power_manager::ScreenIdleState& proto) {
  const bool already_off = !screen_idle_off_time_.is_null();
  if (proto.off() == already_off)
    return;

  screen_idle_off_time_ =
      proto.off() ? tick_clock_->NowTicks() : base::TimeTicks();

  // If the screen is locked and we're no longer idle, we may need to switch to
  // the lock-based delays.
  if (!screen_lock_time_.is_null() && !proto.off())
    UpdatePowerPolicyFromPrefs();
}

void PowerPrefs::OnLockStateChanged(bool locked) {
  const bool already_locked = !screen_lock_time_.is_null();
  if (locked == already_locked)
    return;

  screen_lock_time_ = locked ? tick_clock_->NowTicks() : base::TimeTicks();
  // OnLockStateChanged could be called before ash connects user prefs in tests.
  if (GetPrefService())
    UpdatePowerPolicyFromPrefs();
}

void PowerPrefs::OnSigninScreenPrefServiceInitialized(PrefService* prefs) {
  ObservePrefs(prefs);
}

void PowerPrefs::OnActiveUserPrefServiceChanged(PrefService* prefs) {
  ObservePrefs(prefs);
}

void PowerPrefs::OnLocalStatePrefServiceInitialized(PrefService* prefs) {
  local_state_ = prefs;

  // Pass |nullptr| in tests, because lifetime of local state prefs is shorter
  // than lifetime of PowerPrefs.
  if (local_state_) {
    ObserveLocalStatePrefs(prefs);
  }
}

void PowerPrefs::UpdatePowerPolicyFromPrefs() {
  PrefService* prefs = GetPrefService();
  if (!prefs || !local_state_)
    return;

  // It's possible to end up in a situation where a shortened lock-screen idle
  // delay would cause the system to suspend immediately as soon as the screen
  // is locked due to inactivity; see https://crbug.com/807861 for the gory
  // details. To avoid this, don't switch to the shorter delays immediately when
  // the screen is locked automatically (as indicated by the screen having been
  // previously turned off for inactivity).
  bool use_lock_delays = !screen_lock_time_.is_null() &&
                         (screen_idle_off_time_.is_null() ||
                          screen_idle_off_time_ > screen_lock_time_);

  chromeos::PowerPolicyController::PrefValues values;

  values.ac_brightness_percent =
      prefs->GetInteger(prefs::kPowerAcScreenBrightnessPercent);
  values.ac_screen_dim_delay_ms =
      prefs->GetInteger(use_lock_delays ? prefs::kPowerLockScreenDimDelayMs
                                        : prefs::kPowerAcScreenDimDelayMs);
  values.ac_screen_off_delay_ms =
      prefs->GetInteger(use_lock_delays ? prefs::kPowerLockScreenOffDelayMs
                                        : prefs::kPowerAcScreenOffDelayMs);
  values.ac_screen_lock_delay_ms =
      prefs->GetInteger(prefs::kPowerAcScreenLockDelayMs);
  values.ac_idle_warning_delay_ms =
      prefs->GetInteger(prefs::kPowerAcIdleWarningDelayMs);
  values.ac_idle_delay_ms = prefs->GetInteger(prefs::kPowerAcIdleDelayMs);
  values.battery_brightness_percent =
      prefs->GetInteger(prefs::kPowerBatteryScreenBrightnessPercent);
  values.battery_screen_dim_delay_ms =
      prefs->GetInteger(use_lock_delays ? prefs::kPowerLockScreenDimDelayMs
                                        : prefs::kPowerBatteryScreenDimDelayMs);
  values.battery_screen_off_delay_ms =
      prefs->GetInteger(use_lock_delays ? prefs::kPowerLockScreenOffDelayMs
                                        : prefs::kPowerBatteryScreenOffDelayMs);
  values.battery_screen_lock_delay_ms =
      prefs->GetInteger(prefs::kPowerBatteryScreenLockDelayMs);
  values.battery_idle_warning_delay_ms =
      prefs->GetInteger(prefs::kPowerBatteryIdleWarningDelayMs);
  values.battery_idle_delay_ms =
      prefs->GetInteger(prefs::kPowerBatteryIdleDelayMs);
  values.ac_idle_action =
      GetPowerPolicyAction(prefs, prefs::kPowerAcIdleAction);
  values.battery_idle_action =
      GetPowerPolicyAction(prefs, prefs::kPowerBatteryIdleAction);
  values.lid_closed_action =
      GetPowerPolicyAction(prefs, prefs::kPowerLidClosedAction);
  values.use_audio_activity = prefs->GetBoolean(prefs::kPowerUseAudioActivity);
  values.use_video_activity = prefs->GetBoolean(prefs::kPowerUseVideoActivity);
  values.allow_wake_locks = prefs->GetBoolean(prefs::kPowerAllowWakeLocks);
  values.allow_screen_wake_locks =
      prefs->GetBoolean(prefs::kPowerAllowScreenWakeLocks);
  values.enable_auto_screen_lock =
      prefs->GetBoolean(prefs::kEnableAutoScreenLock);

  // Screen-dim deferral in response to user activity predictions can interact
  // poorly with delay scaling, resulting in the system staying awake for a long
  // time if a prediction is wrong. https://crbug.com/888392.
  if (prefs->GetBoolean(prefs::kPowerSmartDimEnabled) &&
      base::FeatureList::IsEnabled(
          chromeos::features::kUserActivityPrediction)) {
    values.presentation_screen_dim_delay_factor = 1.0;
    values.user_activity_screen_dim_delay_factor = 1.0;
  } else {
    values.presentation_screen_dim_delay_factor =
        prefs->GetDouble(prefs::kPowerPresentationScreenDimDelayFactor);
    values.user_activity_screen_dim_delay_factor =
        prefs->GetDouble(prefs::kPowerUserActivityScreenDimDelayFactor);
  }

  values.wait_for_initial_user_activity =
      prefs->GetBoolean(prefs::kPowerWaitForInitialUserActivity);
  values.force_nonzero_brightness_for_user_activity =
      prefs->GetBoolean(prefs::kPowerForceNonzeroBrightnessForUserActivity);
  values.fast_suspend_when_backlights_forced_off =
      prefs->GetBoolean(prefs::kPowerFastSuspendWhenBacklightsForcedOff);

  if (local_state_->GetBoolean(prefs::kDevicePowerPeakShiftEnabled) &&
      local_state_->IsManagedPreference(prefs::kDevicePowerPeakShiftEnabled) &&
      local_state_->IsManagedPreference(
          prefs::kDevicePowerPeakShiftBatteryThreshold) &&
      local_state_->IsManagedPreference(
          prefs::kDevicePowerPeakShiftDayConfig)) {
    const base::DictionaryValue* configs_value =
        local_state_->GetDictionary(prefs::kDevicePowerPeakShiftDayConfig);
    DCHECK(configs_value);
    std::vector<PeakShiftDayConfiguration> configs;
    if (GetPeakShiftDayConfigurations(*configs_value, &configs)) {
      values.peak_shift_enabled = true;
      values.peak_shift_battery_threshold = local_state_->GetInteger(
          prefs::kDevicePowerPeakShiftBatteryThreshold);
      values.peak_shift_day_configurations = std::move(configs);
    } else {
      LOG(WARNING) << "Invalid Peak Shift day configs format: "
                   << *configs_value;
    }
  }

  if (local_state_->IsManagedPreference(prefs::kDeviceBootOnAcEnabled)) {
    values.boot_on_ac = local_state_->GetBoolean(prefs::kDeviceBootOnAcEnabled);
  }

  power_policy_controller_->ApplyPrefs(values);
}

void PowerPrefs::ObservePrefs(PrefService* prefs) {
  // Observe pref updates from policy.
  base::RepeatingClosure update_callback(base::BindRepeating(
      &PowerPrefs::UpdatePowerPolicyFromPrefs, base::Unretained(this)));

  profile_registrar_ = std::make_unique<PrefChangeRegistrar>();
  profile_registrar_->Init(prefs);
  profile_registrar_->Add(prefs::kPowerAcScreenBrightnessPercent,
                          update_callback);
  profile_registrar_->Add(prefs::kPowerAcScreenDimDelayMs, update_callback);
  profile_registrar_->Add(prefs::kPowerAcScreenOffDelayMs, update_callback);
  profile_registrar_->Add(prefs::kPowerAcScreenLockDelayMs, update_callback);
  profile_registrar_->Add(prefs::kPowerAcIdleWarningDelayMs, update_callback);
  profile_registrar_->Add(prefs::kPowerAcIdleDelayMs, update_callback);
  profile_registrar_->Add(prefs::kPowerBatteryScreenBrightnessPercent,
                          update_callback);
  profile_registrar_->Add(prefs::kPowerBatteryScreenDimDelayMs,
                          update_callback);
  profile_registrar_->Add(prefs::kPowerBatteryScreenOffDelayMs,
                          update_callback);
  profile_registrar_->Add(prefs::kPowerBatteryScreenLockDelayMs,
                          update_callback);
  profile_registrar_->Add(prefs::kPowerBatteryIdleWarningDelayMs,
                          update_callback);
  profile_registrar_->Add(prefs::kPowerBatteryIdleDelayMs, update_callback);
  profile_registrar_->Add(prefs::kPowerLockScreenDimDelayMs, update_callback);
  profile_registrar_->Add(prefs::kPowerLockScreenOffDelayMs, update_callback);
  profile_registrar_->Add(prefs::kPowerAcIdleAction, update_callback);
  profile_registrar_->Add(prefs::kPowerBatteryIdleAction, update_callback);
  profile_registrar_->Add(prefs::kPowerLidClosedAction, update_callback);
  profile_registrar_->Add(prefs::kPowerUseAudioActivity, update_callback);
  profile_registrar_->Add(prefs::kPowerUseVideoActivity, update_callback);
  profile_registrar_->Add(prefs::kPowerAllowWakeLocks, update_callback);
  profile_registrar_->Add(prefs::kPowerAllowScreenWakeLocks, update_callback);
  profile_registrar_->Add(prefs::kEnableAutoScreenLock, update_callback);
  profile_registrar_->Add(prefs::kPowerPresentationScreenDimDelayFactor,
                          update_callback);
  profile_registrar_->Add(prefs::kPowerUserActivityScreenDimDelayFactor,
                          update_callback);
  profile_registrar_->Add(prefs::kPowerWaitForInitialUserActivity,
                          update_callback);
  profile_registrar_->Add(prefs::kPowerForceNonzeroBrightnessForUserActivity,
                          update_callback);
  profile_registrar_->Add(prefs::kAllowScreenLock, update_callback);
  profile_registrar_->Add(prefs::kPowerSmartDimEnabled, update_callback);
  profile_registrar_->Add(prefs::kPowerFastSuspendWhenBacklightsForcedOff,
                          update_callback);

  UpdatePowerPolicyFromPrefs();
}

void PowerPrefs::ObserveLocalStatePrefs(PrefService* prefs) {
  // Observe pref updates from locked state change and policy.
  base::RepeatingClosure update_callback(base::BindRepeating(
      &PowerPrefs::UpdatePowerPolicyFromPrefs, base::Unretained(this)));

  local_state_registrar_ = std::make_unique<PrefChangeRegistrar>();
  local_state_registrar_->Init(prefs);
  local_state_registrar_->Add(prefs::kDevicePowerPeakShiftEnabled,
                              update_callback);
  local_state_registrar_->Add(prefs::kDevicePowerPeakShiftBatteryThreshold,
                              update_callback);
  local_state_registrar_->Add(prefs::kDevicePowerPeakShiftDayConfig,
                              update_callback);

  local_state_registrar_->Add(prefs::kDeviceBootOnAcEnabled, update_callback);

  UpdatePowerPolicyFromPrefs();
}

}  // namespace ash
