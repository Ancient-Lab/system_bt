/******************************************************************************
 *
 *  Copyright (C) 2014  Broadcom Corporation
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at:
 *
 *  http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 *
 ******************************************************************************/

#include <base/bind.h>
#include <base/logging.h>
#include <base/strings/string_number_conversions.h>
#include <string.h>
#include <queue>
#include <vector>

#include "bt_target.h"
#include "device/include/controller.h"
#include "osi/include/alarm.h"

#include "ble_advertiser.h"
#include "ble_advertiser_hci_interface.h"
#include "btm_int_types.h"

using base::Bind;
using RegisterCb =
    base::Callback<void(uint8_t /* inst_id */, uint8_t /* status */)>;
using IdTxPowerStatusCb = base::Callback<void(
    uint8_t /* inst_id */, int8_t /* tx_power */, uint8_t /* status */)>;
extern void btm_gen_resolvable_private_addr(
    base::Callback<void(uint8_t[8])> cb);
extern fixed_queue_t* btu_general_alarm_queue;

constexpr int ADV_DATA_LEN_MAX = 251;

struct AdvertisingInstance {
  uint8_t inst_id;
  bool in_use;
  uint8_t advertising_event_properties;
  alarm_t* adv_raddr_timer;
  int8_t tx_power;
  int timeout_s;
  alarm_t* timeout_timer;
  uint8_t own_address_type;
  BD_ADDR own_address;

  AdvertisingInstance(int inst_id)
      : inst_id(inst_id),
        in_use(false),
        advertising_event_properties(0),
        tx_power(0),
        timeout_s(0),
        timeout_timer(nullptr),
        own_address_type(0),
        own_address{0} {
    adv_raddr_timer = alarm_new_periodic("btm_ble.adv_raddr_timer");
  }

  ~AdvertisingInstance() {
    alarm_free(adv_raddr_timer);
    if (timeout_timer) alarm_free(timeout_timer);
  }
};

void btm_ble_adv_raddr_timer_timeout(void* data);

namespace {

void DoNothing(uint8_t) {}
void DoNothing2(uint8_t, uint8_t) {}

bool is_legacy_connectable(uint16_t advertising_event_properties) {
  if (((advertising_event_properties & 0x10) != 0) &&
      ((advertising_event_properties & 0x01) != 0)) {
    return true;
  }
  return false;
}

struct closure_data {
  base::Closure user_task;
  tracked_objects::Location posted_from;
};

static void alarm_closure_cb(void* p) {
  closure_data* data = (closure_data*)p;
  VLOG(1) << "executing timer scheduled at %s" << data->posted_from.ToString();
  data->user_task.Run();
  delete data;
}

// Periodic alarms are not supported, because we clean up data in callback
void alarm_set_closure_on_queue(const tracked_objects::Location& posted_from,
                                alarm_t* alarm, period_ms_t interval_ms,
                                base::Closure user_task, fixed_queue_t* queue) {
  closure_data* data = new closure_data;
  data->posted_from = posted_from;
  data->user_task = std::move(user_task);
  VLOG(1) << "scheduling timer %s" << data->posted_from.ToString();
  alarm_set_on_queue(alarm, interval_ms, alarm_closure_cb, data, queue);
}

class BleAdvertisingManagerImpl;

/* a temporary type for holding all the data needed in callbacks below*/
struct CreatorParams {
  uint8_t inst_id;
  BleAdvertisingManagerImpl* self;
  IdTxPowerStatusCb cb;
  tBTM_BLE_ADV_PARAMS params;
  std::vector<uint8_t> advertise_data;
  std::vector<uint8_t> scan_response_data;
  tBLE_PERIODIC_ADV_PARAMS periodic_params;
  std::vector<uint8_t> periodic_data;
  int timeout_s;
  RegisterCb timeout_cb;
};

using c_type = std::unique_ptr<CreatorParams>;

class BleAdvertisingManagerImpl
    : public BleAdvertisingManager,
      public BleAdvertiserHciInterface::AdvertisingEventObserver {
 public:
  BleAdvertisingManagerImpl(BleAdvertiserHciInterface* interface) {
    this->hci_interface = interface;
    hci_interface->ReadInstanceCount(
        base::Bind(&BleAdvertisingManagerImpl::ReadInstanceCountCb,
                   base::Unretained(this)));
  }

  ~BleAdvertisingManagerImpl() { adv_inst.clear(); }

  void ReadInstanceCountCb(uint8_t instance_count) {
    this->inst_count = instance_count;
    adv_inst.reserve(inst_count);
    /* Initialize adv instance indices and IDs. */
    for (uint8_t i = 0; i < inst_count; i++) {
      adv_inst.emplace_back(i);
    }
  }

  void OnRpaGenerationComplete(uint8_t inst_id, base::Closure cb,
                               uint8_t rand[8]) {
    LOG(INFO) << "inst_id = " << +inst_id;

    AdvertisingInstance* p_inst = &adv_inst[inst_id];

    rand[2] &= (~BLE_RESOLVE_ADDR_MASK);
    rand[2] |= BLE_RESOLVE_ADDR_MSB;

    p_inst->own_address[2] = rand[0];
    p_inst->own_address[1] = rand[1];
    p_inst->own_address[0] = rand[2];

    BT_OCTET16 irk;
    BTM_GetDeviceIDRoot(irk);
    tSMP_ENC output;

    if (!SMP_Encrypt(irk, BT_OCTET16_LEN, rand, 3, &output))
      LOG_ASSERT(false) << "SMP_Encrypt failed";

    /* set hash to be LSB of rpAddress */
    p_inst->own_address[5] = output.param_buf[0];
    p_inst->own_address[4] = output.param_buf[1];
    p_inst->own_address[3] = output.param_buf[2];

    cb.Run();
  }

  void GenerateRpa(uint8_t inst_id, base::Closure cb) {
    btm_gen_resolvable_private_addr(
        Bind(&BleAdvertisingManagerImpl::OnRpaGenerationComplete,
             base::Unretained(this), inst_id, std::move(cb)));
  }

  void ConfigureRpa(AdvertisingInstance* p_inst) {
    GenerateRpa(p_inst->inst_id,
                Bind(
                    [](AdvertisingInstance* p_inst) {
                      /* set it to controller */
                      ((BleAdvertisingManagerImpl*)BleAdvertisingManager::Get())
                          ->GetHciInterface()
                          ->SetRandomAddress(p_inst->inst_id,
                                             p_inst->own_address,
                                             Bind(DoNothing));
                    },
                    p_inst));
  }

  void RegisterAdvertiser(
      base::Callback<void(uint8_t /* inst_id */, uint8_t /* status */)> cb)
      override {
    AdvertisingInstance* p_inst = &adv_inst[0];
    for (uint8_t i = 0; i < inst_count; i++, p_inst++) {
      if (p_inst->in_use) continue;

      p_inst->in_use = true;

#if (BLE_PRIVACY_SPT == TRUE)
      // set up periodic timer to update address.
      if (BTM_BleLocalPrivacyEnabled()) {
        p_inst->own_address_type = BLE_ADDR_RANDOM;
        GenerateRpa(p_inst->inst_id,
                    Bind(
                        [](AdvertisingInstance* p_inst,
                           base::Callback<void(uint8_t /* inst_id */,
                                               uint8_t /* status */)>
                               cb) {
                          alarm_set_on_queue(p_inst->adv_raddr_timer,
                                             BTM_BLE_PRIVATE_ADDR_INT_MS,
                                             btm_ble_adv_raddr_timer_timeout,
                                             p_inst, btu_general_alarm_queue);
                          cb.Run(p_inst->inst_id, BTM_BLE_MULTI_ADV_SUCCESS);
                        },
                        p_inst, cb));
      }
#else
      p_inst->own_address_type = BLE_ADDR_PUBLIC;
      memcpy(p_inst->own_address,
             controller_get_interface()->get_address()->address, BD_ADDR_LEN);

      cb.Run(p_inst->inst_id, BTM_BLE_MULTI_ADV_SUCCESS);
#endif
      return;
    }

    LOG(INFO) << "no free advertiser instance";
    cb.Run(0xFF, ADVERTISE_FAILED_TOO_MANY_ADVERTISERS);
  }

  void StartAdvertising(uint8_t advertiser_id, MultiAdvCb cb,
                        tBTM_BLE_ADV_PARAMS* params,
                        std::vector<uint8_t> advertise_data,
                        std::vector<uint8_t> scan_response_data, int timeout_s,
                        MultiAdvCb timeout_cb) override {
    /* a temporary type for holding all the data needed in callbacks below*/
    struct CreatorParams {
      uint8_t inst_id;
      BleAdvertisingManagerImpl* self;
      MultiAdvCb cb;
      tBTM_BLE_ADV_PARAMS params;
      std::vector<uint8_t> advertise_data;
      std::vector<uint8_t> scan_response_data;
      int timeout_s;
      MultiAdvCb timeout_cb;
    };

    std::unique_ptr<CreatorParams> c;
    c.reset(new CreatorParams());

    c->self = this;
    c->cb = std::move(cb);
    c->params = *params;
    c->advertise_data = std::move(advertise_data);
    c->scan_response_data = std::move(scan_response_data);
    c->timeout_s = timeout_s;
    c->timeout_cb = std::move(timeout_cb);
    c->inst_id = advertiser_id;

    using c_type = std::unique_ptr<CreatorParams>;

    // this code is intentionally left formatted this way to highlight the
    // asynchronous flow
    // clang-format off
    c->self->SetParameters(c->inst_id, &c->params, Bind(
      [](c_type c, uint8_t status, int8_t tx_power) {
        if (status != 0) {
          LOG(ERROR) << "setting parameters failed, status: " << +status;
          c->cb.Run(status);
          return;
        }

        c->self->adv_inst[c->inst_id].tx_power = tx_power;

        BD_ADDR *rpa = &c->self->adv_inst[c->inst_id].own_address;
        c->self->GetHciInterface()->SetRandomAddress(c->inst_id, *rpa, Bind(
          [](c_type c, uint8_t status) {
            if (status != 0) {
              LOG(ERROR) << "setting random address failed, status: " << +status;
              c->cb.Run(status);
              return;
            }

            c->self->SetData(c->inst_id, false, std::move(c->advertise_data), Bind(
              [](c_type c, uint8_t status) {
                if (status != 0) {
                  LOG(ERROR) << "setting advertise data failed, status: " << +status;
                  c->cb.Run(status);
                  return;
                }

                c->self->SetData(c->inst_id, true, std::move(c->scan_response_data), Bind(
                  [](c_type c, uint8_t status) {
                    if (status != 0) {
                      LOG(ERROR) << "setting scan response data failed, status: " << +status;
                      c->cb.Run(status);
                      return;
                    }

                    c->self->Enable(c->inst_id, true, c->cb, c->timeout_s, std::move(c->timeout_cb));

                }, base::Passed(&c)));
            }, base::Passed(&c)));
        }, base::Passed(&c)));
    }, base::Passed(&c)));
    // clang-format on
  }

  void StartAdvertisingSet(IdTxPowerStatusCb cb, tBTM_BLE_ADV_PARAMS* params,
                           std::vector<uint8_t> advertise_data,
                           std::vector<uint8_t> scan_response_data,
                           tBLE_PERIODIC_ADV_PARAMS* periodic_params,
                           std::vector<uint8_t> periodic_data, int timeout_s,
                           RegisterCb timeout_cb) override {
    std::unique_ptr<CreatorParams> c;
    c.reset(new CreatorParams());

    c->self = this;
    c->cb = std::move(cb);
    c->params = *params;
    c->advertise_data = std::move(advertise_data);
    c->scan_response_data = std::move(scan_response_data);
    c->periodic_params = *periodic_params;
    c->periodic_data = std::move(periodic_data);
    c->timeout_s = timeout_s;
    c->timeout_cb = std::move(timeout_cb);


    // this code is intentionally left formatted this way to highlight the
    // asynchronous flow
    // clang-format off
    c->self->RegisterAdvertiser(Bind(
      [](c_type c, uint8_t advertiser_id, uint8_t status) {
        if (status != 0) {
          LOG(ERROR) << "registering advertiser failed, status: " << +status;
          c->cb.Run(0, 0, status);
          return;
        }

        c->inst_id = advertiser_id;

        c->self->SetParameters(c->inst_id, &c->params, Bind(
          [](c_type c, uint8_t status, int8_t tx_power) {
            if (status != 0) {
              c->self->Unregister(c->inst_id);
              LOG(ERROR) << "setting parameters failed, status: " << +status;
              c->cb.Run(0, 0, status);
              return;
            }

            c->self->adv_inst[c->inst_id].tx_power = tx_power;

            BD_ADDR *rpa = &c->self->adv_inst[c->inst_id].own_address;
            c->self->GetHciInterface()->SetRandomAddress(c->inst_id, *rpa, Bind(
              [](c_type c, uint8_t status) {
                if (status != 0) {
                  c->self->Unregister(c->inst_id);
                  LOG(ERROR) << "setting random address failed, status: " << +status;
                  c->cb.Run(0, 0, status);
                  return;
                }

                c->self->SetData(c->inst_id, false, std::move(c->advertise_data), Bind(
                  [](c_type c, uint8_t status) {
                    if (status != 0) {
                      c->self->Unregister(c->inst_id);
                      LOG(ERROR) << "setting advertise data failed, status: " << +status;
                      c->cb.Run(0, 0, status);
                      return;
                    }

                    c->self->SetData(c->inst_id, true, std::move(c->scan_response_data), Bind(
                      [](c_type c, uint8_t status) {
                        if (status != 0) {
                          c->self->Unregister(c->inst_id);
                          LOG(ERROR) << "setting scan response data failed, status: " << +status;
                          c->cb.Run(0, 0, status);
                          return;
                        }

                        if (c->periodic_params.enable) {
                          c->self->StartAdvertisingSetPeriodicPart(std::move(c));
                        } else {
                          c->self->StartAdvertisingSetFinish(std::move(c));
                        }
                    }, base::Passed(&c)));
                }, base::Passed(&c)));
            }, base::Passed(&c)));
        }, base::Passed(&c)));
    }, base::Passed(&c)));
    // clang-format on
  }

  void StartAdvertisingSetPeriodicPart(c_type c) {
    // this code is intentionally left formatted this way to highlight the
    // asynchronous flow
    // clang-format off
    c->self->SetPeriodicAdvertisingParameters(c->inst_id, &c->periodic_params, Bind(
      [](c_type c, uint8_t status) {
        if (status != 0) {
          c->self->Unregister(c->inst_id);
          LOG(ERROR) << "setting periodic parameters failed, status: " << +status;
          c->cb.Run(0, 0, status);
          return;
        }

        c->self->SetPeriodicAdvertisingData(c->inst_id, std::move(c->periodic_data), Bind(
          [](c_type c, uint8_t status) {
            if (status != 0) {
              c->self->Unregister(c->inst_id);
              LOG(ERROR) << "setting periodic parameters failed, status: " << +status;
              c->cb.Run(0, 0, status);
              return;
            }

            c->self->SetPeriodicAdvertisingEnable(c->inst_id, true, Bind(
              [](c_type c, uint8_t status) {
                if (status != 0) {
                  c->self->Unregister(c->inst_id);
                  LOG(ERROR) << "enabling periodic advertising failed, status: " << +status;
                  c->cb.Run(0, 0, status);
                  return;
                }

                c->self->StartAdvertisingSetFinish(std::move(c));

              }, base::Passed(&c)));
        }, base::Passed(&c)));
    }, base::Passed(&c)));
    // clang-format on
  }

  void StartAdvertisingSetFinish(c_type c) {
    uint8_t inst_id = c->inst_id;
    int timeout_s = c->timeout_s;
    RegisterCb timeout_cb = std::move(c->timeout_cb);
    BleAdvertisingManagerImpl* self = c->self;
    MultiAdvCb enable_cb = Bind(
        [](c_type c, uint8_t status) {
          if (status != 0) {
            c->self->Unregister(c->inst_id);
            LOG(ERROR) << "enabling advertiser failed, status: " << +status;
            c->cb.Run(0, 0, status);
            return;
          }
          int8_t tx_power = c->self->adv_inst[c->inst_id].tx_power;
          c->cb.Run(c->inst_id, tx_power, status);
        },
        base::Passed(&c));

    self->Enable(inst_id, true, std::move(enable_cb), timeout_s,
                 Bind(std::move(timeout_cb), inst_id));
  }

  void EnableWithTimerCb(uint8_t inst_id, MultiAdvCb enable_cb, int timeout_s,
                         MultiAdvCb timeout_cb, uint8_t status) {
    VLOG(1) << __func__ << " inst_id: " << +inst_id;
    AdvertisingInstance* p_inst = &adv_inst[inst_id];

    // Run the regular enable callback
    enable_cb.Run(status);

    p_inst->timeout_s = timeout_s;
    p_inst->timeout_timer = alarm_new("btm_ble.adv_timeout");

    base::Closure cb = Bind(&BleAdvertisingManagerImpl::Enable,
                            base::Unretained(this), inst_id, 0 /* disable */,
                            std::move(timeout_cb), 0, base::Bind(DoNothing));

    // schedule disable when the timeout passes
    alarm_set_closure_on_queue(FROM_HERE, p_inst->timeout_timer,
                               timeout_s * 1000, std::move(cb),
                               btu_general_alarm_queue);
  }

  void Enable(uint8_t inst_id, bool enable, MultiAdvCb cb, int timeout_s,
              MultiAdvCb timeout_cb) {
    VLOG(1) << __func__ << " inst_id: " << +inst_id;
    if (inst_id >= inst_count) {
      LOG(ERROR) << "bad instance id " << +inst_id;
      return;
    }

    AdvertisingInstance* p_inst = &adv_inst[inst_id];
    VLOG(1) << __func__ << " enable: " << enable << ", timeout: " << +timeout_s;
    if (!p_inst->in_use) {
      LOG(ERROR) << "Invalid or no active instance";
      cb.Run(BTM_BLE_MULTI_ADV_FAILURE);
      return;
    }

    if (enable && timeout_s) {
      GetHciInterface()->Enable(
          enable, p_inst->inst_id, 0x0000, 0x00,
          Bind(&BleAdvertisingManagerImpl::EnableWithTimerCb,
               base::Unretained(this), inst_id, std::move(cb), timeout_s,
               std::move(timeout_cb)));

    } else {
      if (p_inst->timeout_timer) {
        alarm_cancel(p_inst->timeout_timer);
        alarm_free(p_inst->timeout_timer);
        p_inst->timeout_timer = nullptr;
      }

      GetHciInterface()->Enable(enable, p_inst->inst_id, 0x0000, 0x00, cb);
    }
  }

  void SetParameters(uint8_t inst_id, tBTM_BLE_ADV_PARAMS* p_params,
                     ParametersCb cb) override {
    VLOG(1) << __func__ << " inst_id: " << +inst_id;
    if (inst_id >= inst_count) {
      LOG(ERROR) << "bad instance id " << +inst_id;
      return;
    }

    AdvertisingInstance* p_inst = &adv_inst[inst_id];
    if (!p_inst->in_use) {
      LOG(ERROR) << "adv instance not in use" << +inst_id;
      cb.Run(BTM_BLE_MULTI_ADV_FAILURE, 0);
      return;
    }

    // TODO: disable only if was enabled, currently no use scenario needs that,
    // we always set parameters before enabling
    // GetHciInterface()->Enable(false, inst_id, Bind(DoNothing));
    p_inst->advertising_event_properties =
        p_params->advertising_event_properties;
    p_inst->tx_power = p_params->tx_power;
    BD_ADDR peer_address = {0, 0, 0, 0, 0, 0};

    GetHciInterface()->SetParameters(
        p_inst->inst_id, p_params->advertising_event_properties,
        p_params->adv_int_min, p_params->adv_int_max, p_params->channel_map,
        p_inst->own_address_type, p_inst->own_address, 0x00, peer_address,
        p_params->adv_filter_policy, p_inst->tx_power,
        p_params->primary_advertising_phy, 0x01,
        p_params->secondary_advertising_phy, 0x01 /* TODO: proper SID */,
        p_params->scan_request_notification_enable, cb);

    // TODO: re-enable only if it was enabled, properly call
    // SetParamsCallback
    // currently no use scenario needs that
    // GetHciInterface()->Enable(true, inst_id, BTM_BleUpdateAdvInstParamCb);
  }

  void SetData(uint8_t inst_id, bool is_scan_rsp, std::vector<uint8_t> data,
               MultiAdvCb cb) override {
    VLOG(1) << __func__ << " inst_id: " << +inst_id;
    if (inst_id >= inst_count) {
      LOG(ERROR) << "bad instance id " << +inst_id;
      return;
    }

    AdvertisingInstance* p_inst = &adv_inst[inst_id];
    VLOG(1) << "is_scan_rsp = " << is_scan_rsp;

    if (!is_scan_rsp &&
        is_legacy_connectable(p_inst->advertising_event_properties)) {
      uint8_t flags_val = BTM_GENERAL_DISCOVERABLE;

      if (p_inst->timeout_s) flags_val = BTM_LIMITED_DISCOVERABLE;

      std::vector<uint8_t> flags;
      flags.push_back(2);  // length
      flags.push_back(HCI_EIR_FLAGS_TYPE);
      flags.push_back(flags_val);

      data.insert(data.begin(), flags.begin(), flags.end());
    }

    // Find and fill TX Power with the correct value
    if (data.size()) {
      size_t i = 0;
      while (i < data.size()) {
        uint8_t type = data[i + 1];
        if (type == HCI_EIR_TX_POWER_LEVEL_TYPE) {
          data[i + 2] = adv_inst[inst_id].tx_power;
        }
        i += data[i] + 1;
      }
    }

    VLOG(1) << "data is: " << base::HexEncode(data.data(), data.size());
    DivideAndSendData(
        inst_id, data, cb,
        base::Bind(&BleAdvertisingManagerImpl::SetDataAdvDataSender,
                   base::Unretained(this), is_scan_rsp));
  }

  void SetDataAdvDataSender(uint8_t is_scan_rsp, uint8_t inst_id,
                            uint8_t operation, uint8_t length, uint8_t* data,
                            MultiAdvCb cb) {
    if (is_scan_rsp)
      GetHciInterface()->SetScanResponseData(inst_id, operation, 0x01, length,
                                             data, cb);
    else
      GetHciInterface()->SetAdvertisingData(inst_id, operation, 0x01, length,
                                            data, cb);
  }

  using DataSender = base::Callback<void(
      uint8_t /*inst_id*/, uint8_t /* operation */, uint8_t /* length */,
      uint8_t* /* data */, MultiAdvCb /* done */)>;

  void DivideAndSendData(int inst_id, std::vector<uint8_t> data,
                         MultiAdvCb done_cb, DataSender sender) {
    DivideAndSendDataRecursively(true, inst_id, std::move(data), 0,
                                 std::move(done_cb), std::move(sender), 0);
  }

  static void DivideAndSendDataRecursively(bool isFirst, int inst_id,
                                           std::vector<uint8_t> data,
                                           int offset, MultiAdvCb done_cb,
                                           DataSender sender, uint8_t status) {
    constexpr uint8_t INTERMEDIATE =
        0x00;                        // Intermediate fragment of fragmented data
    constexpr uint8_t FIRST = 0x01;  // First fragment of fragmented data
    constexpr uint8_t LAST = 0x02;   // Last fragment of fragmented data
    constexpr uint8_t COMPLETE = 0x03;  // Complete extended advertising data

    int dataSize = (int)data.size();
    if (status != 0 || (!isFirst && offset == dataSize)) {
      /* if we got error writing data, or reached the end of data */
      done_cb.Run(status);
      return;
    }

    bool moreThanOnePacket = dataSize - offset > ADV_DATA_LEN_MAX;
    uint8_t operation = isFirst ? moreThanOnePacket ? FIRST : COMPLETE
                                : moreThanOnePacket ? INTERMEDIATE : LAST;
    int length = moreThanOnePacket ? ADV_DATA_LEN_MAX : dataSize - offset;
    int newOffset = offset + length;

    sender.Run(
        inst_id, operation, length, data.data() + offset,
        Bind(&BleAdvertisingManagerImpl::DivideAndSendDataRecursively, false,
             inst_id, std::move(data), newOffset, std::move(done_cb), sender));
  }

  void SetPeriodicAdvertisingParameters(uint8_t inst_id,
                                        tBLE_PERIODIC_ADV_PARAMS* params,
                                        MultiAdvCb cb) override {
    VLOG(1) << __func__ << " inst_id: " << +inst_id;

    GetHciInterface()->SetPeriodicAdvertisingParameters(
        inst_id, params->min_interval, params->max_interval,
        params->periodic_advertising_properties, cb);
  }

  void SetPeriodicAdvertisingData(uint8_t inst_id, std::vector<uint8_t> data,
                                  MultiAdvCb cb) override {
    VLOG(1) << __func__ << " inst_id: " << +inst_id;

    VLOG(1) << "data is: " << base::HexEncode(data.data(), data.size());

    DivideAndSendData(
        inst_id, data, cb,
        base::Bind(&BleAdvertiserHciInterface::SetPeriodicAdvertisingData,
                   base::Unretained(GetHciInterface())));
  }

  void SetPeriodicAdvertisingEnable(uint8_t inst_id, uint8_t enable,
                                    MultiAdvCb cb) override {
    VLOG(1) << __func__ << " inst_id: " << +inst_id << ", enable: " << +enable;

    GetHciInterface()->SetPeriodicAdvertisingEnable(enable, inst_id, cb);
  }

  void Unregister(uint8_t inst_id) override {
    AdvertisingInstance* p_inst = &adv_inst[inst_id];

    VLOG(1) << __func__ << " inst_id: " << +inst_id;
    if (inst_id >= inst_count) {
      LOG(ERROR) << "bad instance id " << +inst_id;
      return;
    }

    // TODO(jpawlowski): only disable when enabled or enabling
    GetHciInterface()->Enable(false, inst_id, 0x00, 0x00, Bind(DoNothing));

    alarm_cancel(p_inst->adv_raddr_timer);
    p_inst->in_use = false;
  }

  void OnAdvertisingSetTerminated(
      uint8_t status, uint8_t advertising_handle, uint16_t connection_handle,
      uint8_t num_completed_extended_adv_events) override {
    AdvertisingInstance* p_inst = &adv_inst[advertising_handle];
    VLOG(1) << __func__ << "status: 0x" << std::hex << +status
            << ", advertising_handle: 0x" << std::hex << +advertising_handle
            << ", connection_handle: 0x" << std::hex << +connection_handle;

#if (BLE_PRIVACY_SPT == TRUE)
    if (BTM_BleLocalPrivacyEnabled() &&
        advertising_handle <= BTM_BLE_MULTI_ADV_MAX) {
      btm_acl_update_conn_addr(connection_handle, p_inst->own_address);
    }
#endif

    VLOG(1) << "reneabling advertising";

    if (p_inst->in_use == true) {
      // TODO(jpawlowski): we don't really allow to do directed advertising
      // right now. This should probably be removed, check with Andre.
      if ((p_inst->advertising_event_properties & 0x0C) ==
          0 /* directed advertising bits not set
      */) {
        GetHciInterface()->Enable(true, advertising_handle, 0x00, 0x00,
                                  Bind(DoNothing));
      } else {
        /* mark directed adv as disabled if adv has been stopped */
        p_inst->in_use = false;
      }
    }
  }

 private:
  BleAdvertiserHciInterface* GetHciInterface() { return hci_interface; }

  BleAdvertiserHciInterface* hci_interface = nullptr;
  std::vector<AdvertisingInstance> adv_inst;
  uint8_t inst_count;
};

BleAdvertisingManager* instance;
}

void BleAdvertisingManager::Initialize(BleAdvertiserHciInterface* interface) {
  instance = new BleAdvertisingManagerImpl(interface);
}

BleAdvertisingManager* BleAdvertisingManager::Get() {
  CHECK(instance);
  return instance;
};

void BleAdvertisingManager::CleanUp() {
  delete instance;
  instance = nullptr;
};

void btm_ble_adv_raddr_timer_timeout(void* data) {
  ((BleAdvertisingManagerImpl*)BleAdvertisingManager::Get())
      ->ConfigureRpa((AdvertisingInstance*)data);
}

/**
 * This function initialize the advertising manager.
 **/
void btm_ble_adv_init() {
  BleAdvertiserHciInterface::Initialize();
  BleAdvertisingManager::Initialize(BleAdvertiserHciInterface::Get());
  BleAdvertiserHciInterface::Get()->SetAdvertisingEventObserver(
      (BleAdvertisingManagerImpl*)BleAdvertisingManager::Get());

  if (BleAdvertiserHciInterface::Get()->QuirkAdvertiserZeroHandle()) {
    // If handle 0 can't be used, register advertiser for it, but never use it.
    BleAdvertisingManager::Get()->RegisterAdvertiser(Bind(DoNothing2));
  }
}

/*******************************************************************************
 *
 * Function         btm_ble_multi_adv_cleanup
 *
 * Description      This function cleans up multi adv control block.
 *
 * Parameters
 * Returns          void
 *
 ******************************************************************************/
void btm_ble_multi_adv_cleanup(void) {
  BleAdvertisingManager::CleanUp();
  BleAdvertiserHciInterface::CleanUp();
}
